/*
 * Headless determinism test for the handoff:
 *   core A: 300 frames -> save state + sram -> 60 frames (hash each frame)
 *   core A: load state -> same 60 frames  == identical hashes   (same-core restore)
 *   core B (fresh): gbah_handoff_restore(state, sav) -> same 60 frames == identical hashes (real handoff)
 * Run for two ROMs: the normal one (SRAM type known from the start) and the
 * "late" one (type undetected until ROM loop 20 (the ROM needs several video frames per loop); the save byte is written once).
 * For the late ROM there is also a negative control: the OLD order (sav import
 * before state load) must be reported as a failure AND must diverge.
 * usage: core_test <rom.gba> <sha1> <rom_late.gba> <sha1_late>
 */
#include "gbah_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PRE_FRAMES 300
#define POST_FRAMES 60
#define INPUT (GBAH_KEY_A | GBAH_KEY_RIGHT)

static int failures;
#define CHECK(cond, ...) \
	do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static size_t audio_total;
static int audio_nonzero;

static void run(gbah_core *c, int frames, uint64_t *hashes)
{
	static int16_t buf[4096 * 2];
	int i;
	for (i = 0; i < frames; i++) {
		size_t n, k;
		gbah_run_frame(c);
		n = gbah_audio_read(c, buf, 4096);
		audio_total += n;
		for (k = 0; k < n * 2; k++)
			if (buf[k]) audio_nonzero = 1;
		if (hashes) hashes[i] = gbah_framebuffer_hash(c);
	}
}

static int same(const uint64_t *a, const uint64_t *b, const char *what)
{
	int i;
	for (i = 0; i < POST_FRAMES; i++) {
		if (a[i] != b[i]) {
			CHECK(0, "%s: frame hash differs at +%d (%016llx vs %016llx)", what, i,
			      (unsigned long long)a[i], (unsigned long long)b[i]);
			return 0;
		}
	}
	return 1;
}

