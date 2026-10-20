#include "gbahp.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ CRC32 */

uint32_t gbahp_crc32(const void *data, size_t n)
{
	static const uint32_t t[16] = {
		0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
		0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
		0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
	};
	const uint8_t *p = (const uint8_t *)data;
	uint32_t c = 0xFFFFFFFFu;
	size_t i;
	for (i = 0; i < n; i++) {
		c ^= p[i];
		c = (c >> 4) ^ t[c & 15];
		c = (c >> 4) ^ t[c & 15];
	}
	return ~c;
}

/* ------------------------------------------------------------ LE helpers */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }

/* ------------------------------------------------------------ framing */

void gbahp_header_write(uint8_t out[GBAHP_HEADER_SIZE], uint8_t type, uint16_t flags,
                        const void *payload, uint32_t len)
{
	out[0] = 'G'; out[1] = 'B'; out[2] = 'A'; out[3] = 'H';
	out[4] = GBAHP_VERSION;
	out[5] = type;
	put16(out + 6, flags);
	put32(out + 8, len);
	put32(out + 12, gbahp_crc32(payload, len));
}

size_t gbahp_frame_encode(uint8_t type, uint16_t flags, const void *payload, size_t len,
                          uint8_t *out, size_t cap)
{
	if (len > GBAHP_MAX_PAYLOAD || (len && !payload) || !out || cap < GBAHP_HEADER_SIZE + len) return 0;
	if (len) memmove(out + GBAHP_HEADER_SIZE, payload, len);
	gbahp_header_write(out, type, flags, out + GBAHP_HEADER_SIZE, (uint32_t)len);
	return GBAHP_HEADER_SIZE + len;
}

/* ------------------------------------------------------------ parser */

enum { P_HDR = 0, P_PAYLOAD = 1, P_DONE = 2 };

void gbahp_parser_init(gbahp_parser *p) { memset(p, 0, sizeof *p); }

void gbahp_parser_free(gbahp_parser *p)
{
	free(p->heap);
	p->heap = NULL;
	p->heap_cap = 0;
}

static int p_fail(gbahp_parser *p, int err) { p->error = err; return err; }

int gbahp_parser_feed(gbahp_parser *p, const uint8_t *data, size_t n, size_t *consumed,
                      gbahp_frame *out)
{
	size_t used = 0;
	if (!p || !consumed || (n && !data)) return GBAHP_E_ARG;
	*consumed = 0;
	if (p->error) return GBAHP_E_POISONED;
	if (p->state == P_DONE) {
		p->state = P_HDR;
		p->hdr_got = 0;
		if (p->heap_cap > (1u << 20)) gbahp_parser_free(p);
	}
	/* the extra condition lets a zero-length payload complete without more input */
	while (used < n || (p->state == P_PAYLOAD && p->got == p->len)) {
		if (p->state == P_HDR) {
			size_t take = GBAHP_HEADER_SIZE - p->hdr_got;
			if (take > n - used) take = n - used;
			memcpy(p->hdr + p->hdr_got, data + used, take);
			p->hdr_got += (uint32_t)take;
			used += take;
			if (p->hdr_got < GBAHP_HEADER_SIZE) break;
			if (memcmp(p->hdr, "GBAH", 4) != 0) return p_fail(p, GBAHP_E_MAGIC);
			p->version = p->hdr[4];
			p->type = p->hdr[5];
			p->flags = get16(p->hdr + 6);
			p->len = get32(p->hdr + 8);
			p->crc = get32(p->hdr + 12);
			if (p->len > GBAHP_MAX_PAYLOAD) return p_fail(p, GBAHP_E_TOO_BIG);
			p->got = 0;
			p->state = P_PAYLOAD;
		}
		if (p->state == P_PAYLOAD) {
			uint8_t *buf;
			size_t take = p->len - p->got;
			if (take > n - used) take = n - used;
			if (p->len <= GBAHP_PARSER_INLINE) {
				buf = p->inl;
			} else {
				size_t need = (size_t)p->got + take;
				if (need > p->heap_cap) {
					/* grow with the data that really arrives, never to the announced size */
					size_t nc = p->heap_cap * 2;
					uint8_t *nb;
					if (nc < need) nc = need;
					if (nc < 4096) nc = 4096;
					if (nc > p->len) nc = p->len;
					nb = (uint8_t *)realloc(p->heap, nc);
					if (!nb) return p_fail(p, GBAHP_E_NOMEM);
					p->heap = nb;
					p->heap_cap = nc;
				}
				buf = p->heap;
			}
			if (take) memcpy(buf + p->got, data + used, take);
			p->got += (uint32_t)take;
			used += take;
			if (p->got < p->len) break;
			if (gbahp_crc32(buf, p->len) != p->crc) return p_fail(p, GBAHP_E_CRC);
			p->state = P_DONE;
			if (out) {
				out->version = p->version;
				out->type = p->type;
				out->flags = p->flags;
				out->len = p->len;
				out->payload = buf;
			}
			*consumed = used;
			return 1;
		}
	}
	*consumed = used;
	return 0;
}

