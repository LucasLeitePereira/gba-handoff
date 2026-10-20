/* Unit tests of sync/gbah_sync: version rules (all cases, full sequences) and the game store. */
#include "gbah_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static const uint8_t SHA[20] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20 };
static uint8_t P[16], C[16];   /* phone / PC device ids */

static gbah_meta empty(void) { gbah_meta m; memset(&m, 0, sizeof m); memcpy(m.sha1, SHA, 20); return m; }
static gbah_ver ver(const uint8_t *dev, uint64_t c) { gbah_ver v; memcpy(v.dev, dev, 16); v.counter = c; return v; }

/* The "obvious but wrong" rule: highest counter wins. Used as the negative control. */
static int naive_decide(const gbah_meta *l, const gbah_meta *r)
{
	if (!l->has_data && !r->has_data) return GBAH_SYNC_NOTHING;
	if (!l->has_data) return GBAH_SYNC_RECEIVE;
	if (!r->has_data) return GBAH_SYNC_SEND;
	if (l->version.counter == r->version.counter) return GBAH_SYNC_NOTHING;
	return l->version.counter > r->version.counter ? GBAH_SYNC_SEND : GBAH_SYNC_RECEIVE;
}

/* A transfer l -> r confirmed by STATE_ACK, as both sides apply it. */
static void transfer(gbah_meta *from, gbah_meta *to)
{
	gbah_sync_adopt(to, from);
	gbah_sync_mark_synced(from);
}

static void test_rules(void)
{
	gbah_meta a = empty(), b = empty();
	/* nobody has data */
	CHECK(gbah_sync_decide(&a, &b) == GBAH_SYNC_NOTHING);
	/* only one side */
	gbah_sync_advance(&a, P, 1000, 10);
	CHECK(gbah_sync_decide(&a, &b) == GBAH_SYNC_SEND);
	CHECK(gbah_sync_decide(&b, &a) == GBAH_SYNC_RECEIVE);
	/* same version, even with different bases */
	b = a; b.base = ver(C, 7);
	CHECK(gbah_sync_decide(&a, &b) == GBAH_SYNC_NOTHING);
	/* both have data, no base at all, different versions: conflict (both directions) */
	b = empty(); gbah_sync_advance(&b, C, 2000, 20);
	CHECK(gbah_sync_decide(&a, &b) == GBAH_SYNC_CONFLICT);
	CHECK(gbah_sync_decide(&b, &a) == GBAH_SYNC_CONFLICT);
	/* a zero version never matches a zero base */
	b = empty(); b.has_data = 1; b.version = ver(C, 1);
	a = empty(); a.has_data = 1; a.version = ver(P, 1);
	CHECK(gbah_sync_decide(&a, &b) == GBAH_SYNC_CONFLICT);
	/* advance keeps the base and bumps the counter with the device's own id */
	a = empty(); gbah_sync_advance(&a, P, 1, 1);
	CHECK(a.version.counter == 1 && !memcmp(a.version.dev, P, 16) && gbah_ver_none(&a.base));
	b = empty(); transfer(&a, &b);
	CHECK(gbah_ver_eq(&a.base, &a.version) && gbah_ver_eq(&b.base, &b.version) && gbah_ver_eq(&a.version, &b.version));
	gbah_sync_advance(&b, C, 2, 2);
	CHECK(b.version.counter == 2 && !memcmp(b.version.dev, C, 16) && gbah_ver_eq(&b.base, &a.version));
	gbah_sync_advance(&b, C, 3, 3);   /* several saves without syncing keep the same base */
	CHECK(b.version.counter == 3 && gbah_ver_eq(&b.base, &a.version));
	CHECK(gbah_sync_decide(&b, &a) == GBAH_SYNC_SEND);
	CHECK(gbah_sync_decide(&a, &b) == GBAH_SYNC_RECEIVE);
	/* both advanced from the common base */
	gbah_sync_advance(&a, P, 4, 4);
	CHECK(gbah_sync_decide(&a, &b) == GBAH_SYNC_CONFLICT);
	CHECK(gbah_sync_decide(&b, &a) == GBAH_SYNC_CONFLICT);
}

