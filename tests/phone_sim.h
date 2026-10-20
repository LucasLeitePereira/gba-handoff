/* Shared helpers of the phone-side tests: a simulated phone (the REAL engine android/.../phone_link.c
 * and core_host.c over a real gbah_core, with an emulation thread that obeys the hold flag exactly
 * like jni_bridge.c) and the management of REAL desktop processes (fork + PR_SET_PDEATHSIG so none
 * outlives the test, whatever happens). POSIX/Linux only. */
#ifndef PHONE_SIM_H
#define PHONE_SIM_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "core_host.h"
#include "gbah_core.h"
#include "gbah_sync.h"
#include "gbahp_net.h"
#include "phone_link.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "%s:%d: REQUIRE failed: %s\n", __FILE__, __LINE__, #c); failures++; return; } } while (0)

static const char *g_rom, *g_sha_hex;
static uint8_t g_sha[20];
static uint8_t *g_rom_bytes;
static size_t g_rom_len;
static char g_base[256];

static uint64_t mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }
static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static void rmrf(const char *dir)
{
	char cmd[400];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
	if (system(cmd) != 0) { /* best effort */ }
}
static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	char *b;
	long n;
	if (!f) return strdup("");
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	b = (char *)calloc(1, (size_t)n + 1);
	if (fread(b, 1, (size_t)n, f) != (size_t)n) b[0] = 0;
	fclose(f);
	return b;
}
static int count_backups(const char *games_dir)
{
	DIR *d = opendir(games_dir);
	struct dirent *e;
	int n = 0;
	if (!d) return -1;
	while ((e = readdir(d)) != NULL)
		if (strstr(e->d_name, ".backup-") && strstr(e->d_name, ".state")) n++;
	closedir(d);
	return n;
}
static uint16_t free_port(void)
{
	gbahp_sock s = gbahp_net_listen_loopback(0);
	uint16_t p = gbahp_net_local_port(s);
	gbahp_net_close(s);
	return p;
}
static void test_init(const char *rom, const char *sha_hex, const char *tag)
{
	int i;
	g_rom = rom; g_sha_hex = sha_hex;
	for (i = 0; i < 20; i++) g_sha[i] = (uint8_t)(hexval(sha_hex[2 * i]) * 16 + hexval(sha_hex[2 * i + 1]));
	{
		void *b = NULL;
		if (gbah_read_file(g_rom, &b, &g_rom_len) != 0) { fprintf(stderr, "cannot read the ROM\n"); exit(2); }
		g_rom_bytes = (uint8_t *)b;
	}
	signal(SIGPIPE, SIG_IGN);
	snprintf(g_base, sizeof g_base, "/tmp/gbah_%s_XXXXXX", tag);
	if (!mkdtemp(g_base)) { perror("mkdtemp"); exit(2); }
	setenv("SDL_VIDEO_DRIVER", "dummy", 1);
	setenv("SDL_AUDIO_DRIVER", "dummy", 1);
	setenv("XDG_DATA_HOME", g_base, 1);
}

/* ------------------------------------------------------------------ desktop processes */

typedef struct { pid_t pid; char log[300]; char data[300]; uint16_t port; } Pc;

static volatile pid_t g_pids[64];
static void reg_add(pid_t p) { int i; for (i = 0; i < 64; i++) if (g_pids[i] == 0) { g_pids[i] = p; return; } }
static void reg_del(pid_t p) { int i; for (i = 0; i < 64; i++) if (g_pids[i] == p) g_pids[i] = 0; }
static void kill_all_pcs(void)   /* async-signal-safe */
{
	int i, st;
	for (i = 0; i < 64; i++) {
		pid_t p = g_pids[i];
		if (p > 0) { kill(p, SIGKILL); waitpid(p, &st, 0); g_pids[i] = 0; }
	}
}
static void on_signal(int sig) { kill_all_pcs(); signal(sig, SIG_DFL); raise(sig); }
static void install_cleanup(int alarm_s)
{
	signal(SIGTERM, on_signal); signal(SIGINT, on_signal); signal(SIGALRM, on_signal);
	atexit(kill_all_pcs);
	alarm((unsigned)alarm_s);
}

