/*
 * Cable link, phone side: TCP client to 127.0.0.1:PORT (the PC's server, reached through
 * `adb reverse`). Same proto/ code as the desktop; written in C instead of Kotlin so encode,
 * decode and the parser exist exactly once (a Kotlin client would need a JNI call per message
 * anyway) and so no JVM objects are created per message.
 *
 * One pthread, only alive while the app is in the foreground (Kotlin start/stop):
 *   connect (1 s timeout) -> HELLO -> HELLO_ACK -> LIBRARY -> PING every 5 s, PONG expected.
 * No connection: retry every 2 s, backing off x1.5 up to 10 s (battery). The UI polls
 * status()/peerName(); there are no callbacks into Java, so no JNIEnv attach on this thread.
 */
#include <jni.h>

#include <android/log.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "gbah_core.h"
#include "gbahp_net.h"

#define TAG "gbah-link"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

/* Keep in sync with LinkClient.kt */
enum { ST_STOPPED = 0, ST_SEARCHING = 1, ST_CONNECTED = 2, ST_REFUSED = 3 };

#define RETRY_MIN_MS 2000
#define RETRY_MAX_MS 10000
#define PING_EVERY_MS 5000
#define IDLE_TIMEOUT_MS 15000

static struct {
	pthread_mutex_t mtx;
	pthread_cond_t cv;
	pthread_t thread;
	int thread_valid;
	atomic_int run;
	atomic_int status;
	gbahp_sock sock;          /* g.mtx: socket of the live session, for stop() to shut down */
	char peer[96];            /* g.mtx */
	uint8_t id[16];
	char name[128];
	uint16_t port;
} g = { .mtx = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .sock = GBAHP_BAD_SOCK };

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Sleeps up to ms, wakes early when stop() signals. */
static void nap(int ms)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
	pthread_mutex_lock(&g.mtx);
	if (atomic_load(&g.run)) pthread_cond_timedwait(&g.cv, &g.mtx, &ts);
	pthread_mutex_unlock(&g.mtx);
}

/* Returns 1 if the session reached CONNECTED (so the retry delay resets), 0 otherwise. */
static int session(gbahp_sock s)
{
	gbahp_conn c;
	gbahp_frame f;
	gbahp_hello me;
	gbahp_hello_ack ack;
	uint8_t out[512];
	size_t n;
	int r = 0, waited, established = 0;
	long last_rx, last_ping;
	uint64_t token = 0;

	pthread_mutex_lock(&g.mtx);
	g.sock = s;
	pthread_mutex_unlock(&g.mtx);
	gbahp_conn_init(&c, s);

	memset(&me, 0, sizeof me);
	me.proto_version = GBAHP_VERSION;
	memcpy(me.device_id, g.id, 16);
	me.core_version = gbahp_str_c(gbah_core_version());
	me.name = gbahp_str_c(g.name);
	n = gbahp_encode_hello(&me, out, sizeof out);
	if (!n || gbahp_conn_send(&c, out, n) != 0) goto done;

	for (waited = 0; waited < 5000 && atomic_load(&g.run); waited += 200) {
		r = gbahp_conn_recv(&c, &f, 200);
		if (r != 0) break;
	}
	if (r != 1 || f.type != GBAHP_MSG_HELLO_ACK || gbahp_decode_hello_ack(f.payload, f.len, &ack) != 0) goto done;
	if (ack.status != GBAHP_ACK_OK) {
		LOGI("PC refused the connection (status %d)", ack.status);
		atomic_store(&g.status, ST_REFUSED);
		goto done;
	}
	pthread_mutex_lock(&g.mtx);
	{
		size_t k = ack.hello.name.n < sizeof g.peer - 1 ? ack.hello.name.n : sizeof g.peer - 1;
		memcpy(g.peer, ack.hello.name.p, k);
		g.peer[k] = 0;
	}
	pthread_mutex_unlock(&g.mtx);
	atomic_store(&g.status, ST_CONNECTED);
	established = 1;
	LOGI("connected to PC '%s'", g.peer);

	n = gbahp_encode_library(NULL, 0, out, sizeof out);   /* nothing to offer yet (handoff phase) */
	if (n) gbahp_conn_send(&c, out, n);

	last_rx = last_ping = now_ms();
	while (atomic_load(&g.run)) {
		long t;
		r = gbahp_conn_recv(&c, &f, 500);
		t = now_ms();
		if (r < 0) break;
		if (r == 1) {
			last_rx = t;
			if (f.type == GBAHP_MSG_BYE) break;
			if (f.type == GBAHP_MSG_PING) {   /* the PC may ping too */
				gbahp_ping p;
				if (gbahp_decode_ping(f.payload, f.len, &p) == 0) {
					n = gbahp_encode_pong(&p, out, sizeof out);
					if (!n || gbahp_conn_send(&c, out, n) != 0) break;
				}
			}
			/* PONG, LIBRARY, ...: nothing to do in this phase */
		}
		if (t - last_rx > IDLE_TIMEOUT_MS) { LOGI("PC silent, dropping"); break; }
		if (t - last_ping >= PING_EVERY_MS) {
			gbahp_ping p;
			p.token = ++token;
			last_ping = t;
			n = gbahp_encode_ping(&p, out, sizeof out);
			if (!n || gbahp_conn_send(&c, out, n) != 0) break;
		}
	}
	if (atomic_load(&g.run) == 0) {   /* leaving on purpose: say goodbye */
		gbahp_bye b = { 0 };
		n = gbahp_encode_bye(&b, out, sizeof out);
		if (n) gbahp_conn_send(&c, out, n);
	}
done:
	pthread_mutex_lock(&g.mtx);
	g.sock = GBAHP_BAD_SOCK;
	g.peer[0] = 0;
	pthread_mutex_unlock(&g.mtx);
	gbahp_conn_close(&c);
	return established;
}

