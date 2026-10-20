/*
 * Thin JNI layer over core/gbah_core: owns ONE emulator instance, its emulation
 * thread, AAudio output and the ANativeWindow renderer.
 *
 * Threading:
 *   - emu thread (pthread, created by start()): per frame, under g.mtx:
 *     set input, run frame, read audio, draw into the window. Then, outside the
 *     lock, a BLOCKING AAudio write: the audio device clock paces the emulation
 *     (same idea as the desktop). Without audio it falls back to a 59.7275 Hz timer.
 *   - any other thread (UI): state/sav/load calls take g.mtx, so they interleave
 *     between frames. setKeys is a lock-free atomic.
 * No allocation per frame: all buffers are static.
 */
#include <jni.h>

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "app_core.h"
#include "gbah_core.h"

#define TAG "gbah"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

#define AUDIO_RATE 48000
#define ABUF_FRAMES 4096
#define GBA_FPS 59.7275

static struct {
	gbah_core *core;            /* g.mtx */
	pthread_mutex_t mtx;
	pthread_mutex_t win_mtx;    /* protects win */
	ANativeWindow *win;
	pthread_t thread;
	int thread_on;              /* touched only by start()/stop() callers (UI thread) */
	atomic_int run;
	atomic_uint keys;
	atomic_int hold;            /* the link engine froze the game (handed to the PC, conflict...) */
	AAudioStream *stream;       /* emu thread only while running; opened by start() before the thread */
	int32_t stream_buf_frames, stream_burst, stream_max_buf;
	int32_t xruns;
	int16_t abuf[ABUF_FRAMES * 2];
	char sha1[41];
} g = { .mtx = PTHREAD_MUTEX_INITIALIZER, .win_mtx = PTHREAD_MUTEX_INITIALIZER };

static core_host_ctx g_ctx = { .core = &g.core, .mtx = &g.mtx, .hold = &g.hold };
core_host_ctx *appcore_ctx(void) { return &g_ctx; }

/* ------------------------------------------------------------ rendering */

static void draw(const uint32_t *fb)
{
	ANativeWindow_Buffer b;
	pthread_mutex_lock(&g.win_mtx);
	if (g.win && ANativeWindow_lock(g.win, &b, NULL) == 0) {
		/* Integer, nearest-neighbour upscale, centered. The Kotlin side makes the
		 * surface exactly 240k x 160k px, so k is derived from the buffer size. */
		int k = b.width / GBAH_WIDTH;
		int k2 = b.height / GBAH_HEIGHT;
		int x, y, i;
		if (k2 < k) k = k2;
		if (k < 1) k = 1;
		{
			int out_w = GBAH_WIDTH * k, out_h = GBAH_HEIGHT * k;
			int ox = (b.width - out_w) / 2, oy = (b.height - out_h) / 2;
			int cw = out_w, ch = out_h; /* clipped sizes (only when buffer < 240x160) */
			int sx0 = 0, sy0 = 0;
			if (ox < 0) { sx0 = -ox; cw += ox; ox = 0; }
			if (oy < 0) { sy0 = -oy; ch += oy; oy = 0; }
			if (cw > b.width) cw = b.width;
			if (ch > b.height) ch = b.height;
			if (b.format == WINDOW_FORMAT_RGBX_8888 || b.format == WINDOW_FORMAT_RGBA_8888) {
				uint32_t *dst = (uint32_t *)b.bits + (size_t)oy * b.stride + ox;
				for (y = 0; y < ch;) {
					int sy = (y + sy0) / k;
					const uint32_t *src = fb + (size_t)sy * GBAH_WIDTH;
					uint32_t *row = dst + (size_t)y * b.stride;
					if (k == 1) {
						memcpy(row, src + sx0, (size_t)cw * 4);
					} else {
						for (x = 0; x < cw; x++) row[x] = src[(x + sx0) / k];
					}
					y++;
					/* the remaining rows of this source line are plain copies */
					for (i = 1; i < k && y < ch && ((y + sy0) / k) == sy; i++, y++)
						memcpy(dst + (size_t)y * b.stride, row, (size_t)cw * 4);
				}
			}
		}
		ANativeWindow_unlockAndPost(g.win);
	}
	pthread_mutex_unlock(&g.win_mtx);
}

/* ---------------------------------------------------------------- audio */