/* ------------------------------------------------------------ writer / reader */

typedef struct { uint8_t *p; size_t cap, len; int err; } W;

static void w_raw(W *w, const void *src, size_t n)
{
	if (w->p) {
		if (n <= w->cap - w->len) { if (n) memcpy(w->p + w->len, src, n); }
		else w->err = 1;
	}
	w->len += n;
}
static void w_u8(W *w, uint8_t v) { w_raw(w, &v, 1); }
static void w_u16(W *w, uint16_t v) { uint8_t b[2]; put16(b, v); w_raw(w, b, 2); }
static void w_u32(W *w, uint32_t v) { uint8_t b[4]; put32(b, v); w_raw(w, b, 4); }
static void w_u64(W *w, uint64_t v) { w_u32(w, (uint32_t)v); w_u32(w, (uint32_t)(v >> 32)); }
static void w_str(W *w, gbahp_str s)
{
	if (s.n > GBAHP_MAX_STR || (s.n && !s.p)) { w->err = 1; return; }
	w_u8(w, (uint8_t)s.n);
	w_raw(w, s.p, s.n);
}

/* Runs a payload writer, then wraps it in a frame. */
typedef void (*payload_fn)(W *, const void *);
static size_t encode(uint8_t type, payload_fn fn, const void *m, uint8_t *out, size_t cap)
{
	W w;
	if (!m) return 0;
	memset(&w, 0, sizeof w);
	if (out) {
		if (cap < GBAHP_HEADER_SIZE) return 0;
		w.p = out + GBAHP_HEADER_SIZE;
		w.cap = cap - GBAHP_HEADER_SIZE;
	}
	fn(&w, m);
	if (w.err || w.len > GBAHP_MAX_PAYLOAD) return 0;
	if (out) gbahp_header_write(out, type, 0, w.p, (uint32_t)w.len);
	return GBAHP_HEADER_SIZE + w.len;
}

typedef struct { const uint8_t *p; size_t len, pos; int err; } R;

static const uint8_t *r_raw(R *r, size_t n)
{
	const uint8_t *q;
	if (r->err || n > r->len - r->pos) { if (!r->err) r->err = GBAHP_E_TRUNC; return NULL; }
	q = r->p + r->pos;
	r->pos += n;
	return q;
}
static uint8_t r_u8(R *r) { const uint8_t *q = r_raw(r, 1); return q ? q[0] : 0; }
static uint16_t r_u16(R *r) { const uint8_t *q = r_raw(r, 2); return q ? get16(q) : 0; }
static uint32_t r_u32(R *r) { const uint8_t *q = r_raw(r, 4); return q ? get32(q) : 0; }
static uint64_t r_u64(R *r) { const uint8_t *q = r_raw(r, 8); return q ? get64(q) : 0; }
static void r_copy(R *r, uint8_t *dst, size_t n)
{
	const uint8_t *q = r_raw(r, n);
	if (q) memcpy(dst, q, n); else memset(dst, 0, n);
}
static gbahp_str r_str(R *r)
{
	gbahp_str s;
	size_t n = r_u8(r);
	s.p = (const char *)r_raw(r, n);
	s.n = s.p ? n : 0;
	return s;
}
static int r_done(const R *r)
{
	if (r->err) return r->err;
	return r->pos == r->len ? GBAHP_OK : GBAHP_E_TRAILING;
}
static void r_init(R *r, const uint8_t *pl, size_t len)
{
	r->p = pl; r->len = len; r->pos = 0; r->err = 0;
}
#define DEC_ARGS(pl, len, m) if ((!(pl) && (len)) || !(m)) return GBAHP_E_ARG