static void *thread_main(void *arg)
{
	int delay = RETRY_MIN_MS;
	(void)arg;
	gbahp_net_init();
	while (atomic_load(&g.run)) {
		gbahp_sock s;
		int refused;
		atomic_store(&g.status, ST_SEARCHING);
		s = gbahp_net_connect_loopback(g.port, 1000);
		if (s == GBAHP_BAD_SOCK) {
			nap(delay);
			delay = delay * 3 / 2;
			if (delay > RETRY_MAX_MS) delay = RETRY_MAX_MS;
			continue;
		}
		delay = session(s) ? RETRY_MIN_MS : RETRY_MAX_MS;
		refused = atomic_load(&g.status) == ST_REFUSED;
		if (atomic_load(&g.run)) {
			if (!refused) atomic_store(&g.status, ST_SEARCHING);
			nap(refused ? RETRY_MAX_MS : delay);   /* refused: keep the message on screen */
		}
	}
	atomic_store(&g.status, ST_STOPPED);
	return NULL;
}

#define J(ret, name) JNIEXPORT ret JNICALL Java_io_gbahandoff_LinkClient_##name

J(void, nativeStart)(JNIEnv *env, jclass cls, jbyteArray id, jstring name)
{
	const char *nm;
	(void)cls;
	if (atomic_load(&g.run)) return;
	if (g.thread_valid) { pthread_join(g.thread, NULL); g.thread_valid = 0; }   /* previous stop() */

	memset(g.id, 0, sizeof g.id);
	if (id && (*env)->GetArrayLength(env, id) == 16) (*env)->GetByteArrayRegion(env, id, 0, 16, (jbyte *)g.id);
	nm = name ? (*env)->GetStringUTFChars(env, name, NULL) : NULL;
	snprintf(g.name, sizeof g.name, "%s", nm ? nm : "Android");
	if (nm) (*env)->ReleaseStringUTFChars(env, name, nm);
	g.port = GBAHP_DEFAULT_PORT;

	atomic_store(&g.run, 1);
	atomic_store(&g.status, ST_SEARCHING);
	if (pthread_create(&g.thread, NULL, thread_main, NULL) == 0) g.thread_valid = 1;
	else { atomic_store(&g.run, 0); atomic_store(&g.status, ST_STOPPED); }
}

/* Non-blocking: the thread notices within ~one poll tick, and nativeStart joins it. */
J(void, nativeStop)(JNIEnv *env, jclass cls)
{
	(void)env; (void)cls;
	pthread_mutex_lock(&g.mtx);
	atomic_store(&g.run, 0);
	if (g.sock != GBAHP_BAD_SOCK) gbahp_net_shutdown(g.sock);
	pthread_cond_broadcast(&g.cv);
	pthread_mutex_unlock(&g.mtx);
}

J(jint, nativeStatus)(JNIEnv *env, jclass cls)
{
	(void)env; (void)cls;
	return atomic_load(&g.status);
}

J(jstring, nativePeerName)(JNIEnv *env, jclass cls)
{
	char tmp[sizeof g.peer];
	(void)cls;
	pthread_mutex_lock(&g.mtx);
	memcpy(tmp, g.peer, sizeof tmp);
	pthread_mutex_unlock(&g.mtx);
	return tmp[0] ? (*env)->NewStringUTF(env, tmp) : NULL;
}
