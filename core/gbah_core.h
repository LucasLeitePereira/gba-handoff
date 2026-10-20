/*
 * gbah_core - thin C wrapper over libmgba (mCore API), GBA only.
 *
 * No SDL / no platform dependency: shared by desktop (SDL3) and Android (NDK/JNI).
 * Not thread-safe: use one gbah_core from one thread at a time.
 */
#ifndef GBAH_CORE_H
#define GBAH_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GBAH_WIDTH  240
#define GBAH_HEIGHT 160

/* Input bitmask (10 buttons). Same bit layout as the GBA KEYINPUT register
 * (but active HIGH here = pressed). This is also the wire format for the
 * future INPUT message. */
enum {
	GBAH_KEY_A      = 1u << 0,
	GBAH_KEY_B      = 1u << 1,
	GBAH_KEY_SELECT = 1u << 2,
	GBAH_KEY_START  = 1u << 3,
	GBAH_KEY_RIGHT  = 1u << 4,
	GBAH_KEY_LEFT   = 1u << 5,
	GBAH_KEY_UP     = 1u << 6,
	GBAH_KEY_DOWN   = 1u << 7,
	GBAH_KEY_R      = 1u << 8,
	GBAH_KEY_L      = 1u << 9,
	GBAH_KEY_MASK   = 0x3FFu
};

/* Native GBA audio is ~32768 Hz; any rate works (resampled by blip_buf). */
#define GBAH_AUDIO_RATE_DEFAULT 32768

typedef struct gbah_core gbah_core;

/* All int-returning functions: 0 = ok, non-zero = error (unless noted). */

/* Create a core. audio_rate in Hz (0 = default). Returns NULL on failure. */
gbah_core *gbah_create(unsigned audio_rate);
void gbah_destroy(gbah_core *c);

/* Load a ROM (replaces any current one) and reset. The buffer version COPIES
 * the data; the caller may free it afterwards. */
int gbah_load_rom_file(gbah_core *c, const char *path);
int gbah_load_rom_buffer(gbah_core *c, const void *data, size_t size);
void gbah_reset(gbah_core *c);

/* SHA-1 of the whole ROM file as loaded (20 raw bytes / 40 hex chars + NUL).
 * Computed at load time. Returns non-zero if no ROM is loaded. */
int gbah_rom_sha1(const gbah_core *c, uint8_t out[20]);
int gbah_rom_sha1_hex(const gbah_core *c, char out[41]);

/* Joypad state for the NEXT frames, bitmask of GBAH_KEY_*. Sticky. */
void gbah_set_input(gbah_core *c, uint32_t keys);

/* Emulate exactly one video frame. */
void gbah_run_frame(gbah_core *c);
uint32_t gbah_frame_counter(const gbah_core *c);

/*
 * Framebuffer: GBAH_WIDTH x GBAH_HEIGHT pixels, 32 bits/pixel, tightly packed
 * (pitch = GBAH_WIDTH * 4 bytes). Pixel format is XBGR8888: as a native
 * uint32_t, bits 0-7 = R, 8-15 = G, 16-23 = B, 24-31 = unused (x). In memory
 * on little-endian: bytes R,G,B,X. SDL3 name: SDL_PIXELFORMAT_XBGR8888.
 * Android Bitmap ARGB_8888/RGBA_8888 memory order is also R,G,B,A, so it can
 * be copied as-is (alpha byte must be forced to 0xFF there).
 * The pointer is stable for the life of the core; contents are valid after
 * gbah_run_frame() and are overwritten by the next one.
 */
const uint32_t *gbah_framebuffer(const gbah_core *c);

/*
 * Audio produced by the frames emulated since the last call. Writes up to
 * max_frames stereo frames of interleaved signed 16-bit L,R into out and
 * returns the number of frames written. Call after every gbah_run_frame()
 * (~546 frames/call at 32768 Hz) or the internal buffer overflows and drops.
 */
size_t gbah_audio_read(gbah_core *c, int16_t *out, size_t max_frames);
unsigned gbah_audio_rate(const gbah_core *c);

/*
 * Save state (CPU/RAM/IO snapshot; does NOT contain the cartridge save RAM:
 * use the sram functions below for that). The format is mGBA's raw in-memory
 * state: only valid for the same libmgba version, same ROM, little-endian.
 * save: *out = malloc'ed buffer (release with gbah_free), returns size; 0 on error.
 * load: returns 0 on success; the ROM loaded must be the one the state came from.
 */
size_t gbah_state_save(gbah_core *c, void **out);
int gbah_state_load(gbah_core *c, const void *data, size_t size);

/*
 * Cartridge save (.sav, battery RAM / flash / EEPROM), standard raw format
 * compatible with other emulators.
 * export: *out = malloc'ed buffer, returns size. Returns 0 with *out = NULL if
 *         the game has not created any save data yet (type not detected).
 * import: replaces the save data in the running core. Returns 0 on success,
 *         GBAH_ERR_SAVE_TYPE_UNKNOWN if nothing was imported because the core
 *         does not know the save type yet (mGBA ignores the data silently in
 *         that case), other non-zero on other errors.
 *         ORDER MATTERS: on a fresh core call it AFTER gbah_state_load (the
 *         state load forces the state's save type and may re-init the save
 *         memory). Prefer gbah_handoff_restore(), which does it right.
 */
#define GBAH_ERR_SAVE_TYPE_UNKNOWN (-3)
size_t gbah_sram_export(gbah_core *c, void **out);
int gbah_sram_import(gbah_core *c, const void *data, size_t size);

/*
 * COLD START with an existing .sav (just after gbah_load_rom_*, before running;
 * no state involved): hands the file to mGBA as the cartridge save, which also
 * fixes the save type from the file size. gbah_sram_import cannot do this: it
 * fails with GBAH_ERR_SAVE_TYPE_UNKNOWN on a core that has not run yet.
 * For resuming a state use gbah_handoff_restore instead.
 */
int gbah_load_sav_buffer(gbah_core *c, const void *data, size_t size);

/*
 * THE way to resume a handoff on a core that has the same ROM loaded:
 * applies state, then .sav (sav may be NULL/0 = game had no save yet), in the
 * only order that works. 0 = ok; otherwise the error of the failing step (a
 * non-NULL sav that could not be imported is an error, never silently dropped).
 */
int gbah_handoff_restore(gbah_core *c, const void *state, size_t state_size,
                         const void *sav, size_t sav_size);

/* libmgba version string (e.g. "0.10.5"). A save state is only valid between
 * identical values: the network handshake compares it (HELLO.core_version). */
const char *gbah_core_version(void);

void gbah_free(void *p);

/* Convenience: whole-file helpers (malloc'ed result). */
int gbah_read_file(const char *path, void **out, size_t *size);
int gbah_write_file(const char *path, const void *data, size_t size);

/* FNV-1a 64-bit hash of the current framebuffer (for determinism tests). */
uint64_t gbah_framebuffer_hash(const gbah_core *c);

#ifdef __cplusplus
}
#endif
#endif
