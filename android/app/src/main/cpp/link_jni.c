/*
 * JNI glue for the phone side of the cable link. All the logic (session, sync decisions, store,
 * autosave, ROM transfer) lives in phone_link.c, which has no JNI in it and is exercised on the
 * host by tests/phone_link_test.c; this file only creates the engine, forwards the UI calls and
 * gives the engine its two Android-only abilities: reading a ROM through a content:// URI (a
 * static Kotlin method returning a detached fd) and the emulator hooks from jni_bridge.c.
 *
 * The UI polls phone_ui_text(); there are no callbacks into Java except the ROM fd one.
 */
#include <jni.h>

#include <android/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_core.h"
#include "gbah_sync.h"
#include "gbahp_net.h"

#define TAG "gbah-link"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static phone_link *g_link;
static JavaVM *g_vm;
static jclass g_client_cls;      /* global ref to LinkClient */
static jmethodID g_open_rom;     /* static int openRomFd(String) */

phone_link *link_jni_engine(void) { return g_link; }
void link_jni_game_loaded(void) { if (g_link) phone_game_loaded(g_link); }
void link_jni_game_unloaded(void) { if (g_link) phone_game_unloaded(g_link); }
void link_jni_running(int running) { if (g_link) phone_set_running(g_link, running); }

/* Called on the link thread: attaches it to the VM just for the call. */
static int jni_open_rom(void *u, const char *source)
{
	JNIEnv *env = NULL;
	int attached = 0, fd = -1;
	jstring js;
	(void)u;
	if (!g_vm || !g_client_cls || !g_open_rom) return -1;
	if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
		if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return -1;
		attached = 1;
	}
	js = (*env)->NewStringUTF(env, source);
	if (js) {
		fd = (*env)->CallStaticIntMethod(env, g_client_cls, g_open_rom, js);
		if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); fd = -1; }
		(*env)->DeleteLocalRef(env, js);
	}
	if (attached) (*g_vm)->DetachCurrentThread(g_vm);
	return fd;
}


static int sha_from_jstring(JNIEnv *env, jstring js, uint8_t sha[20])
{
	const char *s = js ? (*env)->GetStringUTFChars(env, js, NULL) : NULL;
	int rc = -1;
	if (s) {
		rc = strlen(s) == 40 ? gbah_sha1_from_hex(s, sha) : -1;
		(*env)->ReleaseStringUTFChars(env, js, s);
	}
	return rc;
}

#define J(ret, name) JNIEXPORT ret JNICALL Java_io_gbahandoff_LinkClient_##name

/* Creates the engine once per process (idempotent). */
J(void, nativeInit)(JNIEnv *env, jclass cls, jstring gamesDir, jstring romsDir, jbyteArray id, jstring name)
{
	phone_cfg cfg;
	const char *gd, *rd, *nm;
	if (g_link) return;
	memset(&cfg, 0, sizeof cfg);
	(*env)->GetJavaVM(env, &g_vm);
	g_client_cls = (jclass)(*env)->NewGlobalRef(env, cls);
	g_open_rom = (*env)->GetStaticMethodID(env, cls, "openRomFd", "(Ljava/lang/String;)I");
	if (!g_open_rom) (*env)->ExceptionClear(env);
	if (id && (*env)->GetArrayLength(env, id) == 16) (*env)->GetByteArrayRegion(env, id, 0, 16, (jbyte *)cfg.device_id);
	gd = (*env)->GetStringUTFChars(env, gamesDir, NULL);
	rd = (*env)->GetStringUTFChars(env, romsDir, NULL);
	nm = name ? (*env)->GetStringUTFChars(env, name, NULL) : NULL;
	cfg.games_dir = gd;
	cfg.roms_dir = rd;
	cfg.device_name = nm ? nm : "Android";
	{
		core_host_ctx *ctx = appcore_ctx();
		ctx->open_rom = jni_open_rom;
		core_host_fill(&cfg.host, ctx);
	}
	g_link = phone_create(&cfg);     /* copies the strings */
	(*env)->ReleaseStringUTFChars(env, gamesDir, gd);
	(*env)->ReleaseStringUTFChars(env, romsDir, rd);
	if (nm) (*env)->ReleaseStringUTFChars(env, name, nm);
	LOGI("engine %s", g_link ? "ready" : "FAILED");
}

J(void, nativeStart)(JNIEnv *env, jclass cls) { (void)env; (void)cls; if (g_link) phone_link_start(g_link); }

/* Non-blocking: the thread notices within ~one poll tick, and the next start joins it. */
J(void, nativeStop)(JNIEnv *env, jclass cls) { (void)env; (void)cls; if (g_link) phone_link_stop(g_link); }

J(jint, nativeStatus)(JNIEnv *env, jclass cls) { (void)env; (void)cls; return g_link ? phone_link_status(g_link) : PL_ST_STOPPED; }

