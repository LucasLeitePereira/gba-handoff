/*
 * phone_link - the phone side of the handoff: link session with the PC, sync decisions, game
 * store, autosave. Plain C11 + pthreads, NO JNI / Android / mGBA dependency: everything it needs
 * from the emulator comes through `phone_host`, so the SAME code runs in the app (link_jni.c) and
 * in the host test (tests/phone_link_test.c) against the real desktop binary.
 *
 * Threads (created by phone_create):
 *   link thread     TCP session (HELLO, LIBRARY, pushes, ROM transfer, commands from the UI)
 *   autosave thread every ~30 s and when the cartridge save changes: snapshot (core lock, brief),
 *                   then disk, outside the core lock.
 * Locks: p->mtx guards the UI-visible state (short sections); p->store_mtx serialises every access
 * to the games directory (gbah_sync's store is not thread-safe). Order: store_mtx before mtx.
 * The host callbacks are called WITHOUT p->mtx held.
 *
 * What is played where: when the phone hands a game to the PC the emulation is HELD (no frames) and
 * nothing is saved for that game until the PC gives it back or the cable is gone (then it resumes
 * from the last mirror it stored). See sync/gbah_sync.h for the version rules.
 */
#ifndef PHONE_LINK_H
#define PHONE_LINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PHONE_THUMB_BYTES (240u * 160u * 4u)

/* Result of host.snapshot (malloc'ed members; release with phone_snap_free). */
uint64_t phone_hash(const void *data, size_t n);   /* FNV-1a 64; hosts must use it for sav_hash */

typedef struct {
	uint8_t sha1[20];
	void *state; size_t state_n;
	void *sav;   size_t sav_n;      /* sav_n == 0: the game has no save data yet */
	uint8_t *thumb;                 /* PHONE_THUMB_BYTES, XBGR8888, or NULL */
	uint32_t frame;
} phone_snap;
void phone_snap_free(phone_snap *s);

/* What the engine needs from the emulator. All return 0 on success unless noted. */
typedef struct {
	void *user;
	/* 1 and sha1 filled if a game is loaded in the core. */
	int (*loaded)(void *u, uint8_t sha1[20]);
	/* Freeze (1) / unfreeze (0) the emulation. On return no frame is running and none will start. */
	int (*hold)(void *u, int on);
	/* State + .sav + thumbnail of the loaded game, taken between two frames. */
	int (*snapshot)(void *u, phone_snap *s);
	/* Replace the running game by this state (+ .sav) through the correct restore order. */
	int (*apply)(void *u, const uint8_t sha1[20], const void *state, size_t sn, const void *sav, size_t vn);
	/* Cold start with a .sav only (no state), right after the ROM was loaded. */
	int (*cold_sav)(void *u, const void *sav, size_t vn);
	uint32_t (*frame)(void *u);       /* frame counter of the loaded game */
	uint64_t (*sav_hash)(void *u);    /* phone_hash() of the cartridge save as exported (empty save: phone_hash(NULL, 0)) */
	/* ROM bytes of a source that is not a plain file (content:// URI): a readable fd that the
	 * engine closes, or -1. */
	int (*open_rom)(void *u, const char *source);
} phone_host;

typedef struct {
	const char *games_dir;       /* gbah_store directory (<files>/games) */
	const char *roms_dir;        /* ROMs received from the PC: <sha1>.gba */
	uint8_t device_id[16];
	const char *device_name;
	const char *core_version;    /* NULL = gbah_core_version() (tests may lie to provoke a refusal) */
	uint16_t port;               /* 0 = GBAHP_DEFAULT_PORT */
	unsigned autosave_ms;        /* 0 = 30000 */
	unsigned tick_ms;            /* autosave thread period; 0 = 1000 (tests use less) */
	unsigned sav_min_ms;         /* min gap between "the .sav changed" saves; 0 = 5000 */
	phone_host host;
} phone_cfg;

typedef struct phone_link phone_link;

phone_link *phone_create(const phone_cfg *cfg);
void phone_destroy(phone_link *p);                 /* stops and joins both threads */

/* Link thread. stop() never blocks (a later start() joins the old thread). */
void phone_link_start(phone_link *p);
void phone_link_stop(phone_link *p);