static void audio_close(void)
{
	if (g.stream) {
		AAudioStream_requestStop(g.stream);
		AAudioStream_close(g.stream);
		g.stream = NULL;
	}
}

static int audio_open(void)
{
	AAudioStreamBuilder *bld;
	int32_t cap;
	if (AAudio_createStreamBuilder(&bld) != AAUDIO_OK) return -1;
	AAudioStreamBuilder_setDirection(bld, AAUDIO_DIRECTION_OUTPUT);
	AAudioStreamBuilder_setPerformanceMode(bld, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
	AAudioStreamBuilder_setSharingMode(bld, AAUDIO_SHARING_MODE_SHARED);
	AAudioStreamBuilder_setFormat(bld, AAUDIO_FORMAT_PCM_I16);
	AAudioStreamBuilder_setChannelCount(bld, 2);
	AAudioStreamBuilder_setSampleRate(bld, AUDIO_RATE); /* system resamples if the HW differs */
	if (__builtin_available(android 28, *)) /* minSdk is 26 */
		AAudioStreamBuilder_setUsage(bld, AAUDIO_USAGE_GAME);
	if (AAudioStreamBuilder_openStream(bld, &g.stream) != AAUDIO_OK) {
		AAudioStreamBuilder_delete(bld);
		g.stream = NULL;
		return -1;
	}
	AAudioStreamBuilder_delete(bld);
	g.stream_burst = AAudioStream_getFramesPerBurst(g.stream);
	cap = AAudioStream_getBufferCapacityInFrames(g.stream);
	/* ~1.6 GBA frames (803 samples each at 48 kHz) of buffer; grows on underruns. */
	g.stream_max_buf = cap < 2048 ? cap : 2048;
	g.stream_buf_frames = 1280 < g.stream_max_buf ? 1280 : g.stream_max_buf;
	AAudioStream_setBufferSizeInFrames(g.stream, g.stream_buf_frames);
	g.xruns = 0;
	if (AAudioStream_requestStart(g.stream) != AAUDIO_OK) {
		audio_close();
		return -1;
	}
	LOGI("AAudio: rate %d burst %d buffer %d/%d", AAudioStream_getSampleRate(g.stream),
	     g.stream_burst, g.stream_buf_frames, cap);
	return 0;
}

/* Blocking write; returns 0 if paced by audio, -1 if the caller must pace by timer. */
static int audio_write(size_t frames)
{
	const int16_t *p = g.abuf;
	int32_t x;
	if (!g.stream) return -1;
	while (frames) {
		aaudio_result_t r = AAudioStream_write(g.stream, p, (int32_t)frames, 100 * 1000000LL);
		if (r < 0) { /* disconnected (headphones...) or other failure: try to reopen once */
			LOGE("AAudio write error %d, reopening", r);
			audio_close();
			if (audio_open()) return -1;
			return 0;
		}
		if (r == 0) { LOGE("AAudio write timeout"); return 0; }
		p += (size_t)r * 2;
		frames -= (size_t)r;
	}
	x = AAudioStream_getXRunCount(g.stream);
	if (x > g.xruns && g.stream_buf_frames < g.stream_max_buf) {
		g.stream_buf_frames += g.stream_burst;
		if (g.stream_buf_frames > g.stream_max_buf) g.stream_buf_frames = g.stream_max_buf;
		AAudioStream_setBufferSizeInFrames(g.stream, g.stream_buf_frames);
	}
	g.xruns = x;
	return 0;
}

/* ------------------------------------------------------------ emu thread */

static void ts_add_ns(struct timespec *t, long ns)
{
	t->tv_nsec += ns;
	while (t->tv_nsec >= 1000000000L) { t->tv_nsec -= 1000000000L; t->tv_sec++; }
}

static void *emu_main(void *arg)
{
	struct timespec next;
	const long period = (long)(1e9 / GBA_FPS);
	(void)arg;
	clock_gettime(CLOCK_MONOTONIC, &next);
	while (atomic_load(&g.run)) {
		size_t n;
		pthread_mutex_lock(&g.mtx);
		if (atomic_load_explicit(&g.hold, memory_order_relaxed)) {   /* frozen by the link engine: no frame */
			struct timespec nap = { 0, 8000000L };
			pthread_mutex_unlock(&g.mtx);
			nanosleep(&nap, NULL);
			clock_gettime(CLOCK_MONOTONIC, &next);
			continue;
		}
		gbah_set_input(g.core, atomic_load_explicit(&g.keys, memory_order_relaxed));
		gbah_run_frame(g.core);
		n = gbah_audio_read(g.core, g.abuf, ABUF_FRAMES);
		draw(gbah_framebuffer(g.core));
		pthread_mutex_unlock(&g.mtx);

		if (!(g.stream && n && audio_write(n) == 0)) {
			ts_add_ns(&next, period);
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
		} else {
			clock_gettime(CLOCK_MONOTONIC, &next); /* keep the timer path ready, no drift */
		}
	}
	return NULL;
}

/* -------------------------------------------------------------- helpers */

static int write_file_atomic(const char *path, const void *data, size_t n)
{
	char tmp[1024];
	if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -1;
	if (gbah_write_file(tmp, data, n)) return -1;
	return rename(tmp, path);
}

static int read_fd_all(int fd, void **out, size_t *size)
{
	struct stat st;
	size_t cap, n = 0;
	char *buf;
	*out = NULL;
	cap = (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) ? (size_t)st.st_size : (size_t)(4u << 20);
	buf = malloc(cap);
	if (!buf) return -1;
	(void)lseek(fd, 0, SEEK_SET);
	for (;;) {
		ssize_t r;
		if (n == cap) {
			char *nb;
			if (cap >= (64u << 20)) { free(buf); return -1; } /* GBA ROMs are <= 32 MiB */
			nb = realloc(buf, cap * 2);
			if (!nb) { free(buf); return -1; }
			buf = nb; cap *= 2;
		}
		r = read(fd, buf + n, cap - n);
		if (r < 0) { if (errno == EINTR) continue; free(buf); return -1; }
		if (r == 0) break;
		n += (size_t)r;
	}
	*out = buf;
	*size = n;
	return n ? 0 : -1;
}

static void stop_locked_free(void)
{
	link_jni_running(0);
	if (g.thread_on) {
		atomic_store(&g.run, 0);
		pthread_join(g.thread, NULL);
		g.thread_on = 0;
	}
	audio_close();
}

#define J(ret, name) JNIEXPORT ret JNICALL Java_io_gbahandoff_Native_##name

/* ------------------------------------------------------------------ JNI */

J(jboolean, create)(JNIEnv *env, jclass cls)
{
	(void)env; (void)cls;
	if (g.core) return JNI_TRUE;
	g.core = gbah_create(AUDIO_RATE);
	g.sha1[0] = 0;
	atomic_store(&g.keys, 0);
	atomic_store(&g.hold, 0);
	return g.core ? JNI_TRUE : JNI_FALSE;
}

J(void, destroy)(JNIEnv *env, jclass cls)
{
	(void)env; (void)cls;
	stop_locked_free();
	pthread_mutex_lock(&g.win_mtx);
	if (g.win) { ANativeWindow_release(g.win); g.win = NULL; }
	pthread_mutex_unlock(&g.win_mtx);
	pthread_mutex_lock(&g.mtx);
	gbah_destroy(g.core);
	g.core = NULL;
	atomic_store(&g.hold, 0);
	pthread_mutex_unlock(&g.mtx);
	link_jni_game_unloaded();
}

static jint after_rom_load(int r)
{
	if (r == 0) gbah_rom_sha1_hex(g.core, g.sha1);
	return r;
}

J(jint, loadRomBytes)(JNIEnv *env, jclass cls, jbyteArray arr)
{
	jsize n = (*env)->GetArrayLength(env, arr);
	void *p;
	jint r;
	(void)cls;
	if (!g.core) return -100;
	p = (*env)->GetPrimitiveArrayCritical(env, arr, NULL);
	if (!p) return -101;
	pthread_mutex_lock(&g.mtx);
	r = after_rom_load(gbah_load_rom_buffer(g.core, p, (size_t)n));
	pthread_mutex_unlock(&g.mtx);
	(*env)->ReleasePrimitiveArrayCritical(env, arr, p, JNI_ABORT);
	if (r == 0) link_jni_game_loaded();
	return r;
}

/* Reads the whole fd in native memory (no Java heap use for 32 MiB ROMs). Does not close fd. */
J(jint, loadRomFd)(JNIEnv *env, jclass cls, jint fd)
{
	void *buf;
	size_t n;
	jint r;
	(void)env; (void)cls;
	if (!g.core) return -100;
	if (read_fd_all(fd, &buf, &n)) return -101;
	pthread_mutex_lock(&g.mtx);
	r = after_rom_load(gbah_load_rom_buffer(g.core, buf, n));
	pthread_mutex_unlock(&g.mtx);
	free(buf);
	if (r == 0) link_jni_game_loaded();
	return r;
}

J(jstring, romSha1)(JNIEnv *env, jclass cls)
{
	(void)cls;
	return g.sha1[0] ? (*env)->NewStringUTF(env, g.sha1) : NULL;
}

/* Cold start with an existing .sav (before running). */
J(jint, loadSavFile)(JNIEnv *env, jclass cls, jstring jpath)
{
	const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
	void *buf = NULL;
	size_t n = 0;
	jint r = -1;
	(void)cls;
	if (!path || !g.core) return -100;
	if (gbah_read_file(path, &buf, &n) == 0) {
		pthread_mutex_lock(&g.mtx);
		r = gbah_load_sav_buffer(g.core, buf, n);
		pthread_mutex_unlock(&g.mtx);
		gbah_free(buf);
	}
	(*env)->ReleaseStringUTFChars(env, jpath, path);
	return r;
}

/* Resume: state (+ optional sav, jsav may be null/missing file) through gbah_handoff_restore. */
J(jint, restoreFiles)(JNIEnv *env, jclass cls, jstring jstate, jstring jsav)
{
	const char *sp = (*env)->GetStringUTFChars(env, jstate, NULL);
	const char *vp = jsav ? (*env)->GetStringUTFChars(env, jsav, NULL) : NULL;
	void *st = NULL, *sv = NULL;
	size_t stn = 0, svn = 0;
	jint r = -1;
	(void)cls;
	if (!sp || !g.core) return -100;
	if (gbah_read_file(sp, &st, &stn) == 0) {
		if (vp) (void)gbah_read_file(vp, &sv, &svn); /* missing .sav is fine */
		pthread_mutex_lock(&g.mtx);
		r = gbah_handoff_restore(g.core, st, stn, sv, svn);
		pthread_mutex_unlock(&g.mtx);
	}
	gbah_free(st);
	gbah_free(sv);
	(*env)->ReleaseStringUTFChars(env, jstate, sp);
	if (vp) (*env)->ReleaseStringUTFChars(env, jsav, vp);
	return r;
}

/* Byte-array variants (what the sync protocol will use). sav may be null. */
J(jint, restore)(JNIEnv *env, jclass cls, jbyteArray state, jbyteArray sav)
{
	jsize stn = (*env)->GetArrayLength(env, state);
	jsize svn = sav ? (*env)->GetArrayLength(env, sav) : 0;
	void *st, *sv = NULL;
	jint r;
	(void)cls;
	if (!g.core) return -100;
	st = (*env)->GetPrimitiveArrayCritical(env, state, NULL);
	if (!st) return -101;
	if (sav) {
		/* Two critical sections must not nest with other JNI calls; both are plain pointers. */
		sv = (*env)->GetPrimitiveArrayCritical(env, sav, NULL);
		if (!sv) { (*env)->ReleasePrimitiveArrayCritical(env, state, st, JNI_ABORT); return -101; }
	}
	pthread_mutex_lock(&g.mtx);
	r = gbah_handoff_restore(g.core, st, (size_t)stn, sv, (size_t)svn);
	pthread_mutex_unlock(&g.mtx);
	if (sv) (*env)->ReleasePrimitiveArrayCritical(env, sav, sv, JNI_ABORT);
	(*env)->ReleasePrimitiveArrayCritical(env, state, st, JNI_ABORT);
	return r;
}

static jbyteArray to_java(JNIEnv *env, void *p, size_t n)
{
	jbyteArray a;
	if (!p || !n) return NULL;
	a = (*env)->NewByteArray(env, (jsize)n);
	if (a) (*env)->SetByteArrayRegion(env, a, 0, (jsize)n, p);
	return a;
}

J(jbyteArray, exportState)(JNIEnv *env, jclass cls)
{
	void *p = NULL;
	size_t n;
	jbyteArray a;
	(void)cls;
	if (!g.core) return NULL;
	pthread_mutex_lock(&g.mtx);
	n = gbah_state_save(g.core, &p);
	pthread_mutex_unlock(&g.mtx);
	a = to_java(env, p, n);
	gbah_free(p);
	return a;
}

/* null when the game has not created save data yet. */
J(jbyteArray, exportSav)(JNIEnv *env, jclass cls)
{
	void *p = NULL;
	size_t n;
	jbyteArray a;
	(void)cls;
	if (!g.core) return NULL;
	pthread_mutex_lock(&g.mtx);
	n = gbah_sram_export(g.core, &p);
	pthread_mutex_unlock(&g.mtx);
	a = to_java(env, p, n);
	gbah_free(p);
	return a;
}

/* Files, written atomically (tmp + rename). Return 0 ok; for sav 1 = nothing to save yet. */
J(jint, saveStateFile)(JNIEnv *env, jclass cls, jstring jpath)
{
	const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
	void *p = NULL;
	size_t n;
	jint r = -1;
	(void)cls;
	if (!path || !g.core) return -100;
	pthread_mutex_lock(&g.mtx);
	n = gbah_state_save(g.core, &p);
	pthread_mutex_unlock(&g.mtx);
	if (n) r = write_file_atomic(path, p, n);
	gbah_free(p);
	(*env)->ReleaseStringUTFChars(env, jpath, path);
	return r;
}

J(jint, saveSavFile)(JNIEnv *env, jclass cls, jstring jpath)
{
	const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
	void *p = NULL;
	size_t n;
	jint r;
	(void)cls;
	if (!path || !g.core) return -100;
	pthread_mutex_lock(&g.mtx);
	n = gbah_sram_export(g.core, &p);
	pthread_mutex_unlock(&g.mtx);
	r = n ? write_file_atomic(path, p, n) : 1;
	gbah_free(p);
	(*env)->ReleaseStringUTFChars(env, jpath, path);
	return r;
}

J(void, setSurface)(JNIEnv *env, jclass cls, jobject surface)
{
	ANativeWindow *nw = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
	(void)cls;
	if (nw) /* 0,0 keeps the size set by SurfaceHolder.setFixedSize */
		ANativeWindow_setBuffersGeometry(nw, 0, 0, WINDOW_FORMAT_RGBX_8888);
	pthread_mutex_lock(&g.win_mtx);
	if (g.win) ANativeWindow_release(g.win);
	g.win = nw;
	pthread_mutex_unlock(&g.win_mtx);
}

J(void, setKeys)(JNIEnv *env, jclass cls, jint keys)
{
	(void)env; (void)cls;
	atomic_store_explicit(&g.keys, (unsigned)keys & GBAH_KEY_MASK, memory_order_relaxed);
}

/* Returns true if running. Idempotent. Audio failing is not an error (timer pacing). */
J(jboolean, start)(JNIEnv *env, jclass cls)
{
	(void)env; (void)cls;
	if (!g.core || !g.sha1[0]) return JNI_FALSE;
	if (g.thread_on) return JNI_TRUE;
	if (audio_open()) LOGE("no AAudio, pacing by timer");
	atomic_store(&g.run, 1);
	if (pthread_create(&g.thread, NULL, emu_main, NULL)) {
		atomic_store(&g.run, 0);
		audio_close();
		return JNI_FALSE;
	}
	g.thread_on = 1;
	link_jni_running(1);
	return JNI_TRUE;
}

J(void, stop)(JNIEnv *env, jclass cls)
{
	(void)env; (void)cls;
	stop_locked_free();
}

J(jint, frameCounter)(JNIEnv *env, jclass cls)
{
	(void)env; (void)cls;
	return g.core ? (jint)gbah_frame_counter(g.core) : 0;
}

/* ---- store-backed resume / save (the link engine owns the games directory) ---- */

/* Resume the stored progress of the loaded game: 0 restored, 1 nothing stored, <0 error. */
J(jint, resumeStored)(JNIEnv *env, jclass cls)
{
	phone_link *p = link_jni_engine();
	(void)env; (void)cls;
	return p ? phone_resume_stored(p) : -100;
}

/* Synchronous save (onPause): 0 saved, 1 skipped (nothing new / game is on the PC), <0 error. */
J(jint, saveNow)(JNIEnv *env, jclass cls)
{
	phone_link *p = link_jni_engine();
	(void)env; (void)cls;
	return p ? phone_save_now(p) : -100;
}
