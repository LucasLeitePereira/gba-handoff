/* Unit tests of proto/gbahp: roundtrips, incremental parser, error paths, fuzz. */
#include "gbahp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint64_t rng_s = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
	rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17;
	return (uint32_t)(rng_s >> 16);
}

/* Feeds a whole stream in pieces of `step` bytes (0 = random sizes), collects the
 * payload of the n-th frame. Returns number of frames, or the parser error. */
typedef struct { uint8_t type; uint32_t len; uint8_t *copy; } got_t;
static int run_parser(const uint8_t *buf, size_t n, size_t step, got_t *got, int max_got, gbahp_parser *p)
{
	size_t pos = 0;
	int frames = 0;
	while (pos < n) {
		size_t piece = step ? step : 1 + rnd() % 40;
		size_t used = 0;
		gbahp_frame f;
		int r;
		if (piece > n - pos) piece = n - pos;
		/* a piece may contain several frames: loop on the remainder */
		while (piece) {
			r = gbahp_parser_feed(p, buf + pos, piece, &used, &f);
			if (r < 0) return r;
			pos += used; piece -= used;
			if (r == 1) {
				if (frames < max_got) {
					got[frames].type = f.type;
					got[frames].len = f.len;
					got[frames].copy = (uint8_t *)malloc(f.len ? f.len : 1);
					if (f.len) memcpy(got[frames].copy, f.payload, f.len);
				}
				frames++;
			} else break;
		}
	}
	return frames;
}

static void test_crc_and_layout(void)
{
	uint8_t b[64];
	gbahp_ping pg = { 0x1122334455667788ull };
	size_t n;
	CHECK(gbahp_crc32("123456789", 9) == 0xCBF43926u);   /* standard check value */
	CHECK(gbahp_crc32("", 0) == 0);
	n = gbahp_encode_ping(&pg, b, sizeof b);
	CHECK(n == 16 + 8);
	CHECK(!memcmp(b, "GBAH", 4) && b[4] == GBAHP_VERSION && b[5] == GBAHP_MSG_PING);
	CHECK(b[6] == 0 && b[7] == 0);
	CHECK(b[8] == 8 && b[9] == 0 && b[10] == 0 && b[11] == 0);                 /* len LE */
	CHECK(b[16] == 0x88 && b[23] == 0x11);                                       /* payload LE */
	CHECK(b[12] == (gbahp_crc32(b + 16, 8) & 0xFF));
	CHECK(gbahp_encode_ping(&pg, NULL, 0) == 24);                                /* size query */
	CHECK(gbahp_encode_ping(&pg, b, 23) == 0);                                   /* too small */
}