/* rom_arg: ROM path given to the desktop (it plays it), or NULL (idle PC) */
static int pc_start(Pc *pc, const char *exe, const char *fake_adb, const char *name, const char *data, uint16_t port,
                    const char *rom_arg, const char *mirror_ms)
{
	char portarg[16];
	const char *args[32];
	int n = 0;
	pid_t parent = getpid();
	snprintf(pc->log, sizeof pc->log, "%s/%s.log", g_base, name);
	snprintf(pc->data, sizeof pc->data, "%s", data);
	pc->port = port;
	snprintf(portarg, sizeof portarg, "%u", port);
	args[n++] = exe; args[n++] = "--port"; args[n++] = portarg;
	args[n++] = "--adb"; args[n++] = fake_adb;
	args[n++] = "--software"; args[n++] = "--data-dir"; args[n++] = pc->data;
	args[n++] = "--mirror-ms"; args[n++] = mirror_ms; args[n++] = "--sav-min-ms"; args[n++] = "600000";
	if (rom_arg) args[n++] = rom_arg;
	args[n] = NULL;
	pc->pid = fork();
	if (pc->pid < 0) { perror("fork"); return -1; }
	if (pc->pid == 0) {
		int fd = open(pc->log, O_WRONLY | O_CREAT | O_APPEND, 0600);
		if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		if (getppid() != parent) _exit(99);
		execv(exe, (char *const *)args);
		_exit(127);
	}
	reg_add(pc->pid);
	return 0;
}
static int pc_stop(Pc *pc)   /* SIGTERM -> final checkpoint; exit code, -1 if killed */
{
	int st = 0, i;
	kill(pc->pid, SIGTERM);
	for (i = 0; i < 150; i++) {
		if (waitpid(pc->pid, &st, WNOHANG) == pc->pid) { reg_del(pc->pid); return WIFEXITED(st) ? WEXITSTATUS(st) : -2; }
		usleep(100000);
	}
	kill(pc->pid, SIGKILL);
	waitpid(pc->pid, &st, 0);
	reg_del(pc->pid);
	return -1;
}
static void pc_kill(Pc *pc)   /* the cable is pulled / the PC dies: no final checkpoint */
{
	int st;
	kill(pc->pid, SIGKILL);
	waitpid(pc->pid, &st, 0);
	reg_del(pc->pid);
}
static int log_has(Pc *pc, const char *needle)
{
	char *l = slurp(pc->log);
	int r = strstr(l, needle) != NULL;
	free(l);
	return r;
}
static int log_wait(Pc *pc, const char *needle, int timeout_ms)
{
	uint64_t end = mono_ms() + (uint64_t)timeout_ms;
	while (mono_ms() < end) {
		if (log_has(pc, needle)) return 1;
		usleep(50000);
	}
	return 0;
}
static void log_dump(Pc *pc, const char *tag)
{
	char *l = slurp(pc->log);
	fprintf(stderr, "--- %s: desktop log ---\n%s", tag, l);
	free(l);
}

/* ------------------------------------------------------------------ simulated phone */

typedef struct {
	core_host_ctx ctx;          /* MUST be first: the wrappers get &ctx as `user` */
	gbah_core *core;
	pthread_mutex_t mtx;
	atomic_int hold, run;
	pthread_t th;
	int th_on;
	phone_link *link;
	phone_host orig;
	atomic_ullong sav_bias;     /* added to the reported sav hash: simulates "the game wrote its save" */
	atomic_int sav_const;       /* 1: report a constant hash + bias (the test ROM rewrites its save every frame) */
	atomic_ulong snap_max_us;   /* longest time the core lock was held by a snapshot */
	atomic_int snaps;
	atomic_ulong max_frame_gap_us;
	char dir[300], games[320], roms[320], backup[320];
	uint8_t dev[16];
	char source[600];           /* ROM source given to the engine */
} Sim;

