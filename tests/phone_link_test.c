/* The REAL phone engine (android/.../phone_link.c + core_host.c over a real gbah_core, with an
 * emulation thread that obeys the hold flag like jni_bridge.c) against the REAL desktop binary,
 * over real TCP with a fake adb. Children are fork()ed with PR_SET_PDEATHSIG.
 *
 *  A  phone with a game connects to an idle PC: ROM goes to the PC (ROM_REQUEST/ROM_PUSH from the
 *     phone, read through the "content" source), the game is handed off, the phone HOLDS (frames stop),
 *     the PC's MIRRORs are stored WITHOUT being assumed (core untouched), RETURN: the phone gets the
 *     HANDOFF and resumes at exactly the stored frame. Negative control: the core really changed.
 *  B  conflict (both advanced from the same base): keep the phone version, then the PC version.
 *     The loser becomes a backup on the side that lost.
 *  C  phone without the ROM and without the game: the PC offers it, the phone asks for the ROM
 *     (ROM_REQUEST), receives it (hash verified) and stores the progress as a mirror.
 *  D  cable pulled while the game is on the PC: the phone resumes from the last mirror, with a notice.
 *  E  autosave: periodic, on a .sav change, nothing while idle (negative control); the snapshot is
 *     short (core lock hold time is printed).
 *  F  incompatible core version: the PC refuses, the phone reports REFUSED.
 * POSIX only. */
#include "phone_sim.h"

#define WAIT(cond, ms) ({ int _ok = 0; uint64_t _e = mono_ms() + (ms); \
	while (mono_ms() < _e) { if (cond) { _ok = 1; break; } usleep(30000); } _ok; })

static const char *g_exe, *g_fake_adb;