gbahp_str gbahp_str_c(const char *s)
{
	gbahp_str r;
	r.p = s;
	r.n = s ? strlen(s) : 0;
	return r;
}

/* ------------------------------------------------------------ HELLO / ACK */

static void w_hello_body(W *w, const gbahp_hello *h)
{
	w_u16(w, h->proto_version);
	w_raw(w, h->device_id, 16);
	w_str(w, h->core_version);
	w_str(w, h->name);
}
static void w_hello(W *w, const void *m) { w_hello_body(w, (const gbahp_hello *)m); }
static void w_hello_ack(W *w, const void *m)
{
	const gbahp_hello_ack *a = (const gbahp_hello_ack *)m;
	w_u8(w, a->status);
	w_hello_body(w, &a->hello);
}
size_t gbahp_encode_hello(const gbahp_hello *m, uint8_t *out, size_t cap)
{ return encode(GBAHP_MSG_HELLO, w_hello, m, out, cap); }
size_t gbahp_encode_hello_ack(const gbahp_hello_ack *m, uint8_t *out, size_t cap)
{ return encode(GBAHP_MSG_HELLO_ACK, w_hello_ack, m, out, cap); }

static void r_hello_body(R *r, gbahp_hello *h)
{
	h->proto_version = r_u16(r);
	r_copy(r, h->device_id, 16);
	h->core_version = r_str(r);
	h->name = r_str(r);
}
int gbahp_decode_hello(const uint8_t *pl, size_t len, gbahp_hello *m)
{
	R r;
	DEC_ARGS(pl, len, m);
	r_init(&r, pl, len);
	r_hello_body(&r, m);
	return r_done(&r);
}
int gbahp_decode_hello_ack(const uint8_t *pl, size_t len, gbahp_hello_ack *m)
{
	R r;
	DEC_ARGS(pl, len, m);
	r_init(&r, pl, len);
	m->status = r_u8(&r);
	r_hello_body(&r, &m->hello);
	if (!r.err && m->status > GBAHP_ACK_BUSY) return GBAHP_E_RANGE;
	return r_done(&r);
}

int gbahp_hello_compat(const gbahp_hello *local, const gbahp_hello *remote)
{
	if (local->proto_version != remote->proto_version) return GBAHP_ACK_PROTO_MISMATCH;
	if (local->core_version.n != remote->core_version.n ||
	    (local->core_version.n && memcmp(local->core_version.p, remote->core_version.p, local->core_version.n)))
		return GBAHP_ACK_CORE_MISMATCH;
	return GBAHP_ACK_OK;
}

/* ------------------------------------------------------------ LIBRARY */