static void test_roundtrip_all(void)
{
	uint8_t *thumb = (uint8_t *)malloc(GBAHP_THUMB_BYTES);
	uint8_t state[5000], sav[300];
	static uint8_t stream[1 << 20];
	size_t off = 0, n;
	got_t got[20];
	gbahp_parser p;
	int i, frames;
	gbahp_hello h = { GBAHP_VERSION, {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16}, {"0.10.5", 6}, {"Pixel \xC3\xA9", 8} };
	gbahp_hello_ack ack; gbahp_lib_entry lib[3]; gbahp_rom_request rr; gbahp_rom_push rp;
	gbahp_state_push sp; gbahp_state_ack sa; gbahp_input in = { 0xDEADBEEF, 0x3FF };
	gbahp_ping pg = { 42 }; gbahp_bye bye = { 7 };
	gbahp_control ctl; gbahp_thumb th;

	for (i = 0; i < (int)sizeof state; i++) state[i] = (uint8_t)rnd();
	for (i = 0; i < (int)sizeof sav; i++) sav[i] = (uint8_t)rnd();
	for (i = 0; i < (int)GBAHP_THUMB_BYTES; i++) thumb[i] = (uint8_t)rnd();
	for (i = 0; i < 3; i++) { memset(lib[i].sha1, 0x10 + i, 20); lib[i].size = 0x100000000ull * (i + 1) + i;
		lib[i].has_data = (uint8_t)(i != 1); memset(lib[i].version_dev, 0x20 + i, 16); lib[i].version = 100u + (uint64_t)i;
		memset(lib[i].base_dev, 0x30 + i, 16); lib[i].base = 50u + (uint64_t)i; lib[i].timestamp_ms = 1760000000000ull + (uint64_t)i; lib[i].frame = 7000u + (uint32_t)i; }
	memset(&ack, 0, sizeof ack); ack.status = GBAHP_ACK_CORE_MISMATCH; ack.hello = h;
	memset(rr.sha1, 0xAB, 20);
	memset(&rp, 0, sizeof rp); memset(rp.sha1, 0xCD, 20); rp.total_size = 1000; rp.offset = 600; rp.data = sav; rp.data_len = 300;
	memset(&sp, 0, sizeof sp); memset(sp.sha1_rom, 0xEF, 20); sp.version = 0x0102030405060708ull;
	memset(sp.origin_device, 0x77, 16); memset(sp.base_device, 0x66, 16); sp.base = 0x0A0B0C0D0E0F1011ull; sp.flags = GBAHP_PUSH_HANDOFF | GBAHP_PUSH_RESOLVED; sp.timestamp_ms = 1760000000123ull; sp.frame = 123456;
	sp.state = state; sp.state_len = sizeof state; sp.sav = sav; sp.sav_len = sizeof sav;
	sp.thumb = thumb; sp.thumb_len = GBAHP_THUMB_BYTES;
	memset(&sa, 0, sizeof sa); memset(sa.sha1_rom, 0x55, 20); sa.version = 9; memset(sa.origin_device, 0x44, 16); sa.status = GBAHP_STATE_ACK_STALE;
	memset(&ctl, 0, sizeof ctl); ctl.cmd = GBAHP_CTL_PC_STATUS; memset(ctl.sha1, 0x99, 20); ctl.arg = 1;
	memset(&th, 0, sizeof th); memset(th.sha1, 0x98, 20); th.thumb = thumb; th.thumb_len = GBAHP_THUMB_BYTES;

	n = gbahp_encode_hello(&h, stream + off, sizeof stream - off); CHECK(n); off += n;
	n = gbahp_encode_hello_ack(&ack, stream + off, sizeof stream - off); CHECK(n); off += n;
	n = gbahp_encode_library(lib, 3, stream + off, sizeof stream - off); CHECK(n); off += n;
	n = gbahp_encode_library(NULL, 0, stream + off, sizeof stream - off); CHECK(n); off += n;
	n = gbahp_encode_rom_request(&rr, stream + off, sizeof stream - off); CHECK(n); off += n;
	n = gbahp_encode_rom_push(&rp, stream + off, sizeof stream - off); CHECK(n); off += n;
	n = gbahp_encode_state_push(&sp, stream + off, sizeof stream - off); CHECK(n); off += n;
	n = gbahp_encode_state_ack(&sa, stream + off, sizeof stream - off); CHECK(n); off += n;
	n = gbahp_encode_input(&in, stream + off, sizeof stream - off); CHECK(n); off += n;
	n = gbahp_encode_ping(&pg, stream + off, sizeof stream - off); CHECK(n); off += n;
	n = gbahp_encode_pong(&pg, stream + off, sizeof stream - off); CHECK(n); off += n;
	n = gbahp_encode_bye(&bye, stream + off, sizeof stream - off); CHECK(n); off += n;
	n = gbahp_encode_control(&ctl, stream + off, sizeof stream - off); CHECK(n); off += n;
	n = gbahp_encode_thumb(&th, stream + off, sizeof stream - off); CHECK(n); off += n;

	/* the same stream through 1-byte, 7-byte, random and whole-buffer feeding */
	{
		static const size_t steps[] = { 1, 7, 0, 100000 };
		int s;
		for (s = 0; s < 4; s++) {
			gbahp_parser_init(&p);
			frames = run_parser(stream, off, steps[s], got, 20, &p);
			CHECK(frames == 14);
			if (frames != 14) { gbahp_parser_free(&p); continue; }
			{
				gbahp_hello h2; gbahp_hello_ack a2; gbahp_library l2; gbahp_lib_entry e;
				gbahp_rom_request r2; gbahp_rom_push rp2; gbahp_state_push sp2; gbahp_state_ack sa2;
				gbahp_input i2; gbahp_ping p2; gbahp_bye b2; gbahp_control c2; gbahp_thumb t2;
				CHECK(got[0].type == GBAHP_MSG_HELLO);
				CHECK(gbahp_decode_hello(got[0].copy, got[0].len, &h2) == 0);
				CHECK(h2.proto_version == GBAHP_VERSION && !memcmp(h2.device_id, h.device_id, 16));
				CHECK(h2.core_version.n == 6 && !memcmp(h2.core_version.p, "0.10.5", 6));
				CHECK(h2.name.n == 8 && !memcmp(h2.name.p, h.name.p, 8));
				CHECK(gbahp_decode_hello_ack(got[1].copy, got[1].len, &a2) == 0);
				CHECK(a2.status == GBAHP_ACK_CORE_MISMATCH && a2.hello.name.n == 8);
				CHECK(gbahp_decode_library(got[2].copy, got[2].len, &l2) == 0 && l2.count == 3);
				for (i = 0; i < 3; i++) {
					gbahp_library_get(&l2, (uint32_t)i, &e);
					CHECK(!memcmp(e.sha1, lib[i].sha1, 20) && e.size == lib[i].size);
					CHECK(e.has_data == lib[i].has_data && !memcmp(e.version_dev, lib[i].version_dev, 16) && e.version == lib[i].version);
					CHECK(!memcmp(e.base_dev, lib[i].base_dev, 16) && e.base == lib[i].base);
					CHECK(e.timestamp_ms == lib[i].timestamp_ms && e.frame == lib[i].frame);
				}
				CHECK(gbahp_decode_library(got[3].copy, got[3].len, &l2) == 0 && l2.count == 0);
				CHECK(gbahp_decode_rom_request(got[4].copy, got[4].len, &r2) == 0 && !memcmp(r2.sha1, rr.sha1, 20));
				CHECK(gbahp_decode_rom_push(got[5].copy, got[5].len, &rp2) == 0);
				CHECK(rp2.total_size == 1000 && rp2.offset == 600 && rp2.data_len == 300 && !memcmp(rp2.data, sav, 300));
				CHECK(gbahp_decode_state_push(got[6].copy, got[6].len, &sp2) == 0);
				CHECK(!memcmp(sp2.sha1_rom, sp.sha1_rom, 20) && sp2.version == sp.version);
				CHECK(!memcmp(sp2.origin_device, sp.origin_device, 16) && sp2.timestamp_ms == sp.timestamp_ms);
				CHECK(!memcmp(sp2.base_device, sp.base_device, 16) && sp2.base == sp.base && sp2.flags == sp.flags);
				CHECK(sp2.frame == sp.frame && sp2.state_len == sizeof state && sp2.sav_len == sizeof sav);
				CHECK(!memcmp(sp2.state, state, sizeof state) && !memcmp(sp2.sav, sav, sizeof sav));
				CHECK(sp2.thumb_len == GBAHP_THUMB_BYTES && !memcmp(sp2.thumb, thumb, GBAHP_THUMB_BYTES));
				CHECK(gbahp_decode_state_ack(got[7].copy, got[7].len, &sa2) == 0);
				CHECK(sa2.version == 9 && sa2.status == GBAHP_STATE_ACK_STALE && !memcmp(sa2.sha1_rom, sa.sha1_rom, 20) && !memcmp(sa2.origin_device, sa.origin_device, 16));
				CHECK(gbahp_decode_input(got[8].copy, got[8].len, &i2) == 0 && i2.seq == 0xDEADBEEF && i2.keys == 0x3FF);
				CHECK(got[9].type == GBAHP_MSG_PING && got[10].type == GBAHP_MSG_PONG);
				CHECK(gbahp_decode_ping(got[9].copy, got[9].len, &p2) == 0 && p2.token == 42);
				CHECK(gbahp_decode_ping(got[10].copy, got[10].len, &p2) == 0 && p2.token == 42);
				CHECK(gbahp_decode_bye(got[11].copy, got[11].len, &b2) == 0 && b2.reason == 7);
				CHECK(gbahp_decode_control(got[12].copy, got[12].len, &c2) == 0 && c2.cmd == GBAHP_CTL_PC_STATUS && c2.arg == 1 && !memcmp(c2.sha1, ctl.sha1, 20));
				CHECK(gbahp_decode_thumb(got[13].copy, got[13].len, &t2) == 0 && t2.thumb_len == GBAHP_THUMB_BYTES && !memcmp(t2.thumb, thumb, t2.thumb_len));
			}
			for (i = 0; i < frames; i++) free(got[i].copy);
			gbahp_parser_free(&p);
		}
	}

	/* STATE_PUSH without thumbnail and without sav */
	sp.thumb = NULL; sp.thumb_len = 0; sp.sav = NULL; sp.sav_len = 0;
	n = gbahp_encode_state_push(&sp, stream, sizeof stream);
	CHECK(n > 0);
	{
		gbahp_state_push s2;
		CHECK(gbahp_decode_state_push(stream + 16, n - 16, &s2) == 0 && s2.thumb_len == 0 && s2.sav_len == 0);
	}
	free(thumb);
}