typedef struct { Sim *s; } SimRef;

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static int w_snapshot(void *u, phone_snap *s)
{
	Sim *sim = (Sim *)u;
	uint64_t t0 = now_us(), dt;
	int rc = sim->orig.snapshot(u, s);
	if (rc == 0 && atomic_load(&sim->sav_const)) {   /* same contract as the hash: save = those 8 bytes */
		unsigned long long v = 0x5eed + atomic_load(&sim->sav_bias);
		free(s->sav);
		s->sav = malloc(sizeof v); memcpy(s->sav, &v, sizeof v); s->sav_n = sizeof v;
	}
	dt = now_us() - t0;
	if (dt > atomic_load(&sim->snap_max_us)) atomic_store(&sim->snap_max_us, (unsigned long)dt);
	atomic_fetch_add(&sim->snaps, 1);
	return rc;
}
static uint64_t w_sav_hash(void *u)
{
	Sim *sim = (Sim *)u;
	if (atomic_load(&sim->sav_const)) { unsigned long long v = 0x5eed + atomic_load(&sim->sav_bias); return phone_hash(&v, sizeof v); }
	return sim->orig.sav_hash(u) + atomic_load(&sim->sav_bias);
}
/* what a content:// source costs in the app: a call into the host. Here: the path after the last '|' */
static int w_open_rom(void *u, const char *source)
{
	(void)u;
	const char *bar = strchr(source, '|');
	return bar ? open(bar + 1, O_RDONLY) : -1;
}

static void *emu_main(void *a)
{
	Sim *s = (Sim *)a;
	uint64_t last = 0;
	while (atomic_load(&s->run)) {
		uint64_t t;
		pthread_mutex_lock(&s->mtx);
		if (atomic_load(&s->hold)) { pthread_mutex_unlock(&s->mtx); usleep(8000); last = 0; continue; }
		gbah_set_input(s->core, 0);
		gbah_run_frame(s->core);
		pthread_mutex_unlock(&s->mtx);
		t = now_us();
		if (last && t - last > atomic_load(&s->max_frame_gap_us)) atomic_store(&s->max_frame_gap_us, (unsigned long)(t - last));
		last = t;
		usleep(4000);
	}
	return NULL;
}

static void sim_run(Sim *s)
{
	if (s->th_on) return;
	atomic_store(&s->run, 1);
	pthread_create(&s->th, NULL, emu_main, s);
	s->th_on = 1;
	phone_set_running(s->link, 1);
}
static void sim_pause(Sim *s)
{
	phone_set_running(s->link, 0);
	if (!s->th_on) return;
	atomic_store(&s->run, 0);
	pthread_join(s->th, NULL);
	s->th_on = 0;
}
static uint32_t sim_frame(Sim *s)
{
	uint32_t f;
	pthread_mutex_lock(&s->mtx);
	f = gbah_frame_counter(s->core);
	pthread_mutex_unlock(&s->mtx);
	return f;
}
static uint64_t sim_hash(Sim *s)
{
	uint64_t h;
	pthread_mutex_lock(&s->mtx);
	h = gbah_framebuffer_hash(s->core);
	pthread_mutex_unlock(&s->mtx);
	return h;
}