J(void, nativeSetRoms)(JNIEnv *env, jclass cls, jobjectArray shas, jlongArray sizes, jobjectArray sources)
{
	jsize n, i;
	phone_rom *r;
	jlong *sz;
	int k = 0;
	(void)cls;
	if (!g_link || !shas || !sizes || !sources) return;
	n = (*env)->GetArrayLength(env, shas);
	r = (phone_rom *)calloc((size_t)n + 1, sizeof *r);
	sz = (*env)->GetLongArrayElements(env, sizes, NULL);
	if (!r || !sz) { free(r); if (sz) (*env)->ReleaseLongArrayElements(env, sizes, sz, JNI_ABORT); return; }
	for (i = 0; i < n; i++) {
		jstring hs = (jstring)(*env)->GetObjectArrayElement(env, shas, i);
		jstring ss = (jstring)(*env)->GetObjectArrayElement(env, sources, i);
		const char *src = ss ? (*env)->GetStringUTFChars(env, ss, NULL) : NULL;
		if (src && sha_from_jstring(env, hs, r[k].sha1) == 0) {
			r[k].size = (uint64_t)sz[i];
			snprintf(r[k].source, sizeof r[k].source, "%s", src);
			k++;
		}
		if (src) (*env)->ReleaseStringUTFChars(env, ss, src);
		if (hs) (*env)->DeleteLocalRef(env, hs);
		if (ss) (*env)->DeleteLocalRef(env, ss);
	}
	(*env)->ReleaseLongArrayElements(env, sizes, sz, JNI_ABORT);
	phone_set_roms(g_link, r, k);
	free(r);
}

J(jstring, nativeUiText)(JNIEnv *env, jclass cls)
{
	char stackbuf[6000], *buf = stackbuf;
	size_t cap = sizeof stackbuf, need;
	jstring js;
	int try_;
	(void)cls;
	if (!g_link) return NULL;
	need = phone_ui_text(g_link, buf, cap);
	for (try_ = 0; need >= cap && try_ < 3; try_++) {   /* truncated (notices not consumed yet): grow and retry */
		char *big = (char *)malloc(need + 256);
		if (!big) break;
		if (buf != stackbuf) free(buf);
		buf = big; cap = need + 256;
		need = phone_ui_text(g_link, buf, cap);
	}
	if (need >= cap) { buf[cap - 1] = 0; LOGI("ui text truncated (%zu >= %zu)", need, cap); }
	js = (*env)->NewStringUTF(env, buf);
	if (buf != stackbuf) free(buf);
	return js;
}

/* RGBA bytes (alpha forced to 0xFF), 240x160x4, or null. */
J(jbyteArray, nativeConflictThumb)(JNIEnv *env, jclass cls, jstring sha, jboolean remote)
{
	uint8_t id[20];
	uint8_t *px;
	jbyteArray a = NULL;
	(void)cls;
	if (!g_link || sha_from_jstring(env, sha, id) != 0) return NULL;
	px = (uint8_t *)malloc(PHONE_THUMB_BYTES);
	if (px && phone_conflict_thumb(g_link, id, remote ? 1 : 0, px)) {
		size_t i;
		for (i = 3; i < PHONE_THUMB_BYTES; i += 4) px[i] = 0xFF;
		a = (*env)->NewByteArray(env, (jsize)PHONE_THUMB_BYTES);
		if (a) (*env)->SetByteArrayRegion(env, a, 0, (jsize)PHONE_THUMB_BYTES, (const jbyte *)px);
	}
	free(px);
	return a;
}

J(void, nativeReturn)(JNIEnv *env, jclass cls) { (void)env; (void)cls; if (g_link) phone_cmd_return(g_link); }
J(void, nativeSendKeys)(JNIEnv *env, jclass cls, jint keys) { (void)env; (void)cls; if (g_link) phone_cmd_input(g_link, (uint16_t)(keys & 0x3FF)); }
J(void, nativeHandoff)(JNIEnv *env, jclass cls) { (void)env; (void)cls; if (g_link) phone_cmd_handoff(g_link); }

J(void, nativeResolve)(JNIEnv *env, jclass cls, jstring sha, jboolean keepPhone)
{
	uint8_t id[20];
	(void)cls;
	if (g_link && sha_from_jstring(env, sha, id) == 0) phone_cmd_resolve(g_link, id, keepPhone ? 1 : 0);
}

J(void, nativeFetch)(JNIEnv *env, jclass cls, jstring sha)
{
	uint8_t id[20];
	(void)cls;
	if (g_link && sha_from_jstring(env, sha, id) == 0) phone_cmd_fetch(g_link, id);
}

J(void, nativeAckOpen)(JNIEnv *env, jclass cls, jstring sha)
{
	uint8_t id[20];
	(void)cls;
	if (g_link && sha_from_jstring(env, sha, id) == 0) phone_ack_open(g_link, id);
}

/* Store migration (idempotent). Works before the engine exists. */
J(jint, nativeMigrate)(JNIEnv *env, jclass cls, jstring gamesDir, jstring backupDir, jbyteArray id)
{
	uint8_t dev[16] = { 0 };
	const char *gd = (*env)->GetStringUTFChars(env, gamesDir, NULL);
	const char *bd = (*env)->GetStringUTFChars(env, backupDir, NULL);
	jint r = -1;
	(void)cls;
	if (id && (*env)->GetArrayLength(env, id) == 16) (*env)->GetByteArrayRegion(env, id, 0, 16, (jbyte *)dev);
	if (gd && bd) r = phone_migrate_store(gd, bd, dev);
	if (gd) (*env)->ReleaseStringUTFChars(env, gamesDir, gd);
	if (bd) (*env)->ReleaseStringUTFChars(env, backupDir, bd);
	LOGI("migration: %d game(s)", r);
	return r;
}