static void test_small_messages_do_not_allocate(void)
{
	uint8_t b[512];
	gbahp_parser p;
	gbahp_hello h;
	gbahp_frame f;
	size_t used, n;
	int i;
	memset(&h, 0, sizeof h);
	h.proto_version = GBAHP_VERSION; h.core_version = gbahp_str_c("0.10.5"); h.name = gbahp_str_c("x");
	n = gbahp_encode_hello(&h, b, sizeof b);
	gbahp_parser_init(&p);
	for (i = 0; i < 100; i++) {
		size_t pos = 0;
		int r = 0;
		while (pos < n) { r = gbahp_parser_feed(&p, b + pos, 1, &used, &f); pos += used; }
		CHECK(r == 1 && f.type == GBAHP_MSG_HELLO);
	}
	CHECK(p.heap == NULL);
	gbahp_parser_free(&p);
}

static void test_errors(void)
{
	uint8_t b[256], c[256];
	gbahp_parser p;
	gbahp_frame f;
	size_t used, n;
	gbahp_ping pg = { 1 };
	gbahp_input in = { 1, 1 };

	/* bad CRC (flip a payload bit) */
	n = gbahp_encode_ping(&pg, b, sizeof b);
	memcpy(c, b, n); c[20] ^= 1;
	gbahp_parser_init(&p);
	CHECK(gbahp_parser_feed(&p, c, n, &used, &f) == GBAHP_E_CRC);
	CHECK(gbahp_parser_feed(&p, b, n, &used, &f) == GBAHP_E_POISONED);   /* stays failed */
	gbahp_parser_free(&p);
	/* bad CRC field */
	memcpy(c, b, n); c[12] ^= 0x80;
	gbahp_parser_init(&p);
	CHECK(gbahp_parser_feed(&p, c, n, &used, &f) == GBAHP_E_CRC);
	/* bad magic */
	memcpy(c, b, n); c[0] = 'X';
	gbahp_parser_init(&p);
	CHECK(gbahp_parser_feed(&p, c, n, &used, &f) == GBAHP_E_MAGIC);
	/* payload above the limit: rejected from the header alone, nothing allocated */
	memcpy(c, b, 16);
	c[8] = 1; c[9] = 0; c[10] = 0; c[11] = 4;   /* 64 MiB + 1 */
	gbahp_parser_init(&p);
	CHECK(gbahp_parser_feed(&p, c, 16, &used, &f) == GBAHP_E_TOO_BIG);
	CHECK(p.heap == NULL);
	/* exactly at the limit is accepted (header-wise) and costs no memory until data arrives */
	c[8] = 0; c[9] = 0; c[10] = 0; c[11] = 4;
	gbahp_parser_init(&p);
	CHECK(gbahp_parser_feed(&p, c, 16, &used, &f) == 0 && p.heap == NULL);
	gbahp_parser_free(&p);
	/* encoder refuses payload above the limit */
	CHECK(gbahp_frame_encode(1, 0, b, GBAHP_MAX_PAYLOAD + 1, c, sizeof c) == 0);

	/* decoder strictness */
	n = gbahp_encode_input(&in, b, sizeof b);
	{
		gbahp_input o;
		CHECK(gbahp_decode_input(b + 16, n - 16 - 1, &o) == GBAHP_E_TRUNC);
		b[n] = 0;
		CHECK(gbahp_decode_input(b + 16, n - 16 + 1, &o) == GBAHP_E_TRAILING);
		in.keys = 0x400;
		CHECK(gbahp_encode_input(&in, b, sizeof b) == 0);                  /* bit 10 does not exist */
	}
	{
		gbahp_hello h; uint8_t big[300]; gbahp_hello o;
		memset(&h, 0, sizeof h); memset(big, 'a', sizeof big);
		h.name.p = (const char *)big; h.name.n = 256;
		CHECK(gbahp_encode_hello(&h, b, sizeof b) == 0);                   /* name too long */
		h.name.n = 255; h.core_version = gbahp_str_c("v");
		n = gbahp_encode_hello(&h, NULL, 0);
		CHECK(n == 16 + 2 + 16 + (1 + 1) + (1 + 255));
		/* length prefix pointing beyond the payload */
		h.name = gbahp_str_c("ab");
		n = gbahp_encode_hello(&h, b, sizeof b);
		b[16 + 2 + 16 + 2] = 200;
		CHECK(gbahp_decode_hello(b + 16, n - 16, &o) == GBAHP_E_TRUNC);
	}
	{
		gbahp_library l; uint8_t pl[4] = { 5, 0, 0, 0 };                    /* claims 5 entries, has none */
		CHECK(gbahp_decode_library(pl, 4, &l) == GBAHP_E_TRUNC);
		pl[3] = 0xFF;                                                       /* huge count must not overflow */
		CHECK(gbahp_decode_library(pl, 4, &l) == GBAHP_E_TRUNC);
	}
	{
		gbahp_rom_push rp; memset(&rp, 0, sizeof rp);
		rp.total_size = 10; rp.offset = 8; rp.data = b; rp.data_len = 5;    /* runs past the end */
		CHECK(gbahp_encode_rom_push(&rp, c, sizeof c) == 0);
	}
	{
		gbahp_state_push s; uint8_t pl[100]; memset(&s, 0, sizeof s);
		s.state = pl; s.state_len = 100; s.thumb = pl; s.thumb_len = 100;   /* thumb must be 0 or 240*160*4 */
		CHECK(gbahp_encode_state_push(&s, NULL, 0) == 0);
	}
	{   /* invalid flag combinations and control commands */
		gbahp_state_push s; gbahp_control c; uint8_t pl[8];
		memset(&s, 0, sizeof s); s.state = pl; s.state_len = 8;
		s.flags = GBAHP_PUSH_MIRROR | GBAHP_PUSH_HANDOFF; CHECK(gbahp_encode_state_push(&s, NULL, 0) == 0);
		s.flags = 0x80; CHECK(gbahp_encode_state_push(&s, NULL, 0) == 0);
		s.flags = GBAHP_PUSH_MIRROR; CHECK(gbahp_encode_state_push(&s, NULL, 0) != 0);
		memset(&c, 0, sizeof c); c.cmd = 0; CHECK(gbahp_encode_control(&c, NULL, 0) == 0);
		c.cmd = 99; CHECK(gbahp_encode_control(&c, NULL, 0) == 0);
		c.cmd = GBAHP_CTL_LAST + 1; CHECK(gbahp_encode_control(&c, NULL, 0) == 0);
		/* v3 codes: valid with an argument; a frame with an unknown command is refused on decode */
		{
			uint8_t buf[64], pl2[64];
			size_t n, cmd;
			gbahp_control d;
			for (cmd = GBAHP_CTL_PLACE_PC; cmd <= GBAHP_CTL_SETTINGS_SET; cmd++) {
				memset(&c, 0, sizeof c); c.cmd = (uint8_t)cmd; c.arg = GBAHP_SET_PREFER_NEWEST; memset(c.sha1, 0x31, 20);
				n = gbahp_encode_control(&c, buf, sizeof buf);
				CHECK(n == GBAHP_HEADER_SIZE + 25);
				CHECK(gbahp_decode_control(buf + GBAHP_HEADER_SIZE, n - GBAHP_HEADER_SIZE, &d) == 0);
				CHECK(d.cmd == cmd && d.arg == GBAHP_SET_PREFER_NEWEST && !memcmp(d.sha1, c.sha1, 20));
			}
			memcpy(pl2, buf + GBAHP_HEADER_SIZE, 25);
			pl2[0] = GBAHP_CTL_LAST + 1;
			CHECK(gbahp_decode_control(pl2, 25, &d) == GBAHP_E_RANGE);   /* negative control: the range check is live */
		}
	}
	{
		gbahp_state_push o; uint8_t pl[20 + 8 + 16 + 16 + 8 + 1 + 8 + 4 + 12];
		memset(pl, 0, sizeof pl);
		pl[sizeof pl - 12] = 0xFF; pl[sizeof pl - 11] = 0xFF; pl[sizeof pl - 10] = 0xFF; pl[sizeof pl - 9] = 0xFF; /* state_len = 4G-1 */
		CHECK(gbahp_decode_state_push(pl, sizeof pl, &o) == GBAHP_E_TRUNC);
	}
}