/* load = true: the ROM is loaded in the core and the stored progress resumed (what GameActivity does) */
static int sim_create(Sim *s, const char *name, uint8_t devbyte, unsigned autosave_ms, unsigned sav_min_ms, uint16_t port,
                      const char *core_version, int use_content_source)
{
	phone_cfg cfg;
	phone_rom rom;
	memset(s, 0, sizeof *s);
	snprintf(s->dir, sizeof s->dir, "%s/%s", g_base, name);
	snprintf(s->games, sizeof s->games, "%s/games", s->dir);
	snprintf(s->roms, sizeof s->roms, "%s/roms", s->dir);
	snprintf(s->backup, sizeof s->backup, "%s/legacy-backup", s->dir);
	memset(s->dev, devbyte, 16);
	pthread_mutex_init(&s->mtx, NULL);
	s->core = gbah_create(0);
	if (!s->core) return -1;
	s->ctx.core = &s->core; s->ctx.mtx = &s->mtx; s->ctx.hold = (atomic_int *)&s->hold;
	memset(&cfg, 0, sizeof cfg);
	cfg.games_dir = s->games; cfg.roms_dir = s->roms;
	memcpy(cfg.device_id, s->dev, 16);
	cfg.device_name = "TestPhone";
	cfg.core_version = core_version;
	cfg.port = port;
	cfg.autosave_ms = autosave_ms;
	cfg.sav_min_ms = sav_min_ms;
	cfg.tick_ms = 100;
	s->ctx.open_rom = w_open_rom;
	core_host_fill(&cfg.host, &s->ctx);
	s->orig = cfg.host;
	cfg.host.user = s;                 /* wrappers and originals share the same pointer (ctx is first) */
	cfg.host.snapshot = w_snapshot;
	cfg.host.sav_hash = w_sav_hash;
	s->link = phone_create(&cfg);
	if (!s->link) return -1;
	memset(&rom, 0, sizeof rom);
	memcpy(rom.sha1, g_sha, 20);
	rom.size = g_rom_len;
	if (use_content_source) snprintf(rom.source, sizeof rom.source, "content://test/rom|%s", g_rom);
	else snprintf(rom.source, sizeof rom.source, "%s", g_rom);
	snprintf(s->source, sizeof s->source, "%s", rom.source);
	phone_set_roms(s->link, &rom, 1);
	return 0;
}
/* the phone has no ROM table entry (nothing registered) */
static void sim_forget_roms(Sim *s) { phone_set_roms(s->link, NULL, 0); }

static int sim_load_game(Sim *s, int resume)
{
	int rc;
	pthread_mutex_lock(&s->mtx);
	rc = gbah_load_rom_buffer(s->core, g_rom_bytes, g_rom_len);
	pthread_mutex_unlock(&s->mtx);
	if (rc != 0) return rc;
	phone_game_loaded(s->link);
	return resume ? phone_resume_stored(s->link) : 0;
}

static void sim_destroy(Sim *s)
{
	sim_pause(s);
	phone_destroy(s->link);
	gbah_destroy(s->core);
	pthread_mutex_destroy(&s->mtx);
}

/* ---- UI text helpers */

static int ui_has(Sim *s, const char *prefix)
{
	char buf[8000];
	char *line, *save;
	int found = 0;
	/* notices are consumed by phone_ui_text: callers that care use ui_wait_notice */
	phone_ui_text(s->link, buf, sizeof buf);
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
		if (!strncmp(line, prefix, strlen(prefix))) found = 1;
	return found;
}
__attribute__((unused)) static int ui_wait(Sim *s, const char *prefix, int want, int timeout_ms)
{
	uint64_t end = mono_ms() + (uint64_t)timeout_ms;
	while (mono_ms() < end) {
		if (ui_has(s, prefix) == want) return 1;
		usleep(40000);
	}
	return 0;
}

/* Notices are consumed when read; this keeps what it has seen. */
typedef struct { char all[4000]; } Notices;
static void notices_poll(Sim *s, Notices *n)
{
	char buf[8000];
	char *line, *save;
	phone_ui_text(s->link, buf, sizeof buf);
	for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
		if (line[0] == 'N' && strlen(n->all) + strlen(line) + 2 < sizeof n->all) { strcat(n->all, line + 2); strcat(n->all, "\n"); }
}
__attribute__((unused)) static int notice_wait(Sim *s, Notices *n, const char *needle, int timeout_ms)
{
	uint64_t end = mono_ms() + (uint64_t)timeout_ms;
	while (mono_ms() < end) {
		notices_poll(s, n);
		if (strstr(n->all, needle)) return 1;
		usleep(40000);
	}
	return 0;
}

/* ---- store / determinism helpers */

static void store_meta_of(Sim *s, gbah_meta *m)
{
	memset(m, 0, sizeof *m);
	gbah_store_meta(s->games, g_sha, s->dev, m, NULL, NULL);
}

