#include "core_host.h"

#include <stdlib.h>
#include <string.h>

static int ch_loaded(void *u, uint8_t sha1[20])
{
	core_host_ctx *c = (core_host_ctx *)u;
	int ok;
	pthread_mutex_lock(c->mtx);
	ok = *c->core && gbah_rom_sha1(*c->core, sha1) == 0;
	pthread_mutex_unlock(c->mtx);
	return ok;
}

static int ch_hold(void *u, int on)
{
	core_host_ctx *c = (core_host_ctx *)u;
	atomic_store(c->hold, on ? 1 : 0);
	pthread_mutex_lock(c->mtx);     /* waits for the frame in flight */
	pthread_mutex_unlock(c->mtx);
	return 0;
}

static int ch_snapshot(void *u, phone_snap *s)
{
	core_host_ctx *c = (core_host_ctx *)u;
	int rc = -1;
	void *st = NULL, *sv = NULL;
	size_t sn, vn = 0;
	memset(s, 0, sizeof *s);
	pthread_mutex_lock(c->mtx);
	if (*c->core && gbah_rom_sha1(*c->core, s->sha1) == 0) {
		sn = gbah_state_save(*c->core, &st);
		if (sn) {
			vn = gbah_sram_export(*c->core, &sv);
			s->thumb = (uint8_t *)malloc(PHONE_THUMB_BYTES);
			if (s->thumb) memcpy(s->thumb, gbah_framebuffer(*c->core), PHONE_THUMB_BYTES);
			s->frame = gbah_frame_counter(*c->core);
			s->state = st; s->state_n = sn;
			s->sav = sv; s->sav_n = vn;
			rc = 0;
		}
	}
	pthread_mutex_unlock(c->mtx);
	if (rc != 0) { gbah_free(st); gbah_free(sv); }
	return rc;
}

static int ch_apply(void *u, const uint8_t sha1[20], const void *state, size_t sn, const void *sav, size_t vn)
{
	core_host_ctx *c = (core_host_ctx *)u;
	uint8_t cur[20];
	int rc = -1;
	pthread_mutex_lock(c->mtx);
	if (*c->core && gbah_rom_sha1(*c->core, cur) == 0 && !memcmp(cur, sha1, 20))
		rc = gbah_handoff_restore(*c->core, state, sn, vn ? sav : NULL, vn);
	pthread_mutex_unlock(c->mtx);
	return rc;
}

static int ch_cold_sav(void *u, const void *sav, size_t vn)
{
	core_host_ctx *c = (core_host_ctx *)u;
	int rc = -1;
	pthread_mutex_lock(c->mtx);
	if (*c->core) rc = gbah_load_sav_buffer(*c->core, sav, vn);
	pthread_mutex_unlock(c->mtx);
	return rc;
}

static uint32_t ch_frame(void *u)
{
	core_host_ctx *c = (core_host_ctx *)u;
	uint32_t f = 0;
	pthread_mutex_lock(c->mtx);
	if (*c->core) f = gbah_frame_counter(*c->core);
	pthread_mutex_unlock(c->mtx);
	return f;
}

static uint64_t ch_sav_hash(void *u)
{
	core_host_ctx *c = (core_host_ctx *)u;
	void *sv = NULL;
	size_t vn = 0;
	uint64_t h;
	pthread_mutex_lock(c->mtx);
	if (*c->core) vn = gbah_sram_export(*c->core, &sv);
	pthread_mutex_unlock(c->mtx);
	h = phone_hash(sv, vn);
	gbah_free(sv);
	return h;
}

static int ch_open_rom(void *u, const char *source)
{
	core_host_ctx *c = (core_host_ctx *)u;
	return c->open_rom ? c->open_rom(c->open_user, source) : -1;
}

void core_host_fill(phone_host *h, core_host_ctx *ctx)
{
	h->user = ctx;
	h->loaded = ch_loaded;
	h->hold = ch_hold;
	h->snapshot = ch_snapshot;
	h->apply = ch_apply;
	h->cold_sav = ch_cold_sav;
	h->frame = ch_frame;
	h->sav_hash = ch_sav_hash;
	h->open_rom = ch_open_rom;
}
