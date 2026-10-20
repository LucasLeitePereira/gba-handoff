/*
 * core_host - phone_host callbacks over a gbah_core. Shared by the app (jni_bridge.c owns the core,
 * its lock and the emulation thread) and by the host test, so the test exercises the very code that
 * snapshots, applies and freezes the game in production. No JNI / Android dependency.
 *
 * Locking contract: `mtx` is the core lock. The emulation thread holds it while a frame runs and
 * must, INSIDE that lock, look at `hold` and skip the frame when it is set; hold(1) takes the lock
 * after setting the flag, so when it returns no frame is running and none will start.
 */
#ifndef CORE_HOST_H
#define CORE_HOST_H

#include <pthread.h>
#include <stdatomic.h>

#include "gbah_core.h"
#include "phone_link.h"

typedef struct {
	gbah_core **core;
	pthread_mutex_t *mtx;
	atomic_int *hold;
	int (*open_rom)(void *u, const char *source);   /* content:// sources; may be NULL */
	void *open_user;
} core_host_ctx;

void core_host_fill(phone_host *h, core_host_ctx *ctx);

#endif