/* phone -> PC, PC plays alone, reconnect, PC -> phone */
static void test_full_sequence(void)
{
	gbah_meta ph = empty(), pc = empty();
	/* 1. phone plays, cable connected: phone saves and hands off */
	gbah_sync_advance(&ph, P, 1000, 600);
	CHECK(gbah_sync_decide(&ph, &pc) == GBAH_SYNC_SEND && gbah_sync_decide(&pc, &ph) == GBAH_SYNC_RECEIVE);
	transfer(&ph, &pc);
	CHECK(gbah_sync_decide(&ph, &pc) == GBAH_SYNC_NOTHING);
	/* 2. PC plays alone (cable out): several saves */
	gbah_sync_advance(&pc, C, 2000, 1200);
	gbah_sync_advance(&pc, C, 3000, 1800);
	/* 3. reconnect: PC is newer */
	CHECK(gbah_sync_decide(&pc, &ph) == GBAH_SYNC_SEND);
	CHECK(gbah_sync_decide(&ph, &pc) == GBAH_SYNC_RECEIVE);
	/* 4. PC -> phone */
	transfer(&pc, &ph);
	CHECK(gbah_sync_decide(&ph, &pc) == GBAH_SYNC_NOTHING && gbah_sync_decide(&pc, &ph) == GBAH_SYNC_NOTHING);
	CHECK(ph.version.counter == 3 && !memcmp(ph.version.dev, C, 16) && ph.frame == 1800);
	/* 5. phone continues, later syncs back */
	gbah_sync_advance(&ph, P, 4000, 2400);
	CHECK(ph.version.counter == 4 && gbah_sync_decide(&ph, &pc) == GBAH_SYNC_SEND);
}

/* Controls: the naive "highest counter wins" rule gets the divergence wrong, and forgetting
 * to record base after a transfer turns a clean reconnect into a false conflict. */
static void test_negative_controls(void)
{
	gbah_meta ph = empty(), pc = empty();
	gbah_sync_advance(&ph, P, 1, 1);
	transfer(&ph, &pc);
	gbah_sync_advance(&ph, P, 2, 2);      /* phone plays on: (P,2) */
	gbah_sync_advance(&pc, C, 3, 3);      /* PC plays on:    (C,2) */
	CHECK(gbah_sync_decide(&ph, &pc) == GBAH_SYNC_CONFLICT);
	CHECK(naive_decide(&ph, &pc) == GBAH_SYNC_NOTHING);   /* naive rule: same counter, one side silently lost */
	gbah_sync_advance(&pc, C, 4, 4);
	CHECK(naive_decide(&ph, &pc) != GBAH_SYNC_CONFLICT);  /* and it never reports a conflict */

	{   /* base not recorded after a transfer */
		gbah_meta a = empty(), b = empty();
		gbah_sync_advance(&a, P, 1, 1);
		b = a;                            /* data copied, but nobody set base = version */
		gbah_sync_advance(&b, C, 2, 2);
		CHECK(gbah_sync_decide(&b, &a) == GBAH_SYNC_CONFLICT);   /* false conflict: why STATE_ACK sets base */
		transfer(&a, &b);                 /* with the real procedure on a fresh copy: */
		b = empty(); transfer(&a, &b);
		gbah_sync_advance(&b, C, 2, 2);
		CHECK(gbah_sync_decide(&b, &a) == GBAH_SYNC_SEND);
	}
}

/* Mirror ("espelho"): the PC keeps pushing safety copies while the phone waits. A mirror is a
 * confirmed transfer, so it moves base. If the cable is pulled, the phone holds a recent point;
 * if it then plays alone and the PC also played on, that is a REAL conflict. */