static void test_compat(void)
{
	gbahp_hello a, b;
	memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
	a.proto_version = b.proto_version = GBAHP_VERSION;
	a.core_version = gbahp_str_c("0.10.5"); b.core_version = gbahp_str_c("0.10.5");
	CHECK(gbahp_hello_compat(&a, &b) == GBAHP_ACK_OK);
	b.core_version = gbahp_str_c("0.10.4");
	CHECK(gbahp_hello_compat(&a, &b) == GBAHP_ACK_CORE_MISMATCH);
	b.core_version = gbahp_str_c("0.10.55");
	CHECK(gbahp_hello_compat(&a, &b) == GBAHP_ACK_CORE_MISMATCH);
	b.core_version = gbahp_str_c("");
	CHECK(gbahp_hello_compat(&a, &b) == GBAHP_ACK_CORE_MISMATCH);
	b.core_version = a.core_version; b.proto_version = GBAHP_VERSION + 1;
	CHECK(gbahp_hello_compat(&a, &b) == GBAHP_ACK_PROTO_MISMATCH);
	/* the previous release (v2: the phone that resumed by itself after the cable was pulled) is refused */
	CHECK(GBAHP_VERSION == 3);
	b.proto_version = 2;
	CHECK(gbahp_hello_compat(&a, &b) == GBAHP_ACK_PROTO_MISMATCH);
}