static void scenario(const char *path, const char *sha, int late)
{
	uint64_t h1[POST_FRAMES], h2[POST_FRAMES], h3[POST_FRAMES];
	char hex[41];
	void *state = NULL, *sram = NULL, *sram2 = NULL, *rom = NULL;
	size_t state_n, sram_n, sram2_n, rom_n;
	gbah_core *a, *b;
	int i, distinct;

	fprintf(stderr, "-- scenario %s (%s)\n", late ? "late-save-type" : "normal", path);
	audio_total = 0; audio_nonzero = 0;
	a = gbah_create(0);
	CHECK(a != NULL, "gbah_create");
	if (!a) return;
	CHECK(gbah_load_rom_file(a, path) == 0, "load rom");
	CHECK(gbah_rom_sha1_hex(a, hex) == 0 && strcmp(hex, sha) == 0, "sha1 %s != %s", hex, sha);
	if (late) {
		void *t = NULL;
		CHECK(gbah_sram_export(a, &t) == 0, "late ROM: save type must still be undetected after load");
	}

	/* garbage must be rejected without crashing */
	{
		gbah_core *g = gbah_create(0);
		unsigned char junk[64] = {0};
		(void)gbah_load_rom_buffer(g, junk, sizeof junk); /* result may vary; must not crash */
		gbah_destroy(g);
	}

	gbah_set_input(a, INPUT);
	run(a, PRE_FRAMES, NULL);
	CHECK(gbah_frame_counter(a) >= PRE_FRAMES, "frame counter %u", gbah_frame_counter(a));

	state_n = gbah_state_save(a, &state);
	CHECK(state_n > 0, "state save");
	sram_n = gbah_sram_export(a, &sram);
	CHECK(sram_n > 0, "sram export (size %zu)", sram_n);
	if (sram_n > 0x100) {
		if (late) CHECK(((unsigned char *)sram)[0x100] == 0xA5, "sram[0x100]=%02x expected a5", ((unsigned char *)sram)[0x100]);
		else CHECK(((unsigned char *)sram)[1] == (INPUT & 0xFF), "sram[1]=%02x expected %02x (ROM stores keys)",
		           ((unsigned char *)sram)[1], INPUT & 0xFF);
	}
	if (!state_n || !sram_n) return;

	run(a, POST_FRAMES, h1);
	sram2_n = gbah_sram_export(a, &sram2);
	/* the frames must actually change over time, otherwise the test proves nothing */
	distinct = 0;
	for (i = 1; i < POST_FRAMES; i++) if (h1[i] != h1[0]) distinct++;
	CHECK(distinct > 10, "framebuffer barely changes (%d distinct)", distinct);
	CHECK(audio_total > 0 && audio_nonzero, "no audio produced (%zu frames)", audio_total);

	/* 1) same-core restore */
	CHECK(gbah_state_load(a, state, state_n) == 0, "state load (same core)");
	run(a, POST_FRAMES, h2);
	same(h1, h2, "same-core");
	{
		void *s3 = NULL;
		size_t n3 = gbah_sram_export(a, &s3);
		CHECK(n3 == sram2_n && memcmp(s3, sram2, n3) == 0, "sram differs after same-core replay");
		gbah_free(s3);
	}

	/* 2) fresh core handoff via the single entry point */
	b = gbah_create(0);
	CHECK(b != NULL, "create b");
	if (!b || gbah_read_file(path, &rom, &rom_n)) return;
	CHECK(gbah_load_rom_buffer(b, rom, rom_n) == 0, "load rom (buffer) in b");
	CHECK(gbah_rom_sha1_hex(b, hex) == 0 && strcmp(hex, sha) == 0, "sha1 b");
	CHECK(gbah_handoff_restore(b, state, state_n, sram, sram_n) == 0, "handoff_restore");
	gbah_set_input(b, INPUT);
	run(b, POST_FRAMES, h3);
	same(h1, h3, "fresh-core handoff");
	{
		void *s4 = NULL;
		size_t n4 = gbah_sram_export(b, &s4);
		CHECK(n4 == sram2_n && memcmp(s4, sram2, n4) == 0, "sram differs after handoff replay");
		gbah_free(s4);
	}

	/* wrong-size state must be rejected */
	CHECK(gbah_state_load(b, state, state_n - 1) != 0, "truncated state accepted");

	/* 3) NEGATIVE CONTROL (late ROM): the old order (sav before state) loses the save */
	if (late) {
		uint64_t h4[POST_FRAMES];
		gbah_core *d = gbah_create(0);
		int r, differs = 0;
		CHECK(d && gbah_load_rom_buffer(d, rom, rom_n) == 0, "load rom in d");
		r = gbah_sram_import(d, sram, sram_n);
		CHECK(r == GBAH_ERR_SAVE_TYPE_UNKNOWN, "import before state on undetected type must fail (got %d)", r);
		CHECK(gbah_state_load(d, state, state_n) == 0, "state load (old order)");
		gbah_set_input(d, INPUT);
		run(d, POST_FRAMES, h4);
		for (i = 0; i < POST_FRAMES; i++) if (h4[i] != h1[i]) differs = 1;
		CHECK(differs, "negative control: old order should have diverged but did not");
		gbah_destroy(d);
	}

	/* 4) cold start with an existing .sav (no state): the save must be live right away */
	if (late) {
		void *o = NULL;
		size_t on;
		gbah_core *f = gbah_create(0);
		CHECK(f && gbah_load_rom_buffer(f, rom, rom_n) == 0, "load rom in f");
		CHECK(gbah_load_sav_buffer(f, sram, sram_n) == 0, "load_sav_buffer");
		on = gbah_sram_export(f, &o);
		CHECK(on == sram_n && o && memcmp(o, sram, on) == 0, "cold-start sav not live (size %zu)", on);
		gbah_free(o);
		run(f, 5, NULL);
		on = gbah_sram_export(f, &o);
		CHECK(on == sram_n && o && ((unsigned char *)o)[0x100] == 0xA5, "cold-start sav lost after running");
		gbah_free(o);
		gbah_destroy(f);
	}

	/* handoff_restore must not swallow a sav that cannot be applied */
	if (late) {
		gbah_core *e = gbah_create(0);
		CHECK(e && gbah_load_rom_buffer(e, rom, rom_n) == 0, "load rom in e");
		CHECK(gbah_sram_import(e, sram, sram_n) != 0, "import on fresh undetected core must report failure");
		gbah_destroy(e);
	}

	if (!failures) printf("scenario %s OK: state %zu bytes, sram %zu bytes, %zu audio frames, last hash %016llx\n",
	       late ? "late" : "normal", state_n, sram_n, audio_total, (unsigned long long)h1[POST_FRAMES - 1]);
	gbah_free(state); gbah_free(sram); gbah_free(sram2); gbah_free(rom);
	gbah_destroy(a);
	gbah_destroy(b);
}

int main(int argc, char **argv)
{
	if (argc < 5) { fprintf(stderr, "usage: %s rom.gba sha1 rom_late.gba sha1_late\n", argv[0]); return 2; }
	scenario(argv[1], argv[2], 0);
	scenario(argv[3], argv[4], 1);
	if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
	printf("core_test OK\n");
	return 0;
}
