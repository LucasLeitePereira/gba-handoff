/*
 * gbahp - GBA Handoff wire protocol (framing + messages). Pure C99, no
 * dependencies, no I/O, no global state: builds on desktop, NDK and mingw.
 *
 * FRAME (all integers little-endian, header is FIXED FOREVER, 16 bytes):
 *    0  u8[4] magic "GBAH"
 *    4  u8    protocol version (GBAHP_VERSION)
 *    5  u8    message type (GBAHP_MSG_*)
 *    6  u16   flags (reserved, 0)
 *    8  u32   payload length (<= GBAHP_MAX_PAYLOAD)
 *   12  u32   CRC32 (IEEE, zlib-compatible) of the payload
 *   16  payload
 * Everything is encoded/decoded field by field (never memcpy of a struct).
 *
 * Encoders return the size of the COMPLETE frame (header + payload), or 0 on
 * error (bad argument, field too long, buffer too small). With out == NULL
 * they only compute the size. Decoders take the payload of a frame and fill a
 * struct whose pointers are VIEWS into that payload (zero-copy: valid only as
 * long as the payload buffer is).
 */
#ifndef GBAHP_H
#define GBAHP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GBAHP_VERSION        3
#define GBAHP_HEADER_SIZE    16
#define GBAHP_MAX_PAYLOAD    (64u * 1024u * 1024u)
#define GBAHP_DEFAULT_PORT   27183
#define GBAHP_ROM_CHUNK      (256u * 1024u)       /* recommended ROM_PUSH chunk */
#define GBAHP_THUMB_BYTES    (240u * 160u * 4u)   /* raw XBGR8888, same as the core framebuffer */
#define GBAHP_MAX_STR        255                  /* name / core version length (u8 prefix) */

enum {
	GBAHP_MSG_HELLO       = 1,
	GBAHP_MSG_HELLO_ACK   = 2,
	GBAHP_MSG_LIBRARY     = 3,
	GBAHP_MSG_ROM_REQUEST = 4,
	GBAHP_MSG_ROM_PUSH    = 5,
	GBAHP_MSG_STATE_PUSH  = 6,
	GBAHP_MSG_STATE_ACK   = 7,
	GBAHP_MSG_INPUT       = 8,
	GBAHP_MSG_PING        = 9,
	GBAHP_MSG_PONG        = 10,
	GBAHP_MSG_BYE         = 11,
	GBAHP_MSG_CONTROL     = 12,
	GBAHP_MSG_THUMB       = 13
};

enum {
	GBAHP_OK         = 0,
	GBAHP_E_ARG      = -1,   /* bad argument */
	GBAHP_E_MAGIC    = -2,   /* stream out of sync */
	GBAHP_E_TOO_BIG  = -3,   /* payload above GBAHP_MAX_PAYLOAD */
	GBAHP_E_CRC      = -4,
	GBAHP_E_TRUNC    = -5,   /* payload shorter than the message needs */
	GBAHP_E_TRAILING = -6,   /* extra bytes after the message */
	GBAHP_E_RANGE    = -7,   /* field with an invalid value */
	GBAHP_E_NOMEM    = -8,
	GBAHP_E_POISONED = -9    /* parser already failed: drop the connection */
};

uint32_t gbahp_crc32(const void *data, size_t n);

/* ------------------------------------------------------------ framing */

/* Writes the 16-byte header for a payload already placed somewhere. */
void gbahp_header_write(uint8_t out[GBAHP_HEADER_SIZE], uint8_t type, uint16_t flags,
                        const void *payload, uint32_t len);
/* Header + payload copied into out. 0 on error. */
size_t gbahp_frame_encode(uint8_t type, uint16_t flags, const void *payload, size_t len,
                          uint8_t *out, size_t cap);

typedef struct {
	uint8_t version;
	uint8_t type;
	uint16_t flags;
	uint32_t len;
	const uint8_t *payload;   /* valid until the next gbahp_parser_feed / free */
} gbahp_frame;

/*
 * Incremental parser: feed it arbitrary pieces of the TCP stream. Small
 * payloads (<= GBAHP_PARSER_INLINE) never touch the heap; bigger ones grow a
 * heap buffer as data actually arrives (a lying length costs nothing) and
 * keep it for reuse (freed if > 1 MiB, or by gbahp_parser_free).
 * Zero-initialise with gbahp_parser_init.
 */
#define GBAHP_PARSER_INLINE 2048
typedef struct {
	uint8_t hdr[GBAHP_HEADER_SIZE];
	uint8_t inl[GBAHP_PARSER_INLINE];
	uint8_t *heap;
	size_t heap_cap;
	uint32_t hdr_got, len, got, crc;
	uint8_t state, version, type;
	uint16_t flags;
	int error;
} gbahp_parser;