static void test_large_payload_chunked(void)
{
	size_t len = 3u * 1024 * 1024 + 123, n, pos = 0, used;
	uint8_t *pl = (uint8_t *)malloc(len), *fr = (uint8_t *)malloc(len + 16);
	gbahp_parser p;
	gbahp_frame f;
	int r = 0;
	size_t i;
	for (i = 0; i < len; i++) pl[i] = (uint8_t)(i * 31 + (i >> 8));
	n = gbahp_frame_encode(99, 0, pl, len, fr, len + 16);
	CHECK(n == len + 16);
	gbahp_parser_init(&p);
	while (pos < n && r == 0) {
		size_t piece = 1 + rnd() % 9000;
		if (piece > n - pos) piece = n - pos;
		r = gbahp_parser_feed(&p, fr + pos, piece, &used, &f);
		pos += used;
	}
	CHECK(r == 1 && f.type == 99 && f.len == len && !memcmp(f.payload, pl, len));
	/* next frame after a big one works and the >1 MiB buffer is released */
	{
		uint8_t pf[24]; gbahp_ping pg = { 5 };
		size_t m = gbahp_encode_ping(&pg, pf, sizeof pf);
		CHECK(gbahp_parser_feed(&p, pf, m, &used, &f) == 1 && f.type == GBAHP_MSG_PING);
		CHECK(p.heap == NULL);
	}
	gbahp_parser_free(&p);
	free(pl); free(fr);
}