typedef struct { const gbahp_lib_entry *e; uint32_t count; } lib_in;
static void w_library(W *w, const void *m)
{
	const lib_in *l = (const lib_in *)m;
	uint32_t i;
	if (l->count && !l->e) { w->err = 1; return; }
	w_u32(w, l->count);
	for (i = 0; i < l->count && !w->err; i++) {
		const gbahp_lib_entry *e = &l->e[i];
		w_raw(w, e->sha1, 20);
		w_u64(w, e->size);
		w_u8(w, e->has_data);
		w_raw(w, e->version_dev, 16); w_u64(w, e->version);
		w_raw(w, e->base_dev, 16);    w_u64(w, e->base);
		w_u64(w, e->timestamp_ms);
		w_u32(w, e->frame);
	}
}
size_t gbahp_encode_library(const gbahp_lib_entry *e, uint32_t count, uint8_t *out, size_t cap)
{
	lib_in l;
	if (count > (GBAHP_MAX_PAYLOAD - 4) / GBAHP_LIB_ENTRY_BYTES) return 0;
	l.e = e; l.count = count;
	return encode(GBAHP_MSG_LIBRARY, w_library, &l, out, cap);
}
int gbahp_decode_library(const uint8_t *pl, size_t len, gbahp_library *m)
{
	R r;
	uint64_t need;
	DEC_ARGS(pl, len, m);
	r_init(&r, pl, len);
	m->count = r_u32(&r);
	if (r.err) return r.err;
	need = (uint64_t)m->count * GBAHP_LIB_ENTRY_BYTES;
	if ((uint64_t)(len - 4) < need) return GBAHP_E_TRUNC;
	if ((uint64_t)(len - 4) > need) return GBAHP_E_TRAILING;
	m->raw = pl + 4;
	return GBAHP_OK;
}
void gbahp_library_get(const gbahp_library *m, uint32_t i, gbahp_lib_entry *e)
{
	const uint8_t *q = m->raw + (size_t)i * GBAHP_LIB_ENTRY_BYTES;
	memcpy(e->sha1, q, 20);
	e->size = get64(q + 20);
	e->has_data = q[28] ? 1 : 0;
	memcpy(e->version_dev, q + 29, 16); e->version = get64(q + 45);
	memcpy(e->base_dev, q + 53, 16);    e->base = get64(q + 69);
	e->timestamp_ms = get64(q + 77);
	e->frame = get32(q + 85);
}

/* ------------------------------------------------------------ ROM */

static void w_rom_request(W *w, const void *m) { w_raw(w, ((const gbahp_rom_request *)m)->sha1, 20); }
size_t gbahp_encode_rom_request(const gbahp_rom_request *m, uint8_t *out, size_t cap)
{ return encode(GBAHP_MSG_ROM_REQUEST, w_rom_request, m, out, cap); }
int gbahp_decode_rom_request(const uint8_t *pl, size_t len, gbahp_rom_request *m)
{
	R r;
	DEC_ARGS(pl, len, m);
	r_init(&r, pl, len);
	r_copy(&r, m->sha1, 20);
	return r_done(&r);
}

static void w_rom_push(W *w, const void *mm)
{
	const gbahp_rom_push *m = (const gbahp_rom_push *)mm;
	if (m->data_len && !m->data) { w->err = 1; return; }
	w_raw(w, m->sha1, 20);
	w_u64(w, m->total_size);
	w_u64(w, m->offset);
	w_raw(w, m->data, m->data_len);
}
size_t gbahp_encode_rom_push(const gbahp_rom_push *m, uint8_t *out, size_t cap)
{
	if (m && (m->offset > m->total_size || m->data_len > m->total_size - m->offset)) return 0;
	return encode(GBAHP_MSG_ROM_PUSH, w_rom_push, m, out, cap);
}
int gbahp_decode_rom_push(const uint8_t *pl, size_t len, gbahp_rom_push *m)
{
	R r;
	DEC_ARGS(pl, len, m);
	r_init(&r, pl, len);
	r_copy(&r, m->sha1, 20);
	m->total_size = r_u64(&r);
	m->offset = r_u64(&r);
	if (r.err) return r.err;
	m->data_len = (uint32_t)(len - r.pos);
	m->data = pl + r.pos;
	if (m->offset > m->total_size || m->data_len > m->total_size - m->offset) return GBAHP_E_RANGE;
	return GBAHP_OK;
}

/* ------------------------------------------------------------ STATE */

static int push_flags_ok(uint8_t f)
{
	if (f & ~(GBAHP_PUSH_MIRROR | GBAHP_PUSH_RESOLVED | GBAHP_PUSH_HANDOFF)) return 0;
	if ((f & GBAHP_PUSH_MIRROR) && (f & (GBAHP_PUSH_RESOLVED | GBAHP_PUSH_HANDOFF))) return 0;
	return 1;
}