void gbahp_parser_init(gbahp_parser *p);
void gbahp_parser_free(gbahp_parser *p);
/*
 * Consumes bytes from data[0..n). Returns:
 *   1  a frame is complete (*out filled); *consumed says how many bytes were
 *      used, call again with the rest;
 *   0  need more data (all n bytes were consumed);
 *  <0  GBAHP_E_*; the parser is poisoned (the stream cannot be resynchronised).
 */
int gbahp_parser_feed(gbahp_parser *p, const uint8_t *data, size_t n, size_t *consumed,
                      gbahp_frame *out);

/* ------------------------------------------------------------ messages */

typedef struct { const char *p; size_t n; } gbahp_str;   /* view, not NUL-terminated */
gbahp_str gbahp_str_c(const char *s);

/* HELLO: both sides send one. core_version = libmgba version string; a state
 * is only valid between identical core versions (see contrato-handoff-estado). */
typedef struct {
	uint16_t proto_version;
	uint8_t device_id[16];
	gbahp_str core_version;
	gbahp_str name;           /* device name, UTF-8 */
} gbahp_hello;

enum {
	GBAHP_ACK_OK             = 0,
	GBAHP_ACK_PROTO_MISMATCH = 1,
	GBAHP_ACK_CORE_MISMATCH  = 2,
	GBAHP_ACK_BUSY           = 3
};
typedef struct {
	uint8_t status;           /* GBAHP_ACK_*; != OK means the server closes */
	gbahp_hello hello;        /* the answering side's own identity */
} gbahp_hello_ack;

/* Compatibility rule used by the server to fill HELLO_ACK.status: protocol
 * versions and core version strings must be IDENTICAL. */
int gbahp_hello_compat(const gbahp_hello *local, const gbahp_hello *remote);

/* One game as one device sees it. size = ROM size in bytes (0 = this device has no ROM file).
 * has_data = it holds progress (a state); then version/base/timestamp/frame describe it
 * (see sync/gbah_sync.h for the rules). */
#define GBAHP_LIB_ENTRY_BYTES 89
typedef struct {
	uint8_t sha1[20];
	uint64_t size;
	uint8_t has_data;
	uint8_t version_dev[16]; uint64_t version;
	uint8_t base_dev[16];    uint64_t base;
	uint64_t timestamp_ms;
	uint32_t frame;
} gbahp_lib_entry;
typedef struct {
	uint32_t count;
	const uint8_t *raw;       /* view; use gbahp_library_get */
} gbahp_library;

typedef struct { uint8_t sha1[20]; } gbahp_rom_request;

/* ROM_PUSH: one chunk. The ROM is complete when offset + data_len == total_size. */
typedef struct {
	uint8_t sha1[20];
	uint64_t total_size;
	uint64_t offset;
	const uint8_t *data;
	uint32_t data_len;
} gbahp_rom_push;

/* STATE_PUSH flags */
enum {
	GBAHP_PUSH_MIRROR   = 1,   /* safety copy of a game being played by the sender: store, never take over */
	GBAHP_PUSH_RESOLVED = 2,   /* the user picked this version in a conflict: overwrite (old one -> backup) */
	GBAHP_PUSH_HANDOFF  = 4    /* the sender stops playing; the receiver takes over this state */
};

typedef struct {
	uint8_t sha1_rom[20];
	uint64_t version;                 /* version = (origin_device, version counter) */
	uint8_t origin_device[16];
	uint8_t base_device[16];          /* base = version both sides were last identical at */
	uint64_t base;
	uint8_t flags;                    /* GBAHP_PUSH_* */
	uint64_t timestamp_ms;            /* unix epoch, ms */
	uint32_t frame;
	const uint8_t *state; uint32_t state_len;
	const uint8_t *sav;   uint32_t sav_len;      /* may be empty (game has no save yet) */
	const uint8_t *thumb; uint32_t thumb_len;    /* 0 or GBAHP_THUMB_BYTES */
} gbahp_state_push;

enum {
	GBAHP_STATE_ACK_OK       = 0,
	GBAHP_STATE_ACK_CONFLICT = 1,   /* receiver has a diverging version */
	GBAHP_STATE_ACK_NO_ROM   = 2,
	GBAHP_STATE_ACK_ERROR    = 3,
	GBAHP_STATE_ACK_STALE    = 4    /* receiver already holds something newer: nothing stored */
};
typedef struct { uint8_t sha1_rom[20]; uint64_t version; uint8_t origin_device[16]; uint8_t status; } gbahp_state_ack;