static void test_zero_length_frame(void)
{
	uint8_t b[16];
	gbahp_parser p;
	gbahp_frame f;
	size_t used;
	CHECK(gbahp_frame_encode(77, 0, NULL, 0, b, sizeof b) == 16);
	gbahp_parser_init(&p);
	CHECK(gbahp_parser_feed(&p, b, 16, &used, &f) == 1 && used == 16 && f.len == 0 && f.type == 77);
}

/* Fuzz: nothing may crash or read out of bounds (run with ASan/UBSan: proto_test_san). */
static void test_fuzz(void)
{
	uint8_t seed[4096], buf[8192];
	size_t n, i, it;
	gbahp_ping pg = { 1 };
	gbahp_hello h;
	memset(&h, 0, sizeof h);
	h.core_version = gbahp_str_c("0.10.5"); h.name = gbahp_str_c("fuzz");
	n = gbahp_encode_hello(&h, seed, sizeof seed);
	n += gbahp_encode_ping(&pg, seed + n, sizeof seed - n);

	for (it = 0; it < 20000; it++) {
		gbahp_parser p;
		size_t len, pos = 0, used;
		gbahp_frame f;
		int r;
		if (rnd() & 1) {                       /* pure noise */
			len = rnd() % sizeof buf;
			for (i = 0; i < len; i++) buf[i] = (uint8_t)rnd();
		} else {                               /* mutated valid frames */
			len = n;
			memcpy(buf, seed, n);
			for (i = rnd() % 5; i > 0; i--) buf[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8));
		}
		gbahp_parser_init(&p);
		while (pos < len) {
			size_t piece = 1 + rnd() % 64;
			if (piece > len - pos) piece = len - pos;
			r = gbahp_parser_feed(&p, buf + pos, piece, &used, &f);
			if (r < 0) break;
			pos += used;
		}
		gbahp_parser_free(&p);
	}
	/* decoders on random payloads (exercise every length-prefix path) */
	for (it = 0; it < 50000; it++) {
		size_t len = rnd() % 200;
		union { gbahp_hello h; gbahp_hello_ack a; gbahp_library l; gbahp_rom_request rr; gbahp_rom_push rp;
		        gbahp_state_push sp; gbahp_state_ack sa; gbahp_input in; gbahp_ping pg; gbahp_bye b; gbahp_control c; gbahp_thumb t; } u;
		for (i = 0; i < len; i++) buf[i] = (uint8_t)(rnd() % 3 ? rnd() : 0xFF);
		gbahp_decode_hello(buf, len, &u.h);
		gbahp_decode_hello_ack(buf, len, &u.a);
		if (gbahp_decode_library(buf, len, &u.l) == 0 && u.l.count) {
			gbahp_lib_entry e; gbahp_library_get(&u.l, u.l.count - 1, &e);
		}
		gbahp_decode_rom_request(buf, len, &u.rr);
		gbahp_decode_rom_push(buf, len, &u.rp);
		gbahp_decode_state_push(buf, len, &u.sp);
		gbahp_decode_state_ack(buf, len, &u.sa);
		gbahp_decode_input(buf, len, &u.in);
		gbahp_decode_ping(buf, len, &u.pg);
		gbahp_decode_bye(buf, len, &u.b);
		gbahp_decode_control(buf, len, &u.c);
		gbahp_decode_thumb(buf, len, &u.t);
	}
}

int main(void)
{
	test_crc_and_layout();
	test_roundtrip_all();
	test_small_messages_do_not_allocate();
	test_errors();
	test_compat();
	test_large_payload_chunked();
	test_zero_length_frame();
	test_fuzz();
	if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
	puts("proto_test: ok");
	return 0;
}
