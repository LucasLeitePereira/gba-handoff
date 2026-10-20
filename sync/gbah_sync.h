/*
 * gbah_sync - version rules + on-disk game store shared by desktop and Android.
 * Pure C99, no mGBA / SDL / JNI dependency, no threads, no global state.
 * NOT thread-safe on the same directory: callers serialise (one mutex per process).
 *
 * VERSIONS. Every game (key = ROM SHA-1) has, per device:
 *   version = (device_id, counter)   the progress this device holds
 *   base    = version at which both devices were last known to be identical
 *   timestamp (ms), frame, thumbnail
 * A device that saves new progress calls gbah_sync_advance() (counter+1, its own id; base is
 * kept). After a confirmed transfer (STATE_ACK) BOTH sides call mark_synced / adopt
 * (base = version). Comparing two sides (gbah_sync_decide):
 *   same version                      -> NOTHING
 *   a side without data               -> the other wins (SEND / RECEIVE)
 *   local.base == remote.version      -> local advanced from remote: SEND
 *   remote.base == local.version      -> RECEIVE
 *   otherwise (both advanced since the common base, or no base at all) -> CONFLICT
 * A zero version (counter 0) never matches anything.
 */
#ifndef GBAH_SYNC_H
#define GBAH_SYNC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint8_t dev[16]; uint64_t counter; } gbah_ver;

typedef struct {
	uint8_t sha1[20];
	int has_data;               /* 0: this device has no progress for the game */
	gbah_ver version, base;
	uint64_t timestamp_ms;      /* unix epoch ms of the progress */
	uint32_t frame;
} gbah_meta;

enum { GBAH_SYNC_NOTHING = 0, GBAH_SYNC_SEND = 1, GBAH_SYNC_RECEIVE = 2, GBAH_SYNC_CONFLICT = 3 };

int gbah_ver_none(const gbah_ver *v);                       /* counter == 0 */
int gbah_ver_eq(const gbah_ver *a, const gbah_ver *b);

/* local vs remote (what the OTHER side holds): what should happen to LOCAL's data. */
int gbah_sync_decide(const gbah_meta *local, const gbah_meta *remote);

/* "Keep the most recent": which of two diverging versions is newer. 1 = a, -1 = b, 0 = the very
 * same version. Order: a side with data beats one without, then timestamp_ms, then the version
 * counter, then the device id (memcmp) so the answer is deterministic and symmetric:
 * newest(a,b) == -newest(b,a). Clocks of the two devices may differ: the loser is always backed up. */
int gbah_sync_newest(const gbah_meta *a, const gbah_meta *b);

/* New progress saved on this device. version = (dev, version.counter + 1); base unchanged. */
void gbah_sync_advance(gbah_meta *m, const uint8_t dev[16], uint64_t ts_ms, uint32_t frame);
/* Confirmed transfer of m's current version: base = version. */
void gbah_sync_mark_synced(gbah_meta *m);
/* Local data replaced by `in` (received + stored): version = in.version, base = in.version. */
void gbah_sync_adopt(gbah_meta *local, const gbah_meta *in);

/* ------------------------------------------------------------ meta file */

#define GBAH_META_HEADER 128
#define GBAH_THUMB_BYTES (240u * 160u * 4u)
/* Header only; returns GBAH_META_HEADER or 0. thumb_len is stored in the header. */
size_t gbah_meta_encode(const gbah_meta *m, uint32_t thumb_len, uint8_t out[GBAH_META_HEADER]);
/* 0 ok, -1 bad magic/format/size. */
int gbah_meta_decode(const uint8_t *buf, size_t n, gbah_meta *m, uint32_t *thumb_len);

/* ------------------------------------------------------------ files (UTF-8 paths, also on Windows) */

int gbah_fs_mkdirs(const char *dir);                                    /* 0 ok */
int gbah_fs_read_all(const char *path, void **out, size_t *n);           /* malloc'ed; 0 ok, -1 missing/error */
int gbah_fs_write_atomic(const char *path, const void *data, size_t n);  /* tmp + rename; 0 ok */
int gbah_fs_read_prefix(const char *path, void *buf, size_t n, size_t *got);  /* first n bytes (e.g. ROM header); 0 ok */
/* Calls cb for every entry name in dir (no "." / ".."). A missing dir is not an error. 0 ok. */
int gbah_fs_list(const char *dir, void (*cb)(const char *name, void *u), void *u);
int gbah_fs_remove(const char *path);                                    /* 0 ok or already missing */
int gbah_fs_exists(const char *path);
int gbah_fs_size(const char *path, uint64_t *size);                      /* 0 ok */
void gbah_fs_free(void *p);
uint64_t gbah_now_ms(void);                                              /* unix epoch ms */

/* ------------------------------------------------------------ game store */

void gbah_sha1_hex(const uint8_t sha1[20], char out[41]);
int gbah_sha1_from_hex(const char *hex, uint8_t out[20]);                /* 0 ok */

/* Reads <dir>/<sha1>.meta (header; optionally the thumbnail, malloc'ed, *thumb NULL if none).
 * Returns 0 = progress exists (m filled), 1 = no progress (m->has_data = 0, sha1 set),
 * <0 = error. A .state without .meta (older app versions) is reported as progress
 * (dev_self, 1), no base, timestamp 0. */
int gbah_store_meta(const char *dir, const uint8_t sha1[20], const uint8_t dev_self[16],
                    gbah_meta *m, uint8_t **thumb, uint32_t *thumb_len);
/* Atomically writes .state, .sav (removed if sav_len == 0) and finally .meta (the commit marker). */
int gbah_store_commit(const char *dir, const gbah_meta *m, const void *state, size_t state_len,
                      const void *sav, size_t sav_len, const void *thumb, uint32_t thumb_len);
int gbah_store_read(const char *dir, const uint8_t sha1[20], void **state, size_t *state_len,
                    void **sav, size_t *sav_len);                          /* sav may be NULL/0; 0 ok */
/* Sets base = version, but ONLY if the stored version still equals `v` (the game may have
 * advanced meanwhile). 0 = done, 1 = skipped (advanced/other), <0 error. */
int gbah_store_mark_synced(const char *dir, const uint8_t sha1[20], const gbah_ver *v);
/* Moves the current files to <sha1>.backup-<ts>.{state,sav,meta}; keeps the `keep` newest. */
int gbah_store_backup(const char *dir, const uint8_t sha1[20], uint64_t ts_ms, int keep);
/* Number of backups (distinct timestamps) of a game; fills up to `max` timestamps, newest first. */
int gbah_store_list_backups(const char *dir, const uint8_t sha1[20], uint64_t *ts, int max);
/* All games that have a .meta or .state: calls cb(sha1) for each. */
int gbah_store_list(const char *dir, void (*cb)(const uint8_t sha1[20], void *user), void *user);

#ifdef __cplusplus
}
#endif
#endif