static void w_state_push(W *w, const void *mm)
{
	const gbahp_state_push *m = (const gbahp_state_push *)mm;
	if ((m->state_len && !m->state) || (m->sav_len && !m->sav) || (m->thumb_len && !m->thumb) ||
	    (m->thumb_len != 0 && m->thumb_len != GBAHP_THUMB_BYTES) || !push_flags_ok(m->flags)) { w->err = 1; return; }
	w_raw(w, m->sha1_rom, 20);
	w_u64(w, m->version);
	w_raw(w, m->origin_device, 16);
	w_raw(w, m->base_device, 16);
	w_u64(w, m->base);
	w_u8(w, m->flags);
	w_u64(w, m->timestamp_ms);
	w_u32(w, m->frame);
	w_u32(w, m->state_len);
	w_u32(w, m->sav_len);
	w_u32(w, m->thumb_len);
	w_raw(w, m->state, m->state_len);
	w_raw(w, m->sav, m->sav_len);
	w_raw(w, m->thumb, m->thumb_len);
}
size_t gbahp_encode_state_push(const gbahp_state_push *m, uint8_t *out, size_t cap)
{ return encode(GBAHP_MSG_STATE_PUSH, w_state_push, m, out, cap); }
int gbahp_decode_state_push(const uint8_t *pl, size_t len, gbahp_state_push *m)
{
	R r;
	size_t left;
	DEC_ARGS(pl, len, m);
	r_init(&r, pl, len);
	r_copy(&r, m->sha1_rom, 20);
	m->version = r_u64(&r);
	r_copy(&r, m->origin_device, 16);
	r_copy(&r, m->base_device, 16);
	m->base = r_u64(&r);
	m->flags = r_u8(&r);
	m->timestamp_ms = r_u64(&r);
	m->frame = r_u32(&r);
	m->state_len = r_u32(&r);
	m->sav_len = r_u32(&r);
	m->thumb_len = r_u32(&r);
	if (r.err) return r.err;
	if (m->thumb_len != 0 && m->thumb_len != GBAHP_THUMB_BYTES) return GBAHP_E_RANGE;
	if (!push_flags_ok(m->flags)) return GBAHP_E_RANGE;
	/* u32 sizes, size_t may be 32 bits: subtract from what is left, never add */
	left = len - r.pos;
	if (m->state_len > left) return GBAHP_E_TRUNC;
	left -= m->state_len;
	if (m->sav_len > left) return GBAHP_E_TRUNC;
	left -= m->sav_len;
	if (m->thumb_len > left) return GBAHP_E_TRUNC;
	m->state = r_raw(&r, m->state_len);
	m->sav = r_raw(&r, m->sav_len);
	m->thumb = r_raw(&r, m->thumb_len);
	return r_done(&r);
}

static void w_state_ack(W *w, const void *mm)
{
	const gbahp_state_ack *m = (const gbahp_state_ack *)mm;
	w_raw(w, m->sha1_rom, 20);
	w_u64(w, m->version);
	w_raw(w, m->origin_device, 16);
	w_u8(w, m->status);
}
size_t gbahp_encode_state_ack(const gbahp_state_ack *m, uint8_t *out, size_t cap)
{ return encode(GBAHP_MSG_STATE_ACK, w_state_ack, m, out, cap); }
int gbahp_decode_state_ack(const uint8_t *pl, size_t len, gbahp_state_ack *m)
{
	R r;
	DEC_ARGS(pl, len, m);
	r_init(&r, pl, len);
	r_copy(&r, m->sha1_rom, 20);
	m->version = r_u64(&r);
	r_copy(&r, m->origin_device, 16);
	m->status = r_u8(&r);
	if (!r.err && m->status > GBAHP_STATE_ACK_STALE) return GBAHP_E_RANGE;
	return r_done(&r);
}

/* ------------------------------------------------------------ INPUT / PING / BYE */

static void w_input(W *w, const void *mm)
{
	const gbahp_input *m = (const gbahp_input *)mm;
	if (m->keys & ~0x3FFu) { w->err = 1; return; }
	w_u32(w, m->seq);
	w_u16(w, m->keys);
}
size_t gbahp_encode_input(const gbahp_input *m, uint8_t *out, size_t cap)
{ return encode(GBAHP_MSG_INPUT, w_input, m, out, cap); }
int gbahp_decode_input(const uint8_t *pl, size_t len, gbahp_input *m)
{
	R r;
	DEC_ARGS(pl, len, m);
	r_init(&r, pl, len);
	m->seq = r_u32(&r);
	m->keys = r_u16(&r);
	if (!r.err && (m->keys & ~0x3FFu)) return GBAHP_E_RANGE;
	return r_done(&r);
}