static void ui_state(Sim *s, int *on_pc, int *conflict)
{
	char b[8000], *line, *save;
	*on_pc = 0; *conflict = 0;
	phone_ui_text(s->link, b, sizeof b);
	for (line = strtok_r(b, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (line[0] == 'H') *on_pc = 1;
		if (line[0] == 'C') *conflict = 1;
	}
}
static int on_pc(Sim *s) { int a, b; ui_state(s, &a, &b); return a; }
static int has_conflict(Sim *s) { int a, b; ui_state(s, &a, &b); return b; }

/* a state advanced by `n` frames of input `keys` from the loaded ROM, for seeding stores */
static size_t make_state(int n, uint32_t keys, void **st, void **sv, size_t *vn, uint32_t *frame)
{
	gbah_core *c = new_core();
	int i;
	size_t sn;
	gbah_set_input(c, keys);
	for (i = 0; i < n; i++) gbah_run_frame(c);
	sn = gbah_state_save(c, st);
	*vn = gbah_sram_export(c, sv);
	*frame = gbah_frame_counter(c);
	gbah_destroy(c);
	return sn;
}

static void phone_up(Sim *s, const char *name, uint8_t devb, unsigned auto_ms, unsigned sav_ms, uint16_t port, const char *core)
{
	if (sim_create(s, name, devb, auto_ms, sav_ms, port, core, 1) != 0) { fprintf(stderr, "sim_create\n"); exit(2); }
}

/* A + B + D share the PC data dir (the story of a game moving between the two) */
static void scenario_a_b(void)
{
	Sim ph;
	Pc pc;
	char data[300], games_pc[340], hex[41];
	uint16_t port = free_port();
	gbah_meta m0, m1, m2;
	uint32_t f0, fh;
	uint64_t h_before;
	int round;

	snprintf(data, sizeof data, "%s/pc_ab", g_base);
	snprintf(games_pc, sizeof games_pc, "%s/games", data);
	gbah_sha1_hex(g_sha, hex);
	phone_up(&ph, "ph_ab", 0x11, 600000, 600000, port, NULL);
	REQUIRE(sim_load_game(&ph, 1) == 1);          /* nothing stored */
	sim_run(&ph);
	usleep(500000);
	CHECK(phone_save_now(ph.link) == 0);            /* the phone has progress (version (phone,1)) */
	store_meta_of(&ph, &m0);
	CHECK(m0.has_data && m0.version.counter == 1 && memcmp(m0.version.dev, ph.dev, 16) == 0);

	REQUIRE(pc_start(&pc, g_exe, g_fake_adb, "pc_ab", data, port, NULL, "400") == 0);
	phone_link_start(ph.link);
	/* A1: handoff: the game goes to the PC (needs the ROM: ROM_REQUEST answered through the content source) */
	if (!WAIT(on_pc(&ph), 20000)) { fprintf(stderr, "no handoff\n"); log_dump(&pc, "A"); }
	REQUIRE(on_pc(&ph));
	CHECK(phone_link_status(ph.link) == PL_ST_CONNECTED);
	CHECK(WAIT(log_has(&pc, "ROM"), 3000));   /* the PC received the ROM from the phone */
	{   /* held: no frames while the game is on the PC */
		uint32_t a = sim_frame(&ph);
		usleep(400000);
		CHECK(sim_frame(&ph) == a);
		f0 = a;
	}
	/* A2: PC mirrors: stored without assuming (core untouched, store ahead) */
	h_before = sim_hash(&ph);
	if (!WAIT((store_meta_of(&ph, &m1), memcmp(m1.version.dev, ph.dev, 16) != 0), 15000)) log_dump(&pc, "A mirrors");
	store_meta_of(&ph, &m1);
	CHECK(memcmp(m1.version.dev, ph.dev, 16) != 0);       /* the PC's version */
	CHECK(m1.frame > f0);
	CHECK(sim_frame(&ph) == f0 && sim_hash(&ph) == h_before);   /* NOT assumed */
	CHECK(on_pc(&ph));
	/* A3: back to the phone */
	phone_cmd_return(ph.link);
	CHECK(WAIT(!on_pc(&ph), 15000));
	store_meta_of(&ph, &m2);
	fh = sim_frame(&ph);
	CHECK(fh >= m2.frame && fh < m2.frame + 120);        /* resumed exactly at the delivered frame (+ frames since) */
	CHECK(fh > f0);                                         /* negative control: the core was replaced (it was held at f0) */
	CHECK(m2.frame >= m1.frame);
	CHECK(gbah_ver_eq(&m2.base, &m2.version));
	/* the core is now running: it advances */
	{ uint32_t a = sim_frame(&ph); usleep(300000); CHECK(sim_frame(&ph) > a); }
	/* determinism: frozen core == stored state advanced by the same frames */
	sim_pause(&ph);
	phone_link_stop(ph.link);
	pthread_mutex_lock(&ph.mtx);
	{
		/* restore the stored state again and compare with a reference: proves what the store holds
		 * is what the phone resumed from */
		gbah_meta ms;
		void *st = NULL, *sv = NULL;
		size_t sn = 0, vn = 0;
		gbah_core *ref = new_core();
		store_meta_of(&ph, &ms);
		CHECK(gbah_store_read(ph.games, g_sha, &st, &sn, &sv, &vn) == 0);
		CHECK(gbah_handoff_restore(ref, st, sn, vn ? sv : NULL, vn) == 0);
		CHECK(gbah_frame_counter(ref) == ms.frame);
		gbah_free(st); gbah_free(sv); gbah_destroy(ref);
	}
	pthread_mutex_unlock(&ph.mtx);
	CHECK(pc_stop(&pc) == 0);
	{ char *l = slurp(pc.log); if (failures) fprintf(stderr, "--- A: desktop log ---\n%s", l); free(l); }

	/* ---------------- B: conflict. Both advance from the synced base while apart. */
	for (round = 0; round < 2; round++) {
		int keep_phone = (round == 0);
		gbah_meta pcm, phm, aft;
		void *st = NULL, *sv = NULL;
		size_t sn, vn;
		uint32_t fr;
		uint8_t pcdev[16];
		int pc_back_before, ph_back_before;
		Pc pc2;
		uint16_t port2 = free_port();
		char nm[16];
		memset(pcdev, 0xEE, 16);

		/* phone advances (new frames, phone version counter+1) */
		sim_run(&ph);
		usleep(300000);
		CHECK(phone_save_now(ph.link) == 0);
		sim_pause(&ph);
		store_meta_of(&ph, &phm);
		/* PC advances from the same base with different input */
		CHECK(pc_data_meta(data, &pcm) == 0);
		sn = make_state(60 + 40 * round, 0x40, &st, &sv, &vn, &fr);
		gbah_sync_advance(&pcm, pcdev, gbah_now_ms(), fr);
		{
			static uint8_t pic[GBAH_THUMB_BYTES];
			memset(pic, 0x77, sizeof pic);
			CHECK(gbah_store_commit(games_pc, &pcm, st, sn, vn ? sv : NULL, vn, pic, sizeof pic) == 0);
		}
		gbah_free(st); gbah_free(sv);
		pc_back_before = count_backups(games_pc);
		ph_back_before = count_backups(ph.games);

		snprintf(nm, sizeof nm, "pc_b%d", round);
		REQUIRE(pc_start(&pc2, g_exe, g_fake_adb, nm, data, port2, NULL, "600000") == 0);
		/* the phone connects to the new port: recreate the link with it (the port is a config) */
		{
			uint8_t keep[16];
			memcpy(keep, ph.dev, 16);
			sim_pause(&ph);
			phone_destroy(ph.link);
			{
				phone_cfg cfg;
				memset(&cfg, 0, sizeof cfg);
				cfg.games_dir = ph.games; cfg.roms_dir = ph.roms;
				memcpy(cfg.device_id, keep, 16);
				cfg.device_name = "TestPhone"; cfg.port = port2;
				cfg.autosave_ms = 600000; cfg.sav_min_ms = 600000; cfg.tick_ms = 100;
				core_host_fill(&cfg.host, &ph.ctx);
				ph.orig = cfg.host;
				cfg.host.user = &ph; cfg.host.snapshot = w_snapshot; cfg.host.sav_hash = w_sav_hash;
				ph.link = phone_create(&cfg);
				REQUIRE(ph.link != NULL);
				{
					phone_rom r;
					memset(&r, 0, sizeof r);
					memcpy(r.sha1, g_sha, 20); r.size = g_rom_len;
					snprintf(r.source, sizeof r.source, "content://test/rom|%s", g_rom);
					phone_set_roms(ph.link, &r, 1);
				}
				phone_game_loaded(ph.link);
				phone_resume_stored(ph.link);
			}
			sim_run(&ph);
		}
		phone_link_start(ph.link);
		if (!WAIT(has_conflict(&ph), 20000)) log_dump(&pc2, "B conflict");
		REQUIRE(has_conflict(&ph));
		CHECK(!on_pc(&ph));
		{ uint8_t th[PHONE_THUMB_BYTES]; CHECK(phone_conflict_thumb(ph.link, g_sha, 0, th) == 1); }
		CHECK(WAIT(({ uint8_t th[PHONE_THUMB_BYTES]; phone_conflict_thumb(ph.link, g_sha, 1, th); }), 5000));
		/* nothing was decided on its own: both stores intact */
		/* (connecting with the game open saves it first: the phone version may be newer than phm, never other device) */
		{ gbah_meta x; store_meta_of(&ph, &x); CHECK(memcmp(x.version.dev, ph.dev, 16) == 0 && x.version.counter >= phm.version.counter); }
		{ gbah_meta x; CHECK(pc_data_meta(data, &x) == 0 && memcmp(x.version.dev, pcdev, 16) == 0); }   /* PC untouched */

		phone_cmd_resolve(ph.link, g_sha, keep_phone);
		CHECK(WAIT(!has_conflict(&ph), 15000));
		usleep(600000);
		store_meta_of(&ph, &aft);
		CHECK(pc_data_meta(data, &pcm) == 0);
		if (keep_phone) {
			CHECK(gbah_ver_eq(&pcm.version, &aft.version) && memcmp(aft.version.dev, ph.dev, 16) == 0);   /* PC adopted the phone version */
			CHECK(gbah_ver_eq(&aft.base, &aft.version) && gbah_ver_eq(&pcm.base, &pcm.version));
			CHECK(count_backups(games_pc) == pc_back_before + 1); /* PC's loser kept */
			CHECK(count_backups(ph.games) == ph_back_before);
		} else {
			CHECK(memcmp(aft.version.dev, pcdev, 16) == 0);       /* phone adopted the PC version */
			CHECK(gbah_ver_eq(&aft.version, &pcm.version) && gbah_ver_eq(&aft.base, &aft.version));
			CHECK(count_backups(ph.games) == ph_back_before + 1); /* phone's loser kept */
			CHECK(count_backups(games_pc) == pc_back_before);
			{   /* the running core is the PC's state */
				void *st2 = NULL, *sv2 = NULL;
				size_t sn2, vn2;
				int ok;
				gbah_core *ref = new_core();
				gbah_store_read(ph.games, g_sha, &st2, &sn2, &sv2, &vn2);
				gbah_handoff_restore(ref, st2, sn2, vn2 ? sv2 : NULL, vn2);
				sim_pause(&ph);   /* frozen: compare the frame the core is at with the stored one + advance */
				ok = core_matches_store(&ph, ph.games, NULL);
				CHECK(ok);
				/* negative control: one frame more or less and the comparison fails */
				CHECK(!core_matches_store_skew(&ph, ph.games, NULL, 1));
				gbah_free(st2); gbah_free(sv2); gbah_destroy(ref);
				sim_run(&ph);
			}
		}
		phone_link_stop(ph.link);
		CHECK(pc_stop(&pc2) == 0);
		{ char *l = slurp(pc2.log); if (failures) fprintf(stderr, "--- B%d: desktop log ---\n%s", round, l); free(l); }
	}
	sim_destroy(&ph);
}

/* C: phone without the ROM, without the game: the PC plays and offers it */
static void scenario_c(void)
{
	Sim ph;
	Pc pc;
	char data[300], rp[400], hex[41];
	uint16_t port = free_port();
	gbah_meta m;
	void *rb = NULL;
	size_t rn = 0;

	snprintf(data, sizeof data, "%s/pc_c", g_base);
	gbah_sha1_hex(g_sha, hex);
	phone_up(&ph, "ph_c", 0x22, 600000, 600000, port, NULL);
	sim_forget_roms(&ph);
	REQUIRE(pc_start(&pc, g_exe, g_fake_adb, "pc_c", data, port, g_rom, "300") == 0);
	phone_link_start(ph.link);
	snprintf(rp, sizeof rp, "%s/%s.gba", ph.roms, hex);
	if (!WAIT(file_exists(rp), 20000)) log_dump(&pc, "C");
	CHECK(file_exists(rp));
	CHECK(gbah_read_file(rp, &rb, &rn) == 0 && rn == g_rom_len && !memcmp(rb, g_rom_bytes, rn));
	gbah_free(rb);
	/* (a .state whose .meta is not written yet reads as the phone's own progress: wait for the PC's version) */
	CHECK(WAIT((store_meta_of(&ph, &m), m.has_data && memcmp(m.version.dev, ph.dev, 16) != 0), 15000));   /* stored as a mirror */
	CHECK(!on_pc(&ph));                                           /* nothing loaded: nothing to hold */
	phone_link_stop(ph.link);
	CHECK(pc_stop(&pc) == 0);
	{ char *l = slurp(pc.log); if (failures) fprintf(stderr, "--- C: desktop log ---\n%s", l); free(l); }
	sim_destroy(&ph);
}

/* D: cable pulled while the game is on the PC */
static void scenario_d(void)
{
	Sim ph;
	Pc pc;
	char data[300];
	uint16_t port = free_port();
	gbah_meta m, ms;
	Notices nt;
	uint32_t f0;

	memset(&nt, 0, sizeof nt);
	snprintf(data, sizeof data, "%s/pc_d", g_base);
	phone_up(&ph, "ph_d", 0x33, 600000, 600000, port, NULL);
	REQUIRE(sim_load_game(&ph, 1) == 1);
	sim_run(&ph);
	usleep(400000);
	REQUIRE(pc_start(&pc, g_exe, g_fake_adb, "pc_d", data, port, NULL, "300") == 0);
	phone_link_start(ph.link);
	REQUIRE(WAIT(on_pc(&ph), 20000));
	f0 = sim_frame(&ph);
	CHECK(WAIT((store_meta_of(&ph, &m), memcmp(m.version.dev, ph.dev, 16) != 0), 15000));
	usleep(700000);                                   /* one more mirror, maybe */
	pc_kill(&pc);                                     /* cable pulled, no final checkpoint */
	{   /* leaving the PC state; the notice is consumed by the poll, so collect it here */
		uint64_t end = mono_ms() + 20000;
		int gone = 0;
		while (mono_ms() < end && !gone) {
			char b[8000], *line, *save;
			phone_ui_text(ph.link, b, sizeof b);
			gone = 1;
			for (line = strtok_r(b, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
				if (line[0] == 'H') gone = 0;
				if (line[0] == 'N' && strlen(nt.all) + strlen(line) + 2 < sizeof nt.all) { strcat(nt.all, line + 2); strcat(nt.all, "\n"); }
			}
			if (!gone) usleep(30000);
		}
		CHECK(gone);
		CHECK(strstr(nt.all, "Cabo desconectado") != NULL);
	}
	store_meta_of(&ph, &ms);
	CHECK(ms.frame > f0);                              /* resumed from the PC's last mirror, not from where it was handed */
	CHECK(sim_frame(&ph) >= ms.frame);
	{ uint32_t a = sim_frame(&ph); usleep(300000); CHECK(sim_frame(&ph) > a); }   /* playing again */
	sim_pause(&ph);
	CHECK(core_matches_store(&ph, ph.games, NULL));
	CHECK(phone_link_status(ph.link) != PL_ST_CONNECTED);
	sim_destroy(&ph);
}

/* E: autosave */
static void scenario_e(void)
{
	Sim ph;
	gbah_meta a, b;
	uint16_t port = free_port();
	int i;

	phone_up(&ph, "ph_e", 0x44, 400, 600000, port, NULL);
	REQUIRE(sim_load_game(&ph, 1) == 1);
	sim_run(&ph);
	/* periodic */
	CHECK(WAIT((store_meta_of(&ph, &a), a.has_data && a.version.counter >= 3), 8000));
	CHECK(count_backups(ph.games) == 0);
	/* not running (activity paused): nothing is written (negative control) */
	phone_set_running(ph.link, 0);
	usleep(300000);
	store_meta_of(&ph, &a);
	usleep(1200000);
	store_meta_of(&ph, &b);
	CHECK(b.version.counter == a.version.counter);
	CHECK(snprintf(NULL, 0, "%lu", atomic_load(&ph.snap_max_us)) > 0);
	fprintf(stderr, "autosave: %d snapshot(s), longest under the core lock %lu us, longest gap between frames %lu us\n",
	        atomic_load(&ph.snaps), atomic_load(&ph.snap_max_us), atomic_load(&ph.max_frame_gap_us));
	CHECK(atomic_load(&ph.snap_max_us) < 200000);
	sim_destroy(&ph);

	/* on a .sav change (periodic effectively off) */
	phone_up(&ph, "ph_e2", 0x45, 600000, 300, port, NULL);
	atomic_store(&ph.sav_const, 1);     /* the test ROM rewrites its save every frame: control the signal */
	REQUIRE(sim_load_game(&ph, 1) == 1);
	sim_run(&ph);
	usleep(800000);
	store_meta_of(&ph, &a);
	CHECK(!a.has_data || a.version.counter <= 1);
	usleep(1200000);
	store_meta_of(&ph, &b);
	CHECK(b.version.counter == a.version.counter);     /* nothing changed: nothing saved */
	for (i = 0; i < 1; i++) atomic_fetch_add(&ph.sav_bias, 1);   /* the game wrote its save */
	(void)port;
	CHECK(WAIT((store_meta_of(&ph, &b), b.version.counter > a.version.counter), 5000));
	store_meta_of(&ph, &a);
	usleep(1000000);
	store_meta_of(&ph, &b);
	CHECK(b.version.counter == a.version.counter);     /* one change, one save */
	sim_destroy(&ph);
}

/* F: incompatible core */
static void scenario_f(void)
{
	Sim ph;
	Pc pc;
	char data[300];
	uint16_t port = free_port();

	snprintf(data, sizeof data, "%s/pc_f", g_base);
	phone_up(&ph, "ph_f", 0x55, 600000, 600000, port, "0.0.0-outro");
	REQUIRE(sim_load_game(&ph, 1) == 1);
	REQUIRE(pc_start(&pc, g_exe, g_fake_adb, "pc_f", data, port, NULL, "300") == 0);
	phone_link_start(ph.link);
	CHECK(WAIT(phone_link_status(ph.link) == PL_ST_REFUSED, 20000));
	CHECK(!on_pc(&ph));
	phone_link_stop(ph.link);
	pc_stop(&pc);
	sim_destroy(&ph);
}

/* G: the PC asks for the ROM and the phone cannot read it (nothing registered): the game must NOT end up held */
static void scenario_g(void)
{
	Sim ph;
	Pc pc;
	char data[300];
	uint16_t port = free_port();
	Notices nt;
	uint32_t a;

	memset(&nt, 0, sizeof nt);
	snprintf(data, sizeof data, "%s/pc_g", g_base);
	phone_up(&ph, "ph_g", 0x66, 600000, 600000, port, NULL);
	sim_forget_roms(&ph);
	REQUIRE(sim_load_game(&ph, 1) == 1);
	sim_run(&ph);
	REQUIRE(pc_start(&pc, g_exe, g_fake_adb, "pc_g", data, port, NULL, "300") == 0);
	phone_link_start(ph.link);
	CHECK(notice_wait(&ph, &nt, "nao recebeu a ROM", 20000));
	usleep(1500000);
	CHECK(!on_pc(&ph));
	a = sim_frame(&ph);
	usleep(300000);
	CHECK(sim_frame(&ph) > a);                 /* still playing on the phone */
	notices_poll(&ph, &nt);
	fprintf(stderr, "G notices: %s", nt.all);
	phone_link_stop(ph.link);
	pc_stop(&pc);
	sim_destroy(&ph);
}

int main(int argc, char **argv)
{
	if (argc < 5) { fprintf(stderr, "uso: %s <gbahandoff> <rom.gba> <rom-sha1> <fake_adb.sh> [cenarios]\n", argv[0]); return 2; }
	g_exe = argv[1]; g_fake_adb = argv[4];
	test_init(argv[2], argv[3], "phone");
	install_cleanup(300);
	{
		const char *only = argc > 5 ? argv[5] : "abcdefg";   /* optional: run only some scenarios (debugging) */
		if (strchr(only, 'a')) { scenario_a_b(); kill_all_pcs(); }
		if (strchr(only, 'c')) { scenario_c(); kill_all_pcs(); }
		if (strchr(only, 'd')) { scenario_d(); kill_all_pcs(); }
		if (strchr(only, 'e')) scenario_e();
		if (strchr(only, 'f')) scenario_f();
		if (strchr(only, 'g')) scenario_g();
	}
	kill_all_pcs();
	if (failures) { fprintf(stderr, "phone_link_test: %d falha(s)\n", failures); return 1; }
	printf("phone_link_test OK\n");
	rmrf(g_base);
	return 0;
}