static void test_mirror(void)
{
	gbah_meta ph = empty(), pc = empty();
	gbah_sync_advance(&ph, P, 1, 1);
	transfer(&ph, &pc);                    /* handoff to the PC */
	gbah_sync_advance(&pc, C, 2, 100);     /* PC plays, mirror #1 */
	transfer(&pc, &ph);
	CHECK(gbah_sync_decide(&ph, &pc) == GBAH_SYNC_NOTHING);
	gbah_sync_advance(&pc, C, 3, 200);     /* mirror #2 */
	transfer(&pc, &ph);
	{   /* A: cable pulled, phone does not play, PC plays on: later PC -> phone */
		gbah_meta ph2 = ph, pc2 = pc;
		gbah_sync_advance(&pc2, C, 4, 300);
		CHECK(gbah_sync_decide(&pc2, &ph2) == GBAH_SYNC_SEND);
		CHECK(gbah_sync_decide(&ph2, &pc2) == GBAH_SYNC_RECEIVE);
	}
	{   /* B: cable pulled, both play on: conflict */
		gbah_meta ph2 = ph, pc2 = pc;
		gbah_sync_advance(&pc2, C, 4, 300);
		gbah_sync_advance(&ph2, P, 5, 250);
		CHECK(gbah_sync_decide(&pc2, &ph2) == GBAH_SYNC_CONFLICT);
		CHECK(gbah_sync_decide(&ph2, &pc2) == GBAH_SYNC_CONFLICT);
	}
	{   /* C: a stale mirror in flight arrives after the phone already has something newer */
		gbah_meta stale = pc;
		gbah_meta newer = pc;
		gbah_sync_advance(&newer, P, 9, 999);
		CHECK(gbah_sync_decide(&newer, &stale) == GBAH_SYNC_SEND);   /* receiver answers STALE, keeps its data */
	}
}

static void test_meta_codec(void)
{
	gbah_meta m = empty(), d;
	uint8_t h[GBAH_META_HEADER];
	uint32_t tl = 0;
	gbah_sync_advance(&m, P, 123456789012ull, 777);
	m.base = ver(C, 41);
	CHECK(gbah_meta_encode(&m, 1234, h) == GBAH_META_HEADER);
	CHECK(gbah_meta_decode(h, sizeof h, &d, &tl) == 0 && tl == 1234);
	CHECK(!memcmp(&d.version, &m.version, sizeof m.version) && !memcmp(&d.base, &m.base, sizeof m.base));
	CHECK(d.timestamp_ms == 123456789012ull && d.frame == 777 && d.has_data && !memcmp(d.sha1, SHA, 20));
	CHECK(gbah_meta_decode(h, 20, &d, &tl) != 0);                 /* short */
	h[0] = 'X'; CHECK(gbah_meta_decode(h, sizeof h, &d, &tl) != 0); h[0] = 'G';
	h[88] = 0xFF; h[89] = 0xFF; h[90] = 0xFF; h[91] = 0x7F;         /* absurd thumb size */
	CHECK(gbah_meta_decode(h, sizeof h, &d, &tl) != 0);
}

static int count_cb_n;
static void count_cb(const uint8_t s[20], void *u) { (void)s; (void)u; count_cb_n++; }

