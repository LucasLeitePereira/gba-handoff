/* Migration of the pre-sync app store (<sha1>.state/.sav, no .meta) to gbah_store.
 * The user's game in progress must come out byte for byte, with a backup copy, and the
 * migration must be idempotent. Negative control: without it the store reports no version. */
#include "gbah_sync.h"
#include "phone_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void put(const char *path, const void *d, size_t n)
{
	FILE *f = fopen(path, "wb");
	if (!f || fwrite(d, 1, n, f) != n) { perror(path); exit(2); }
	fclose(f);
}
static int same(const char *path, const void *d, size_t n)
{
	void *b = NULL; size_t bn = 0;
	int r = gbah_fs_read_all(path, &b, &bn) == 0 && bn == n && memcmp(b, d, n) == 0;
	gbah_fs_free(b);
	return r;
}

int main(void)
{
	char base[] = "/tmp/gbah_migr_XXXXXX", games[300], back[300], p[400], hex[41];
	uint8_t dev[16], sha[20], sha2[20], st[3000], sv[32768], st2[500];
	gbah_meta m;
	struct timeval tv[2];
	time_t when = 1700000000;   /* mtime of the legacy state */
	int i;

	if (!mkdtemp(base)) return 2;
	snprintf(games, sizeof games, "%s/games", base);
	snprintf(back, sizeof back, "%s/legacy-backup", base);
	gbah_fs_mkdirs(games);
	memset(dev, 0xAB, 16);
	for (i = 0; i < 20; i++) { sha[i] = (uint8_t)(i + 1); sha2[i] = (uint8_t)(0x80 + i); }
	for (i = 0; i < (int)sizeof st; i++) st[i] = (uint8_t)(i * 7);
	for (i = 0; i < (int)sizeof sv; i++) sv[i] = (uint8_t)(i * 13 + 5);
	memset(st2, 0x5A, sizeof st2);

	/* game 1: state + sav ; game 2: state only (no sav yet) */
	gbah_sha1_hex(sha, hex);
	snprintf(p, sizeof p, "%s/%s.state", games, hex); put(p, st, sizeof st);
	tv[0].tv_sec = tv[1].tv_sec = when; tv[0].tv_usec = tv[1].tv_usec = 0;
	utimes(p, tv);
	snprintf(p, sizeof p, "%s/%s.sav", games, hex); put(p, sv, sizeof sv);
	gbah_sha1_hex(sha2, hex);
	snprintf(p, sizeof p, "%s/%s.state", games, hex); put(p, st2, sizeof st2);
	put((snprintf(p, sizeof p, "%s/lixo.txt", games), p), "x", 1);   /* stray file is ignored */

	/* negative control: before migrating there is no sync version of the game */
	memset(&m, 0, sizeof m);
	CHECK(gbah_store_meta(games, sha, dev, &m, NULL, NULL) == 0);
	CHECK(m.timestamp_ms == 0);   /* legacy fallback: (dev_self, 1) with timestamp 0 */

	CHECK(phone_migrate_store(games, back, dev) == 2);

	memset(&m, 0, sizeof m);
	CHECK(gbah_store_meta(games, sha, dev, &m, NULL, NULL) == 0);
	CHECK(m.has_data);
	CHECK(m.version.counter == 1 && memcmp(m.version.dev, dev, 16) == 0);
	CHECK(m.timestamp_ms == (uint64_t)when * 1000u);
	CHECK(gbah_ver_none(&m.base));

	/* user data untouched, backup copy made */
	gbah_sha1_hex(sha, hex);
	snprintf(p, sizeof p, "%s/%s.state", games, hex); CHECK(same(p, st, sizeof st));
	snprintf(p, sizeof p, "%s/%s.sav", games, hex); CHECK(same(p, sv, sizeof sv));
	snprintf(p, sizeof p, "%s/%s.state", back, hex); CHECK(same(p, st, sizeof st));
	snprintf(p, sizeof p, "%s/%s.sav", back, hex); CHECK(same(p, sv, sizeof sv));
	gbah_sha1_hex(sha2, hex);
	snprintf(p, sizeof p, "%s/%s.state", back, hex); CHECK(same(p, st2, sizeof st2));
	snprintf(p, sizeof p, "%s/%s.sav", back, hex); CHECK(!gbah_fs_exists(p));

	/* idempotent: second run does nothing; a later save must not be re-migrated nor the backup overwritten */
	{
		gbah_meta n2 = m;
		uint8_t newst[100];
		memset(newst, 1, sizeof newst);
		gbah_sync_advance(&n2, dev, 1800000000000ull, 99);
		CHECK(gbah_store_commit(games, &n2, newst, sizeof newst, sv, sizeof sv, NULL, 0) == 0);
		CHECK(phone_migrate_store(games, back, dev) == 0);
		memset(&m, 0, sizeof m);
		gbah_store_meta(games, sha, dev, &m, NULL, NULL);
		CHECK(m.version.counter == 2 && m.frame == 99);
		gbah_sha1_hex(sha, hex);
		snprintf(p, sizeof p, "%s/%s.state", back, hex); CHECK(same(p, st, sizeof st));
	}
	/* a corrupt .meta is rewritten, state kept */
	gbah_sha1_hex(sha2, hex);
	snprintf(p, sizeof p, "%s/%s.meta", games, hex); put(p, "garbage", 7);
	CHECK(phone_migrate_store(games, back, dev) == 1);
	snprintf(p, sizeof p, "%s/%s.state", games, hex); CHECK(same(p, st2, sizeof st2));
	/* empty / missing dir is not an error */
	CHECK(phone_migrate_store("/tmp/gbah_no_such_dir_xyz", back, dev) == 0);

	{ char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", base); if (system(cmd)) {} }
	if (failures) { fprintf(stderr, "phone_migrate_test: %d failure(s)\n", failures); return 1; }
	printf("phone_migrate_test OK\n");
	return 0;
}