/* CONTROL: small commands that are not state transfers. */
enum {
	GBAHP_CTL_RETURN          = 1,  /* phone -> PC: stop playing and HANDOFF-push the game back to me */
	GBAHP_CTL_FETCH           = 2,  /* phone -> PC: push your version of the game (RESOLVED); you keep playing */
	GBAHP_CTL_PC_STATUS       = 3,  /* PC -> phone: arg 1 = playing sha1, 0 = idle (sha1 ignored) */
	GBAHP_CTL_ROM_UNAVAILABLE = 4,  /* either way: cannot provide the ROM that was asked for (answers ROM_REQUEST) */
	GBAHP_CTL_PLACE_PC        = 5,  /* phone -> PC: after a conflict was resolved, the PC plays the game (sha1) from what it stores */
	GBAHP_CTL_PLACE_PHONE     = 6,  /* phone -> PC: ... the phone plays it; the PC stays paused ("Jogando no celular") */
	GBAHP_CTL_SETTINGS        = 7,  /* either way, right after HELLO: arg = GBAHP_SET_* bits (announcement; sha1 ignored) */
	GBAHP_CTL_SETTINGS_SET    = 8   /* phone -> PC: the user chose these bits as the default for both: adopt and save */
};
/* GBAHP_CTL_SETTINGS / SETTINGS_SET bits */
#define GBAHP_SET_PREFER_NEWEST 1u  /* resolve a version conflict by itself: the newest version wins (the loser is backed up) */
#define GBAHP_CTL_LAST GBAHP_CTL_SETTINGS_SET
typedef struct { uint8_t cmd; uint8_t sha1[20]; uint32_t arg; } gbahp_control;

/* THUMB: the thumbnail of a game's version (sent for conflicts: the LIBRARY has no images). */
typedef struct { uint8_t sha1[20]; const uint8_t *thumb; uint32_t thumb_len; } gbahp_thumb;

/* keys: GBAH_KEY_* bitmask (KEYINPUT order, bits 0..9, see core/gbah_core.h) */
typedef struct { uint32_t seq; uint16_t keys; } gbahp_input;
typedef struct { uint64_t token; } gbahp_ping;     /* PONG echoes the token */
typedef struct { uint16_t reason; } gbahp_bye;

size_t gbahp_encode_hello(const gbahp_hello *m, uint8_t *out, size_t cap);
size_t gbahp_encode_hello_ack(const gbahp_hello_ack *m, uint8_t *out, size_t cap);
size_t gbahp_encode_library(const gbahp_lib_entry *e, uint32_t count, uint8_t *out, size_t cap);
size_t gbahp_encode_rom_request(const gbahp_rom_request *m, uint8_t *out, size_t cap);
size_t gbahp_encode_rom_push(const gbahp_rom_push *m, uint8_t *out, size_t cap);
size_t gbahp_encode_state_push(const gbahp_state_push *m, uint8_t *out, size_t cap);
size_t gbahp_encode_state_ack(const gbahp_state_ack *m, uint8_t *out, size_t cap);
size_t gbahp_encode_input(const gbahp_input *m, uint8_t *out, size_t cap);
size_t gbahp_encode_ping(const gbahp_ping *m, uint8_t *out, size_t cap);
size_t gbahp_encode_pong(const gbahp_ping *m, uint8_t *out, size_t cap);
size_t gbahp_encode_bye(const gbahp_bye *m, uint8_t *out, size_t cap);
size_t gbahp_encode_control(const gbahp_control *m, uint8_t *out, size_t cap);
size_t gbahp_encode_thumb(const gbahp_thumb *m, uint8_t *out, size_t cap);

int gbahp_decode_hello(const uint8_t *pl, size_t len, gbahp_hello *m);
int gbahp_decode_hello_ack(const uint8_t *pl, size_t len, gbahp_hello_ack *m);
int gbahp_decode_library(const uint8_t *pl, size_t len, gbahp_library *m);
/* i < m->count */
void gbahp_library_get(const gbahp_library *m, uint32_t i, gbahp_lib_entry *e);
int gbahp_decode_rom_request(const uint8_t *pl, size_t len, gbahp_rom_request *m);
int gbahp_decode_rom_push(const uint8_t *pl, size_t len, gbahp_rom_push *m);
int gbahp_decode_state_push(const uint8_t *pl, size_t len, gbahp_state_push *m);
int gbahp_decode_state_ack(const uint8_t *pl, size_t len, gbahp_state_ack *m);
int gbahp_decode_input(const uint8_t *pl, size_t len, gbahp_input *m);
int gbahp_decode_ping(const uint8_t *pl, size_t len, gbahp_ping *m);   /* also PONG */
int gbahp_decode_bye(const uint8_t *pl, size_t len, gbahp_bye *m);
int gbahp_decode_control(const uint8_t *pl, size_t len, gbahp_control *m);
int gbahp_decode_thumb(const uint8_t *pl, size_t len, gbahp_thumb *m);

#ifdef __cplusplus
}
#endif
#endif