static void w_ping(W *w, const void *m) { w_u64(w, ((const gbahp_ping *)m)->token); }
size_t gbahp_encode_ping(const gbahp_ping *m, uint8_t *out, size_t cap)
{ return encode(GBAHP_MSG_PING, w_ping, m, out, cap); }
size_t gbahp_encode_pong(const gbahp_ping *m, uint8_t *out, size_t cap)
{ return encode(GBAHP_MSG_PONG, w_ping, m, out, cap); }
int gbahp_decode_ping(const uint8_t *pl, size_t len, gbahp_ping *m)
{
	R r;
	DEC_ARGS(pl, len, m);
	r_init(&r, pl, len);
	m->token = r_u64(&r);
	return r_done(&r);
}

static void w_bye(W *w, const void *m) { w_u16(w, ((const gbahp_bye *)m)->reason); }
size_t gbahp_encode_bye(const gbahp_bye *m, uint8_t *out, size_t cap)
{ return encode(GBAHP_MSG_BYE, w_bye, m, out, cap); }
int gbahp_decode_bye(const uint8_t *pl, size_t len, gbahp_bye *m)
{
	R r;
	DEC_ARGS(pl, len, m);
	r_init(&r, pl, len);
	m->reason = r_u16(&r);
	return r_done(&r);
}

/* ------------------------------------------------------------ CONTROL / THUMB */

static void w_control(W *w, const void *mm)
{
	const gbahp_control *m = (const gbahp_control *)mm;
	w_u8(w, m->cmd);
	w_raw(w, m->sha1, 20);
	w_u32(w, m->arg);
}
size_t gbahp_encode_control(const gbahp_control *m, uint8_t *out, size_t cap)
{
	if (m && (m->cmd < GBAHP_CTL_RETURN || m->cmd > GBAHP_CTL_ROM_UNAVAILABLE)) return 0;
	return encode(GBAHP_MSG_CONTROL, w_control, m, out, cap);
}
int gbahp_decode_control(const uint8_t *pl, size_t len, gbahp_control *m)
{
	R r;
	DEC_ARGS(pl, len, m);
	r_init(&r, pl, len);
	m->cmd = r_u8(&r);
	r_copy(&r, m->sha1, 20);
	m->arg = r_u32(&r);
	if (!r.err && (m->cmd < GBAHP_CTL_RETURN || m->cmd > GBAHP_CTL_ROM_UNAVAILABLE)) return GBAHP_E_RANGE;
	return r_done(&r);
}

static void w_thumb(W *w, const void *mm)
{
	const gbahp_thumb *m = (const gbahp_thumb *)mm;
	if ((m->thumb_len && !m->thumb) || (m->thumb_len != 0 && m->thumb_len != GBAHP_THUMB_BYTES)) { w->err = 1; return; }
	w_raw(w, m->sha1, 20);
	w_u32(w, m->thumb_len);
	w_raw(w, m->thumb, m->thumb_len);
}
size_t gbahp_encode_thumb(const gbahp_thumb *m, uint8_t *out, size_t cap)
{ return encode(GBAHP_MSG_THUMB, w_thumb, m, out, cap); }
int gbahp_decode_thumb(const uint8_t *pl, size_t len, gbahp_thumb *m)
{
	R r;
	DEC_ARGS(pl, len, m);
	r_init(&r, pl, len);
	r_copy(&r, m->sha1, 20);
	m->thumb_len = r_u32(&r);
	if (r.err) return r.err;
	if (m->thumb_len != 0 && m->thumb_len != GBAHP_THUMB_BYTES) return GBAHP_E_RANGE;
	if (len - r.pos < m->thumb_len) return GBAHP_E_TRUNC;
	m->thumb = r_raw(&r, m->thumb_len);
	return r_done(&r);
}