static gbah_core *new_core(void)
{
	gbah_core *c = gbah_create(0);
	if (!c || gbah_load_rom_buffer(c, g_rom_bytes, g_rom_len) != 0) { fprintf(stderr, "cannot create a reference core\n"); exit(2); }
	return c;
}

/* The phone core (frozen by the caller or not running) must be the stored state advanced by the
 * frames it ran since: restore the store in a fresh core, run the same number of frames with no
 * input, compare the screen. Returns 1 if equal. delta_out: frames compared. */
static int core_matches_store_skew(Sim *s, const char *games_dir, int *delta_out, int skew)
{
	void *st = NULL, *sv = NULL, *a = NULL, *b = NULL;
	size_t sn = 0, vn = 0, an, bn;
	gbah_core *ref = new_core();
	uint32_t fc, k;
	int ok = 0;
	for (k = 0; k < 300; k++) gbah_run_frame(ref);   /* a used core, like the phone's: the restore does not rewrite every field */
	if (gbah_store_read(games_dir, g_sha, &st, &sn, &sv, &vn) != 0) { gbah_destroy(ref); return 0; }
	pthread_mutex_lock(&s->mtx);
	fc = gbah_frame_counter(s->core);
	an = gbah_state_save(s->core, &a);
	pthread_mutex_unlock(&s->mtx);
	if (gbah_handoff_restore(ref, st, sn, vn ? sv : NULL, vn) == 0 && gbah_frame_counter(ref) <= fc) {
		int d = (int)(fc - gbah_frame_counter(ref));
		gbah_set_input(ref, 0);                           /* the emulation thread presses nothing */
		for (k = 0; k < (uint32_t)(d + skew); k++) gbah_run_frame(ref);
		bn = gbah_state_save(ref, &b);
		/* Whole machine state, not only the (static) screen. Ignored: mGBA's audio block [304,496) and the
		 * output sample buffers [880,976) (offsetof(GBASerializedState, audio / samples..reserved)):
		 * their residue differs between a core that loaded a state and one that ran there (found by
		 * diffing, a handful of bytes); CPU, timers, DMA, IO, palette, VRAM, OAM and RAM must be identical. */
		ok = an && an == bn;
		for (k = 0; ok && k < an; k++)
			if (((uint8_t *)a)[k] != ((uint8_t *)b)[k] && !((k >= 304 && k < 496) || (k >= 880 && k < 976))) ok = 0;
		if (!ok && getenv("PL_DBG")) {
			size_t i, nd = 0, first = (size_t)-1;
			for (i = 0; i < an && i < bn; i++) if (((uint8_t *)a)[i] != ((uint8_t *)b)[i]) { if (first == (size_t)-1) first = i; nd++; }
			for (i = 0; i < an && i < bn; i++) if (((uint8_t *)a)[i] != ((uint8_t *)b)[i] && nd < 40) fprintf(stderr, "  off %zu: %02x vs %02x\n", i, ((uint8_t *)a)[i], ((uint8_t *)b)[i]);
			fprintf(stderr, "[dbg] state mismatch: sizes %zu/%zu, %zu bytes differ, first at %zu, delta %d skew %d fc %u\n", an, bn, nd, first, d, skew, fc);
		}
		if (delta_out) *delta_out = d;
	}
	gbah_free(a); gbah_free(b); gbah_free(st); gbah_free(sv);
	gbah_destroy(ref);
	return ok;
}
/* The phone core (frozen by the caller) must be the stored state advanced by the frames it ran since. */
static int core_matches_store(Sim *s, const char *games_dir, int *delta_out)
{
	return core_matches_store_skew(s, games_dir, delta_out, 0);
}

static int pc_data_meta(const char *pc_data, gbah_meta *m)
{
	char games[400];
	uint8_t dev[16] = { 0 };
	snprintf(games, sizeof games, "%s/games", pc_data);
	memset(m, 0, sizeof *m);
	return gbah_store_meta(games, g_sha, dev, m, NULL, NULL);
}

#endif