static void test_store(const char *root)
{
	char dir[512], p[600], hex[41];
	gbah_meta m = empty(), r;
	void *st = NULL, *sv = NULL;
	size_t stn = 0, svn = 0;
	uint8_t *thumb = NULL;
	uint32_t tl = 0;
	uint8_t state1[1000], state2[900], sav1[64], thumbd[GBAH_THUMB_BYTES];
	uint64_t ts[8];
	int i;

	snprintf(dir, sizeof dir, "%s/Jo\xc3\xa3o/games", root);   /* UTF-8 path with accents */
	gbah_sha1_hex(SHA, hex);
	CHECK(!strcmp(hex, "0102030405060708090a0b0c0d0e0f1011121314"));
	memset(state1, 0xA1, sizeof state1); memset(state2, 0xB2, sizeof state2);
	memset(sav1, 0x5A, sizeof sav1); memset(thumbd, 0x77, sizeof thumbd);

	CHECK(gbah_store_meta(dir, SHA, P, &r, &thumb, &tl) == 1 && !r.has_data);   /* missing dir = no data */
	gbah_sync_advance(&m, P, 5000, 42);
	CHECK(gbah_store_commit(dir, &m, state1, sizeof state1, sav1, sizeof sav1, thumbd, sizeof thumbd) == 0);
	CHECK(gbah_store_meta(dir, SHA, C, &r, &thumb, &tl) == 0);
	CHECK(r.has_data && r.version.counter == 1 && !memcmp(r.version.dev, P, 16) && r.frame == 42 && r.timestamp_ms == 5000);
	CHECK(tl == sizeof thumbd && thumb && !memcmp(thumb, thumbd, tl));
	free(thumb);
	CHECK(gbah_store_read(dir, SHA, &st, &stn, &sv, &svn) == 0 && stn == sizeof state1 && svn == sizeof sav1);
	CHECK(st && !memcmp(st, state1, stn) && sv && !memcmp(sv, sav1, svn));
	gbah_fs_free(st); gbah_fs_free(sv);
	snprintf(p, sizeof p, "%s/%s.state.tmp", dir, hex);
	CHECK(!gbah_fs_exists(p));                                   /* no temp file left behind */

	/* mark_synced only when the stored version is the one that was acked */
	CHECK(gbah_store_mark_synced(dir, SHA, &m.version) == 0);
	gbah_store_meta(dir, SHA, C, &r, NULL, NULL);
	CHECK(gbah_ver_eq(&r.base, &r.version));
	{
		gbah_ver old = m.version;
		gbah_sync_advance(&m, P, 6000, 50);
		m.base = r.base;
		CHECK(gbah_store_commit(dir, &m, state2, sizeof state2, NULL, 0, NULL, 0) == 0);   /* no sav, no thumb */
		CHECK(gbah_store_mark_synced(dir, SHA, &old) == 1);                                /* advanced meanwhile: skipped */
		gbah_store_meta(dir, SHA, C, &r, &thumb, &tl);
		CHECK(r.version.counter == 2 && r.base.counter == 1 && tl == 0 && thumb == NULL);
		snprintf(p, sizeof p, "%s/%s.sav", dir, hex);
		CHECK(!gbah_fs_exists(p));                                                           /* stale .sav removed */
	}

	/* legacy: .state without .meta (app before this phase) */
	snprintf(p, sizeof p, "%s/%s.meta", dir, hex);
	CHECK(gbah_fs_remove(p) == 0);
	CHECK(gbah_store_meta(dir, SHA, C, &r, NULL, NULL) == 0 && r.has_data && r.version.counter == 1 && !memcmp(r.version.dev, C, 16));
	gbah_store_commit(dir, &m, state2, sizeof state2, NULL, 0, NULL, 0);

	/* backups: keep the 3 newest, files are moved, 3 extensions together */
	for (i = 1; i <= 5; i++) {
		gbah_sync_advance(&m, P, 7000 + (uint64_t)i, 60 + (uint32_t)i);
		CHECK(gbah_store_commit(dir, &m, state1, sizeof state1, sav1, sizeof sav1, NULL, 0) == 0);
		CHECK(gbah_store_backup(dir, SHA, 1000u * (uint64_t)i, 3) == 0);
		snprintf(p, sizeof p, "%s/%s.state", dir, hex);
		CHECK(!gbah_fs_exists(p));
	}
	CHECK(gbah_store_list_backups(dir, SHA, ts, 8) == 3);
	CHECK(ts[0] == 5000 && ts[1] == 4000 && ts[2] == 3000);
	snprintf(p, sizeof p, "%s/%s.backup-5000.meta", dir, hex); CHECK(gbah_fs_exists(p));
	snprintf(p, sizeof p, "%s/%s.backup-5000.sav", dir, hex);  CHECK(gbah_fs_exists(p));
	snprintf(p, sizeof p, "%s/%s.backup-1000.state", dir, hex); CHECK(!gbah_fs_exists(p));
	snprintf(p, sizeof p, "%s/%s.backup-2000.meta", dir, hex);  CHECK(!gbah_fs_exists(p));
	/* same timestamp twice: the first backup survives */
	gbah_store_commit(dir, &m, state2, sizeof state2, NULL, 0, NULL, 0);
	CHECK(gbah_store_backup(dir, SHA, 5000, 3) == 0);
	CHECK(gbah_store_list_backups(dir, SHA, ts, 8) == 3 && ts[0] == 5001);
	/* backups are not listed as games */
	count_cb_n = 0;
	CHECK(gbah_store_list(dir, count_cb, NULL) == 0);
	CHECK(count_cb_n == 0);   /* current files were moved into the backup */
	gbah_store_commit(dir, &m, state2, sizeof state2, NULL, 0, NULL, 0);
	CHECK(gbah_store_list(dir, count_cb, NULL) == 0 && count_cb_n == 1);
}

int main(void)
{
	char root[] = "/tmp/gbah_sync_XXXXXX", cmd[300];
	memset(P, 0xAA, 16); memset(C, 0xCC, 16);
	test_rules();
	test_full_sequence();
	test_negative_controls();
	test_mirror();
	test_meta_codec();
	if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
	test_store(root);
	snprintf(cmd, sizeof cmd, "rm -rf %s", root);
	if (system(cmd) != 0) { /* best effort */ }
	if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
	puts("sync_test: ok");
	return 0;
}