/* Sources of ROMs the phone knows: sha1, size and where to read them ("file:///path",
 * "content://..." or a plain path). Replaces the previous table. */
typedef struct { uint8_t sha1[20]; uint64_t size; char source[512]; } phone_rom;
void phone_set_roms(phone_link *p, const phone_rom *roms, int n);

/* Hooks from the emulator side. */
void phone_game_loaded(phone_link *p);             /* a ROM was loaded into the core */
void phone_game_unloaded(phone_link *p);
void phone_set_running(phone_link *p, int running); /* frames are being emulated (autosave only then) */

/* Resumes the stored progress of the loaded game into the core (state + sav, else cold .sav).
 * 0 = restored, 1 = nothing stored, <0 = error (the core may be fresh). */
int phone_resume_stored(phone_link *p);
/* Synchronous save of the loaded game (onPause, quitting). 0 = saved, 1 = skipped (nothing new,
 * or the game is on the PC / held), <0 = error. */
int phone_save_now(phone_link *p);

/* ---- UI ---------------------------------------------------------------------------------
 * phone_ui_text: a compact text snapshot, one record per line, fields separated by ' ':
 *   S <link 0..3> <peer name...>       link status (PL_ST_*), PC name (may be empty)
 *   H <sha1>                           this game was handed to the PC and is being played there
 *   W <sha1>                           handing a game to the PC (waiting for its answer)
 *   P <0|1> <sha1|->                   the PC is playing a game
 *   O <sha1>                           the PC gave a game back that is not loaded: open it
 *   C <sha1> <lts> <lframe> <ldev> <rts> <rframe> <rdev> <rthumb> <waiting_pc> <have_remote>
 *                                      open conflict; dev: 0 this phone, 1 the PC, 2 other
 *   T <sha1> <percent>                 ROM being received
 *   N <text...>                        notice (consumed by this call)
 * Returns the length (>= cap means truncated; then the notices are NOT consumed, so a retry with a
 * bigger buffer loses nothing). */
size_t phone_ui_text(phone_link *p, char *buf, size_t cap);
/* Thumbnails of a conflict (XBGR8888, PHONE_THUMB_BYTES). remote=0: this phone. 1 if available. */
int phone_conflict_thumb(phone_link *p, const uint8_t sha1[20], int remote, uint8_t *out);
/* Link statuses (match LinkClient.kt) */
enum { PL_ST_STOPPED = 0, PL_ST_SEARCHING = 1, PL_ST_CONNECTED = 2, PL_ST_REFUSED = 3 };
int phone_link_status(phone_link *p);

/* Commands (UI thread; executed by the link thread). */
void phone_cmd_return(phone_link *p);                              /* "Voltar para o celular" */
void phone_cmd_handoff(phone_link *p);                             /* "Enviar ao PC": the open game */
void phone_cmd_resolve(phone_link *p, const uint8_t sha1[20], int keep_phone);
void phone_cmd_fetch(phone_link *p, const uint8_t sha1[20]);       /* bring the PC's version of a game */
/* Phone as the PC's controller (GBA KEYINPUT bits, GBAH_KEY_*): the latest keys, sent immediately on a
 * change (coalesced) and re-sent every ~100 ms while any key is held. Ignored unless the open game is
 * on the PC. Callable from any thread; only the link thread touches the socket. */
void phone_cmd_input(phone_link *p, uint16_t keys);
void phone_ack_open(phone_link *p, const uint8_t sha1[20]);        /* the UI handled an "O" record */

/* ---- migration ------------------------------------------------------------------------
 * The pre-sync app stored <sha1>.state/.sav without a .meta. This writes the missing (or
 * unreadable) .meta (version (dev,1), timestamp = the state file's mtime) and, first, copies the
 * legacy .state/.sav into backup_dir (never overwrites an existing copy). The .state/.sav
 * themselves are never touched. Idempotent. Returns the number of games migrated now (0 = nothing
 * to do), <0 on error. */
int phone_migrate_store(const char *games_dir, const char *backup_dir, const uint8_t dev[16]);

#ifdef __cplusplus
}
#endif
#endif
