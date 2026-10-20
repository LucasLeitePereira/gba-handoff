/* End to end handoff test: the REAL desktop binary against a simulated phone, over real TCP.
 *
 *  A  phone -> PC handoff with ROM transfer (negative control: corrupted ROM is refused), PC
 *     plays, MIRROR pushes (and the base they move), "back to the phone" (CONTROL RETURN) whose
 *     state, loaded in a fresh core, reproduces the frame hashes of an independent reference
 *     (determinism end to end, with negative controls), PC stops mirroring while waiting.
 *  B  conflict while the PC plays: PC pauses (no mirrors), sends THUMB, refuses a HANDOFF with
 *     CONFLICT; the phone picks the PC version (CONTROL FETCH -> RESOLVED push); the PC STAYS paused
 *     ("AwaitPlace", one owner) until PLACE_PC, then mirrors again.
 *  C  conflict with the PC idle: the phone picks its own version (RESOLVED push); the loser becomes
 *     a backup; only the 3 newest backups are kept.
 *  D  cable pulled while the PC plays: it keeps running, versions advance from the synced base;
 *     on reconnection the PC (now ahead) pushes its version.
 *  E  PC asks for pushes to a phone without the ROM: ACK NO_ROM -> ROM_PUSH -> push again; and the
 *     "the .sav changed" trigger sends a MIRROR without waiting for the period.
 *  F  second instance on the same port reports PORT_BUSY.
 *  G  one owner: INPUT incl. L/R reaches the PC; after a conflict key state is cleared; PLACE_PC runs the PC.
 *  H  one owner: PLACE_PHONE keeps the PC paused; cable lost in AwaitPlace does not make the PC play alone.
 *  I  settings: the PC announces them, SETTINGS_SET persists (also after a restart).
 * POSIX only (fork/exec); the Windows build is cross-compiled and not run here. */
#include "gbah_core.h"
#include "gbah_sync.h"
#include "gbahp_net.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
/* a failed precondition aborts the scenario (the rest would only produce noise) */
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "%s:%d: REQUIRE failed: %s\n", __FILE__, __LINE__, #c); failures++; return; } } while (0)

static const char *g_exe, *g_rom, *g_sha_hex, *g_fake_adb;
static uint8_t g_sha[20];
static char g_base[256];
static uint8_t *g_rom_bytes;
static size_t g_rom_len;

static uint64_t mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

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

static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* ------------------------------------------------------------------ PC process */

typedef struct { pid_t pid; char log[300]; char data[300]; uint16_t port; } Pc;

static uint16_t free_port(void)
{
	gbahp_sock s = gbahp_net_listen_loopback(0);
	uint16_t p = gbahp_net_local_port(s);
	gbahp_net_close(s);
	return p;
}

/* Every desktop process we start is registered so that NO path (failed REQUIRE, timeout of this
 * test, SIGTERM/SIGINT from ctest) leaves a `gbahandoff` orphan behind: kill_all_pcs() runs after
 * every scenario, from the signal handlers and from atexit; the child also asks the kernel to
 * SIGKILL it if this test dies first (PR_SET_PDEATHSIG, Linux). */
static volatile pid_t g_pids[64];

static void reg_add(pid_t p)
{
	int i;
	for (i = 0; i < 64; i++) if (g_pids[i] == 0) { g_pids[i] = p; return; }
}
static void reg_del(pid_t p)
{
	int i;
	for (i = 0; i < 64; i++) if (g_pids[i] == p) g_pids[i] = 0;
}
static void kill_all_pcs(void)   /* async-signal-safe */
{
	int i, st;
	for (i = 0; i < 64; i++) {
		pid_t p = g_pids[i];
		if (p > 0) { kill(p, SIGKILL); waitpid(p, &st, 0); g_pids[i] = 0; }
	}
}
static void on_signal(int sig)
{
	kill_all_pcs();
	signal(sig, SIG_DFL);
	raise(sig);
}

/* extra: NULL-terminated additional args (may include the ROM path last) */
static int pc_start(Pc *pc, const char *name, const char *data, uint16_t port, const char *const *extra)
{
	char portarg[16];
	const char *args[32];
	int n = 0, i;
	pid_t parent = getpid();
	snprintf(pc->log, sizeof pc->log, "%s/%s.log", g_base, name);
	snprintf(pc->data, sizeof pc->data, "%s", data);
	pc->port = port;
	snprintf(portarg, sizeof portarg, "%u", port);
	args[n++] = g_exe; args[n++] = "--port"; args[n++] = portarg;
	args[n++] = "--adb"; args[n++] = g_fake_adb;
	args[n++] = "--software"; args[n++] = "--data-dir"; args[n++] = pc->data;
	for (i = 0; extra && extra[i] && n < 30; i++) args[n++] = extra[i];
	args[n] = NULL;
	pc->pid = fork();
	if (pc->pid < 0) { perror("fork"); return -1; }
	if (pc->pid == 0) {
		int fd = open(pc->log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		if (getppid() != parent) _exit(99);   /* the test died between fork and prctl */
		execv(g_exe, (char *const *)args);
		_exit(127);
	}
	reg_add(pc->pid);
	return 0;
}

static int pc_alive(Pc *pc)
{
	int st;
	if (waitpid(pc->pid, &st, WNOHANG) == 0) return 1;
	reg_del(pc->pid);
	return 0;
}

/* SIGTERM -> SDL_EVENT_QUIT -> final checkpoint. Returns the exit code (-1 if it had to be killed). */
static int pc_stop(Pc *pc)
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

static int log_has(Pc *pc, const char *needle)
{
	char *l = slurp(pc->log);
	int r = strstr(l, needle) != NULL;
	free(l);
	return r;
}

/* ------------------------------------------------------------------ simulated phone */

typedef struct { uint8_t type; uint8_t *p; uint32_t len; } Msg;

typedef struct {
	gbahp_conn c;
	uint8_t dev[16];
	gbah_meta meta;          /* what the phone holds for the test game */
	int mirrors;             /* MIRROR pushes it auto-acknowledged */
	gbah_ver last_mirror;
	gbah_ver prev_mirror_base;
} Phone;

static void msg_free(Msg *m) { free(m->p); m->p = NULL; }

static int phone_connect(Phone *ph, uint16_t port, const char *core)
{
	time_t end = time(NULL) + 20;
	gbahp_sock s = GBAHP_BAD_SOCK;
	gbahp_hello h;
	gbahp_frame f;
	gbahp_hello_ack ack;
	uint8_t out[512];
	while (time(NULL) < end) {
		s = gbahp_net_connect_loopback(port, 300);
		if (s != GBAHP_BAD_SOCK) break;
		usleep(100000);
	}
	if (s == GBAHP_BAD_SOCK) return -1;
	gbahp_conn_init(&ph->c, s);
	memset(&h, 0, sizeof h);
	h.proto_version = GBAHP_VERSION; memcpy(h.device_id, ph->dev, 16);
	h.core_version = gbahp_str_c(core); h.name = gbahp_str_c("TestPhone");
	if (gbahp_conn_send(&ph->c, out, gbahp_encode_hello(&h, out, sizeof out)) != 0) return -1;
	if (gbahp_conn_recv(&ph->c, &f, 5000) != 1 || f.type != GBAHP_MSG_HELLO_ACK) return -1;
	if (gbahp_decode_hello_ack(f.payload, f.len, &ack) != 0 || ack.status != GBAHP_ACK_OK) return -1;
	return 0;
}

static int next_msg(Phone *ph, Msg *m, int timeout_ms)
{
	gbahp_frame f;
	int r = gbahp_conn_recv(&ph->c, &f, timeout_ms);
	if (r != 1) return r;
	m->type = f.type;
	m->len = f.len;
	m->p = (uint8_t *)malloc(f.len ? f.len : 1);
	memcpy(m->p, f.payload, f.len);
	return 1;
}

static void send_frame(Phone *ph, uint8_t *buf, size_t n) { CHECK(n && gbahp_conn_send(&ph->c, buf, n) == 0); }

static void send_ack(Phone *ph, const uint8_t sha[20], const gbah_ver *v, uint8_t status)
{
	gbahp_state_ack a;
	uint8_t out[128];
	memset(&a, 0, sizeof a);
	memcpy(a.sha1_rom, sha, 20); a.version = v->counter; memcpy(a.origin_device, v->dev, 16); a.status = status;
	send_frame(ph, out, gbahp_encode_state_ack(&a, out, sizeof out));
}

static void send_control(Phone *ph, uint8_t cmd, const uint8_t sha[20], uint32_t arg)
{
	gbahp_control c;
	uint8_t out[128];
	memset(&c, 0, sizeof c);
	c.cmd = cmd; memcpy(c.sha1, sha, 20); c.arg = arg;
	send_frame(ph, out, gbahp_encode_control(&c, out, sizeof out));
}

static void send_library(Phone *ph, const gbah_meta *m, uint64_t rom_size)
{
	gbahp_lib_entry e;
	uint8_t out[512];
	memset(&e, 0, sizeof e);
	if (!m) { send_frame(ph, out, gbahp_encode_library(&e, 0, out, sizeof out)); return; }
	memcpy(e.sha1, m->sha1, 20);
	e.size = rom_size;
	e.has_data = (uint8_t)m->has_data;
	memcpy(e.version_dev, m->version.dev, 16); e.version = m->version.counter;
	memcpy(e.base_dev, m->base.dev, 16); e.base = m->base.counter;
	e.timestamp_ms = m->timestamp_ms; e.frame = m->frame;
	send_frame(ph, out, gbahp_encode_library(&e, 1, out, sizeof out));
}

static void send_push(Phone *ph, const gbah_meta *m, uint8_t flags, const void *state, size_t sn, const void *sav, size_t vn,
                      const void *thumb, size_t tn)
{
	gbahp_state_push p;
	size_t cap = sn + vn + tn + 512, n;
	uint8_t *out = (uint8_t *)malloc(cap);
	memset(&p, 0, sizeof p);
	memcpy(p.sha1_rom, m->sha1, 20);
	p.version = m->version.counter; memcpy(p.origin_device, m->version.dev, 16);
	p.base = m->base.counter; memcpy(p.base_device, m->base.dev, 16);
	p.flags = flags; p.timestamp_ms = m->timestamp_ms; p.frame = m->frame;
	p.state = (const uint8_t *)state; p.state_len = (uint32_t)sn;
	p.sav = (const uint8_t *)sav; p.sav_len = (uint32_t)vn;
	p.thumb = (const uint8_t *)thumb; p.thumb_len = (uint32_t)tn;
	n = gbahp_encode_state_push(&p, out, cap);
	send_frame(ph, out, n);
	free(out);
}

/* ROM in `chunk`-byte ROM_PUSH pieces; corrupt != 0 flips one byte (negative control) */
static void send_rom(Phone *ph, const uint8_t *rom, size_t n, size_t chunk, int corrupt)
{
	size_t off = 0;
	uint8_t out[8192];
	uint8_t *copy = (uint8_t *)malloc(n);
	memcpy(copy, rom, n);
	if (corrupt) copy[n / 2] ^= 0xFF;
	while (off < n) {
		gbahp_rom_push p;
		size_t k = n - off < chunk ? n - off : chunk;
		memset(&p, 0, sizeof p);
		memcpy(p.sha1, g_sha, 20); p.total_size = n; p.offset = off; p.data = copy + off; p.data_len = (uint32_t)k;
		send_frame(ph, out, gbahp_encode_rom_push(&p, out, sizeof out));
		off += k;
	}
	free(copy);
}

/* The phone's reaction to a push that is not the one the test waits for (MIRROR), as the real
 * phone must do: decide with the version rules, ACK. */
static void phone_handle_mirror(Phone *ph, const Msg *m)
{
	gbahp_state_push sp;
	gbah_meta r;
	uint8_t st = GBAHP_STATE_ACK_OK;
	if (gbahp_decode_state_push(m->p, m->len, &sp) != 0) { CHECK(!"bad push"); return; }
	memset(&r, 0, sizeof r);
	memcpy(r.sha1, sp.sha1_rom, 20); r.has_data = 1;
	memcpy(r.version.dev, sp.origin_device, 16); r.version.counter = sp.version;
	memcpy(r.base.dev, sp.base_device, 16); r.base.counter = sp.base;
	r.timestamp_ms = sp.timestamp_ms; r.frame = sp.frame;
	switch (gbah_sync_decide(&ph->meta, &r)) {
	case GBAH_SYNC_RECEIVE:
		ph->prev_mirror_base = r.base;
		gbah_sync_adopt(&ph->meta, &r);
		ph->last_mirror = r.version;
		ph->mirrors++;
		break;
	case GBAH_SYNC_NOTHING: break;
	case GBAH_SYNC_SEND: st = GBAHP_STATE_ACK_STALE; break;
	default: st = GBAHP_STATE_ACK_CONFLICT; break;
	}
	{ gbah_ver v = r.version; send_ack(ph, sp.sha1_rom, &v, st); }
}

/* Reads until a message of `want` arrives (returned in *out). MIRROR pushes met on the way are
 * answered like the phone would; PONG/LIBRARY/CONTROL(PC_STATUS) are dropped. Returns 1 / 0 timeout. */
static int wait_type(Phone *ph, uint8_t want, Msg *out, int timeout_ms)
{
	uint64_t end = mono_ms() + (uint64_t)timeout_ms;
	while (mono_ms() < end) {
		Msg m;
		int r = next_msg(ph, &m, 200);
		if (r < 0) return 0;
		if (r == 0) continue;
		if (m.type == want) { *out = m; return 1; }
		if (m.type == GBAHP_MSG_STATE_PUSH) {
			gbahp_state_push sp;
			if (gbahp_decode_state_push(m.p, m.len, &sp) == 0 && (sp.flags & GBAHP_PUSH_MIRROR)) phone_handle_mirror(ph, &m);
		}
		msg_free(&m);
	}
	return 0;
}

/* STATE_PUSH with a given flag bit (answering MIRROR ones on the way) */
static int wait_push_flag(Phone *ph, uint8_t flag, Msg *out, int timeout_ms)
{
	uint64_t end = mono_ms() + (uint64_t)timeout_ms;
	while (mono_ms() < end) {
		Msg m;
		gbahp_state_push sp;
		if (!wait_type(ph, GBAHP_MSG_STATE_PUSH, &m, (int)(end - mono_ms()))) return 0;
		if (gbahp_decode_state_push(m.p, m.len, &sp) == 0 && (sp.flags & flag)) { *out = m; return 1; }
		if (gbahp_decode_state_push(m.p, m.len, &sp) == 0 && (sp.flags & GBAHP_PUSH_MIRROR)) phone_handle_mirror(ph, &m);
		msg_free(&m);
	}
	return 0;
}

static void phone_reset(Phone *ph, uint8_t devbyte)
{
	memset(ph, 0, sizeof *ph);
	memset(ph->dev, devbyte, 16);
	memcpy(ph->meta.sha1, g_sha, 20);
}

static void phone_close(Phone *ph)
{
	uint8_t out[64];
	gbahp_bye b = { 0 };
	gbahp_conn_send(&ph->c, out, gbahp_encode_bye(&b, out, sizeof out));
	gbahp_conn_close(&ph->c);
}

/* ------------------------------------------------------------------ helpers */

static gbah_core *new_core(void)
{
	gbah_core *c = gbah_create(0);
	if (!c || gbah_load_rom_buffer(c, g_rom_bytes, g_rom_len) != 0) { fprintf(stderr, "cannot create a reference core\n"); exit(2); }
	return c;
}

static void core_snapshot(gbah_core *c, void **st, size_t *sn, void **sv, size_t *vn)
{
	*sn = gbah_state_save(c, st);
	*vn = gbah_sram_export(c, sv);
}

static void rmrf(const char *dir)
{
	char cmd[400];
	snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
	if (system(cmd) != 0) { /* best effort */ }
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

static void store_meta(const char *data, gbah_meta *m)
{
	char games[400];
	uint8_t dev[16] = { 0 };
	snprintf(games, sizeof games, "%s/games", data);
	memset(m, 0, sizeof *m);
	gbah_store_meta(games, g_sha, dev, m, NULL, NULL);
}

/* STATE_PUSHes seen in `ms` (MIRROR ones are answered like the phone would) */
static int pushes_within(Phone *ph, int ms)
{
	uint64_t end = mono_ms() + (uint64_t)ms;
	int n = 0;
	while (mono_ms() < end) {
		Msg x;
		int r = next_msg(ph, &x, 200);
		if (r < 0) break;
		if (r == 1) {
			if (x.type == GBAHP_MSG_STATE_PUSH) { n++; phone_handle_mirror(ph, &x); }
			msg_free(&x);
		}
	}
	return n;
}

static void send_input(Phone *ph, uint16_t keys)
{
	static uint32_t seq;
	gbahp_input in;
	uint8_t out[64];
	in.seq = ++seq; in.keys = keys;
	send_frame(ph, out, gbahp_encode_input(&in, out, sizeof out));
}

/* waits for a MIRROR whose SRAM bytes [1],[2] (keys low/high written by the test ROM every frame) match */
static int wait_keys_in_mirror(Phone *ph, uint8_t lo, uint8_t hi, int timeout_ms)
{
	uint64_t end = mono_ms() + (uint64_t)timeout_ms;
	while (mono_ms() < end) {
		Msg m;
		gbahp_state_push sp;
		if (!wait_push_flag(ph, GBAHP_PUSH_MIRROR, &m, (int)(end - mono_ms()))) return 0;
		if (gbahp_decode_state_push(m.p, m.len, &sp) == 0 && sp.sav_len > 2 && sp.sav[1] == lo && sp.sav[2] == hi) {
			phone_handle_mirror(ph, &m);
			msg_free(&m);
			return 1;
		}
		phone_handle_mirror(ph, &m);
		msg_free(&m);
	}
	return 0;
}

/* The phone reports a version the PC does not descend from (CONFLICT), picks the PC's (FETCH) and acks the
 * RESOLVED push. The PC must be playing the game. Returns 0 on success, *frame = the resolved frame. */
static int conflict_fetch_pc(Phone *ph, uint32_t *frame)
{
	Msg m;
	gbahp_state_push sp;
	gbah_ver v;
	memset(&ph->meta, 0, sizeof ph->meta);
	memcpy(ph->meta.sha1, g_sha, 20);
	ph->meta.has_data = 1;
	memcpy(ph->meta.version.dev, ph->dev, 16); ph->meta.version.counter = 90;
	memcpy(ph->meta.base.dev, ph->dev, 16); ph->meta.base.counter = 1;
	ph->meta.timestamp_ms = gbah_now_ms(); ph->meta.frame = 4242;
	send_library(ph, &ph->meta, g_rom_len);
	if (!wait_type(ph, GBAHP_MSG_THUMB, &m, 5000)) return -1;
	msg_free(&m);
	send_control(ph, GBAHP_CTL_FETCH, g_sha, 0);
	if (!wait_push_flag(ph, GBAHP_PUSH_RESOLVED, &m, 5000)) return -2;
	if (gbahp_decode_state_push(m.p, m.len, &sp) != 0) { msg_free(&m); return -3; }
	*frame = sp.frame;
	memcpy(v.dev, sp.origin_device, 16); v.counter = sp.version;
	send_ack(ph, g_sha, &v, GBAHP_STATE_ACK_OK);
	ph->meta.version = v; ph->meta.base = v;
	msg_free(&m);
	return 0;
}

/* ------------------------------------------------------------------ scenario A */

static uint8_t g_pc_dev[16];   /* learnt in A from the first push */

static void scenario_a(const char *data)
{
	Pc pc;
	Phone ph;
	Msg m;
	uint16_t port = free_port();
	const char *extra[] = { "--mirror-ms", "1500", "--sav-min-ms", "600000", NULL };
	gbah_core *R = new_core();
	void *s0 = NULL, *sav0 = NULL;
	size_t s0n, sav0n, i;
	uint32_t f0;
	char romfile[400];
	gbahp_state_push sp;
	gbah_ver first_mirror = { { 0 }, 0 };
	char *l;

	phone_reset(&ph, 0x5A);
	snprintf(romfile, sizeof romfile, "%s/roms/%s.gba", data, g_sha_hex);
	REQUIRE(pc_start(&pc, "a", data, port, extra) == 0);
	REQUIRE(phone_connect(&ph, port, gbah_core_version()) == 0);

	/* the PC announces an empty library (no ROM, no game) and that it is idle */
	REQUIRE(wait_type(&ph, GBAHP_MSG_LIBRARY, &m, 5000));
	{ gbahp_library lib; CHECK(gbahp_decode_library(m.p, m.len, &lib) == 0 && lib.count == 0); }
	msg_free(&m);
	REQUIRE(wait_type(&ph, GBAHP_MSG_CONTROL, &m, 3000));
	{ gbahp_control c; CHECK(gbahp_decode_control(m.p, m.len, &c) == 0 && c.cmd == GBAHP_CTL_PC_STATUS && c.arg == 0); }
	msg_free(&m);

	/* the phone has played: reference core, 100 frames holding A+Start */
	gbah_set_input(R, 0x9);
	for (i = 0; i < 100; i++) gbah_run_frame(R);
	core_snapshot(R, &s0, &s0n, &sav0, &sav0n);
	f0 = gbah_frame_counter(R);
	REQUIRE(s0n > 0 && sav0n > 0);
	ph.meta.has_data = 1;
	memcpy(ph.meta.version.dev, ph.dev, 16); ph.meta.version.counter = 5;
	ph.meta.timestamp_ms = gbah_now_ms(); ph.meta.frame = f0;
	send_library(&ph, &ph.meta, g_rom_len);

	/* HANDOFF push; the PC has no ROM: it asks for it. NEGATIVE CONTROL: a corrupted ROM is refused. */
	send_push(&ph, &ph.meta, GBAHP_PUSH_HANDOFF, s0, s0n, sav0, sav0n, NULL, 0);
	REQUIRE(wait_type(&ph, GBAHP_MSG_ROM_REQUEST, &m, 5000));
	{ gbahp_rom_request rq; CHECK(gbahp_decode_rom_request(m.p, m.len, &rq) == 0 && !memcmp(rq.sha1, g_sha, 20)); }
	msg_free(&m);
	send_rom(&ph, g_rom_bytes, g_rom_len, 100, 1);
	REQUIRE(wait_type(&ph, GBAHP_MSG_STATE_ACK, &m, 5000));
	{ gbahp_state_ack a; CHECK(gbahp_decode_state_ack(m.p, m.len, &a) == 0 && a.status == GBAHP_STATE_ACK_NO_ROM); }
	msg_free(&m);
	CHECK(!file_exists(romfile));
	CHECK(log_has(&pc, "does not match its SHA-1"));

	/* second attempt with the right ROM, in several chunks */
	send_push(&ph, &ph.meta, GBAHP_PUSH_HANDOFF, s0, s0n, sav0, sav0n, NULL, 0);
	REQUIRE(wait_type(&ph, GBAHP_MSG_ROM_REQUEST, &m, 5000));
	msg_free(&m);
	send_rom(&ph, g_rom_bytes, g_rom_len, 100, 0);
	REQUIRE(wait_type(&ph, GBAHP_MSG_STATE_ACK, &m, 5000));
	{
		gbahp_state_ack a;
		CHECK(gbahp_decode_state_ack(m.p, m.len, &a) == 0 && a.status == GBAHP_STATE_ACK_OK);
		CHECK(a.version == 5 && !memcmp(a.origin_device, ph.dev, 16));
	}
	msg_free(&m);
	gbah_sync_mark_synced(&ph.meta);   /* confirmed: base = version on the phone too */
	CHECK(file_exists(romfile));

	/* the PC plays and mirrors every 1.5 s; each mirror moves the base (documented decision) */
	REQUIRE(wait_push_flag(&ph, GBAHP_PUSH_MIRROR, &m, 8000));
	REQUIRE(gbahp_decode_state_push(m.p, m.len, &sp) == 0);
	CHECK(memcmp(sp.origin_device, ph.dev, 16) != 0 && sp.version > 5);
	CHECK(sp.base == 5 && !memcmp(sp.base_device, ph.dev, 16));     /* based on the confirmed handoff */
	CHECK(sp.thumb_len == GBAHP_THUMB_BYTES);
	memcpy(g_pc_dev, sp.origin_device, 16);
	{   /* a mirror is a complete, loadable snapshot */
		gbah_core *t = new_core();
		CHECK(gbah_handoff_restore(t, sp.state, sp.state_len, sp.sav_len ? sp.sav : NULL, sp.sav_len) == 0);
		gbah_destroy(t);
	}
	first_mirror.counter = sp.version; memcpy(first_mirror.dev, sp.origin_device, 16);
	phone_handle_mirror(&ph, &m);
	msg_free(&m);
	REQUIRE(wait_push_flag(&ph, GBAHP_PUSH_MIRROR, &m, 8000));
	REQUIRE(gbahp_decode_state_push(m.p, m.len, &sp) == 0);
	CHECK(sp.version > first_mirror.counter);
	CHECK(sp.base == first_mirror.counter && !memcmp(sp.base_device, first_mirror.dev, 16));   /* the ACKed mirror became the base */
	phone_handle_mirror(&ph, &m);
	msg_free(&m);
	CHECK(ph.mirrors == 2);

	/* "back to the phone": CONTROL RETURN -> HANDOFF push */
	send_control(&ph, GBAHP_CTL_RETURN, g_sha, 0);
	REQUIRE(wait_push_flag(&ph, GBAHP_PUSH_HANDOFF, &m, 8000));
	REQUIRE(gbahp_decode_state_push(m.p, m.len, &sp) == 0);
	{
		gbah_core *ref = new_core(), *neg = new_core(), *fresh = new_core(), *off = new_core();
		uint32_t f1 = sp.frame, k;
		int diverged_s0 = 0, diverged_off = 0, same = 0;
		void *rsav = NULL;
		size_t rsn;
		gbah_ver rv;

		CHECK(f1 > f0);
		/* reference: the phone's S0, advanced by the same number of frames the PC ran (keys 0) */
		CHECK(gbah_handoff_restore(ref, s0, s0n, sav0, sav0n) == 0);
		CHECK(gbah_frame_counter(ref) == f0);
		while (gbah_frame_counter(ref) < f1) gbah_run_frame(ref);
		CHECK(gbah_frame_counter(ref) == f1);
		/* what the phone does with the pushed state */
		CHECK(gbah_handoff_restore(fresh, sp.state, sp.state_len, sp.sav_len ? sp.sav : NULL, sp.sav_len) == 0);
		/* NEGATIVE CONTROLS: (1) the state the PC started from, (2) a reference one frame short */
		CHECK(gbah_handoff_restore(neg, s0, s0n, sav0, sav0n) == 0);
		CHECK(gbah_handoff_restore(off, s0, s0n, sav0, sav0n) == 0);
		while (gbah_frame_counter(off) + 1 < f1) gbah_run_frame(off);

		rsn = gbah_sram_export(ref, &rsav);
		CHECK(rsn == sp.sav_len && rsn > 0 && !memcmp(rsav, sp.sav, rsn));   /* the .sav matches too */
		gbah_free(rsav);
		for (k = 0; k < 60; k++) {
			gbah_set_input(ref, k < 30 ? 0 : 0x1); gbah_set_input(fresh, k < 30 ? 0 : 0x1);
			gbah_set_input(neg, k < 30 ? 0 : 0x1); gbah_set_input(off, k < 30 ? 0 : 0x1);
			gbah_run_frame(ref); gbah_run_frame(fresh); gbah_run_frame(neg); gbah_run_frame(off);
			if (gbah_framebuffer_hash(ref) == gbah_framebuffer_hash(fresh)) same++;
			if (gbah_framebuffer_hash(ref) != gbah_framebuffer_hash(neg)) diverged_s0++;
			if (gbah_framebuffer_hash(ref) != gbah_framebuffer_hash(off)) diverged_off++;
		}
		CHECK(same == 60);              /* determinism end to end */
		CHECK(diverged_s0 > 0);         /* the control can fail: S0 is not S1 */
		CHECK(diverged_off > 0);        /* ... and a one-frame offset is visible */
		gbah_destroy(ref); gbah_destroy(neg); gbah_destroy(fresh); gbah_destroy(off);
		memcpy(rv.dev, sp.origin_device, 16); rv.counter = sp.version;
		send_ack(&ph, g_sha, &rv, GBAHP_STATE_ACK_OK);
	}
	msg_free(&m);

	/* after the ACK the PC is "Jogando no celular": paused, no more mirrors */
	usleep(500000);
	CHECK(log_has(&pc, "Jogando no celular"));
	{
		uint64_t end = mono_ms() + 3500;
		int pushes = 0;
		while (mono_ms() < end) {
			Msg x;
			int r = next_msg(&ph, &x, 200);
			if (r < 0) break;
			if (r == 1) { if (x.type == GBAHP_MSG_STATE_PUSH) pushes++; msg_free(&x); }
		}
		CHECK(pushes == 0);
	}
	phone_close(&ph);
	CHECK(pc_stop(&pc) == 0);
	l = slurp(pc.log);
	if (failures) fprintf(stderr, "--- A: desktop log ---\n%s", l);
	free(l);
	{   /* the returned version is the PC's, synced */
		gbah_meta mm;
		store_meta(data, &mm);
		CHECK(mm.has_data && !memcmp(mm.version.dev, g_pc_dev, 16) && gbah_ver_eq(&mm.base, &mm.version));
	}
	free(s0); free(sav0);
	gbah_destroy(R);
}

/* ------------------------------------------------------------------ scenario B (conflict, PC playing) */

static void scenario_b(const char *data)
{
	Pc pc;
	Phone ph;
	Msg m;
	uint16_t port = free_port();
	const char *extra[] = { "--mirror-ms", "1500", "--sav-min-ms", "600000", NULL, NULL };
	gbah_meta pcm;
	gbah_core *R = new_core();
	void *s = NULL, *sv = NULL;
	size_t sn, vn;
	int pushes = 0;
	uint64_t end;
	gbahp_state_push sp;
	uint32_t frame_resolved;
	char *l;

	extra[4] = g_rom;
	phone_reset(&ph, 0x5A);
	store_meta(data, &pcm);
	REQUIRE(pcm.has_data);
	REQUIRE(pc_start(&pc, "b", data, port, extra) == 0);
	REQUIRE(phone_connect(&ph, port, gbah_core_version()) == 0);

	/* the phone also advanced from an old common point: both diverged -> CONFLICT */
	ph.meta.has_data = 1;
	memcpy(ph.meta.version.dev, ph.dev, 16); ph.meta.version.counter = 9;
	memcpy(ph.meta.base.dev, ph.dev, 16); ph.meta.base.counter = 3;
	ph.meta.timestamp_ms = gbah_now_ms(); ph.meta.frame = 777;
	gbah_set_input(R, 0x4);
	{ int i; for (i = 0; i < 50; i++) gbah_run_frame(R); }
	core_snapshot(R, &s, &sn, &sv, &vn);
	REQUIRE(wait_type(&ph, GBAHP_MSG_LIBRARY, &m, 5000));
	{
		gbahp_library lib; gbahp_lib_entry e;
		CHECK(gbahp_decode_library(m.p, m.len, &lib) == 0 && lib.count == 1);
		gbahp_library_get(&lib, 0, &e);
		CHECK(e.has_data && e.size == g_rom_len && !memcmp(e.version_dev, g_pc_dev, 16));
	}
	msg_free(&m);
	send_library(&ph, &ph.meta, g_rom_len);

	/* the PC marks the conflict, pauses and sends the thumbnail of its version */
	REQUIRE(wait_type(&ph, GBAHP_MSG_THUMB, &m, 5000));
	{ gbahp_thumb t; CHECK(gbahp_decode_thumb(m.p, m.len, &t) == 0 && t.thumb_len == GBAHP_THUMB_BYTES && !memcmp(t.sha1, g_sha, 20)); }
	msg_free(&m);
	usleep(300000);
	CHECK(log_has(&pc, "Conflito em"));

	/* a HANDOFF during the conflict is refused with CONFLICT and nothing is stored */
	send_push(&ph, &ph.meta, GBAHP_PUSH_HANDOFF, s, sn, sv, vn, NULL, 0);
	REQUIRE(wait_type(&ph, GBAHP_MSG_STATE_ACK, &m, 5000));
	{ gbahp_state_ack a; CHECK(gbahp_decode_state_ack(m.p, m.len, &a) == 0 && a.status == GBAHP_STATE_ACK_CONFLICT); }
	msg_free(&m);

	/* paused while waiting: no MIRROR for 3 s (period is 1.5 s) -- the control for "it would mirror" is the end of this scenario */
	end = mono_ms() + 3200;
	while (mono_ms() < end) {
		Msg x;
		int r = next_msg(&ph, &x, 200);
		if (r < 0) break;
		if (r == 1) { if (x.type == GBAHP_MSG_STATE_PUSH) pushes++; msg_free(&x); }
	}
	CHECK(pushes == 0);

	/* the user picks the PC's version on the phone: FETCH -> RESOLVED push; the PC keeps playing */
	send_control(&ph, GBAHP_CTL_FETCH, g_sha, 0);
	REQUIRE(wait_push_flag(&ph, GBAHP_PUSH_RESOLVED, &m, 5000));
	REQUIRE(gbahp_decode_state_push(m.p, m.len, &sp) == 0);
	CHECK(sp.state_len > 0 && !memcmp(sp.origin_device, g_pc_dev, 16));
	frame_resolved = sp.frame;
	{
		gbah_ver v;
		memcpy(v.dev, sp.origin_device, 16); v.counter = sp.version;
		send_ack(&ph, g_sha, &v, GBAHP_STATE_ACK_OK);
		/* the phone adopts the PC version (its own one becomes a backup on the phone) */
		ph.meta.version = v; ph.meta.base = v;
	}
	msg_free(&m);
	/* ONE OWNER: after the resolution the PC does NOT resume by itself ("AwaitPlace"): no mirror for 3.2 s
	 * (period 1.5 s). The control that this silence is the pause, and not a dead mirror, is the next step. */
	CHECK(pushes_within(&ph, 3200) == 0);
	CHECK(log_has(&pc, "Playing -> Conflict") && log_has(&pc, "Conflict -> AwaitPlace"));
	CHECK(!log_has(&pc, "Conflict -> Playing"));
	/* the phone chose the PC: now (and only now) the PC runs and mirrors, with newer frames */
	send_control(&ph, GBAHP_CTL_PLACE_PC, g_sha, 0);
	REQUIRE(wait_push_flag(&ph, GBAHP_PUSH_MIRROR, &m, 8000));
	REQUIRE(gbahp_decode_state_push(m.p, m.len, &sp) == 0);
	CHECK(sp.frame > frame_resolved);
	phone_handle_mirror(&ph, &m);
	msg_free(&m);
	CHECK(log_has(&pc, "Versao do PC mantida"));
	phone_close(&ph);
	CHECK(pc_stop(&pc) == 0);
	l = slurp(pc.log);
	if (failures) fprintf(stderr, "--- B: desktop log ---\n%s", l);
	free(l);
	free(s); free(sv);
	gbah_destroy(R);
}

/* ------------------------------------------------------------------ scenario C (conflict, PC idle, phone wins) */

static void scenario_c(const char *data)
{
	Pc pc;
	Phone ph;
	Msg m;
	uint16_t port = free_port();
	const char *extra[] = { NULL };
	gbah_core *R = new_core();
	void *s = NULL, *sv = NULL;
	size_t sn, vn;
	char games[400];
	int round;
	gbah_meta pcm, after;
	char *l;

	snprintf(games, sizeof games, "%s/games", data);
	phone_reset(&ph, 0x5A);
	store_meta(data, &pcm);
	REQUIRE(pcm.has_data);
	CHECK(count_backups(games) == 0);
	REQUIRE(pc_start(&pc, "c", data, port, extra) == 0);   /* menu: no game running */
	REQUIRE(phone_connect(&ph, port, gbah_core_version()) == 0);
	{ int i; gbah_set_input(R, 0x40); for (i = 0; i < 80; i++) gbah_run_frame(R); }
	core_snapshot(R, &s, &sn, &sv, &vn);
	REQUIRE(wait_type(&ph, GBAHP_MSG_LIBRARY, &m, 5000));
	msg_free(&m);
	ph.meta.has_data = 1;
	memcpy(ph.meta.version.dev, ph.dev, 16); ph.meta.version.counter = 12;
	memcpy(ph.meta.base.dev, ph.dev, 16); ph.meta.base.counter = 2;
	ph.meta.timestamp_ms = gbah_now_ms(); ph.meta.frame = gbah_frame_counter(R);
	send_library(&ph, &ph.meta, g_rom_len);
	REQUIRE(wait_type(&ph, GBAHP_MSG_THUMB, &m, 5000));   /* conflict reported with the PC's picture */
	msg_free(&m);

	/* four times the phone's version wins: each time the PC's loser becomes a backup; only 3 are kept */
	for (round = 0; round < 4; round++) {
		gbah_ver v;
		ph.meta.version.counter = 12 + (uint64_t)round;
		ph.meta.base.counter = 2;
		send_push(&ph, &ph.meta, GBAHP_PUSH_RESOLVED, s, sn, sv, vn, NULL, 0);
		REQUIRE(wait_type(&ph, GBAHP_MSG_STATE_ACK, &m, 5000));
		{ gbahp_state_ack a; CHECK(gbahp_decode_state_ack(m.p, m.len, &a) == 0 && a.status == GBAHP_STATE_ACK_OK); }
		msg_free(&m);
		v = ph.meta.version;
		(void)v;
		CHECK(count_backups(games) == (round + 1 < 3 ? round + 1 : 3));
		usleep(20000);
	}
	store_meta(data, &after);
	CHECK(after.has_data && !memcmp(after.version.dev, ph.dev, 16) && after.version.counter == 15 && gbah_ver_eq(&after.base, &after.version));
	{   /* the stored state is the phone's: loads and matches */
		void *st = NULL, *sav = NULL;
		size_t stn = 0, savn = 0;
		CHECK(gbah_store_read(games, g_sha, &st, &stn, &sav, &savn) == 0 && stn == sn && !memcmp(st, s, sn));
		gbah_fs_free(st); gbah_fs_free(sav);
	}
	phone_close(&ph);
	CHECK(pc_stop(&pc) == 0);
	l = slurp(pc.log);
	if (failures) fprintf(stderr, "--- C: desktop log ---\n%s", l);
	free(l);
	free(s); free(sv);
	gbah_destroy(R);
}

/* ------------------------------------------------------------------ scenario D (cable pulled) */

static void scenario_d(const char *data)
{
	Pc pc;
	Phone ph;
	Msg m;
	uint16_t port = free_port();
	const char *extra[] = { "--mirror-ms", "600000", "--sav-min-ms", "600000", NULL, NULL };
	gbah_meta pcm, mm;
	gbahp_state_push sp;
	gbah_ver handoff_ver;
	char *l;

	extra[4] = g_rom;
	phone_reset(&ph, 0x5A);
	store_meta(data, &pcm);
	REQUIRE(pcm.has_data && pcm.version.counter == 15);
	REQUIRE(pc_start(&pc, "d", data, port, extra) == 0);   /* plays the stored game */
	REQUIRE(phone_connect(&ph, port, gbah_core_version()) == 0);
	ph.meta = pcm;   /* the phone holds what was synced in C (phone,15) */
	REQUIRE(wait_type(&ph, GBAHP_MSG_LIBRARY, &m, 5000));
	msg_free(&m);
	send_library(&ph, &ph.meta, g_rom_len);
	/* the PC played since: it is ahead of the phone -> auto push, the phone adopts it */
	REQUIRE(wait_push_flag(&ph, GBAHP_PUSH_MIRROR, &m, 5000));
	REQUIRE(gbahp_decode_state_push(m.p, m.len, &sp) == 0);
	CHECK(!memcmp(sp.origin_device, g_pc_dev, 16) && sp.base == 15 && !memcmp(sp.base_device, ph.dev, 16));
	phone_handle_mirror(&ph, &m);
	msg_free(&m);
	CHECK(ph.mirrors == 1);
	handoff_ver = ph.meta.version;   /* last version both sides share (the ACKed mirror) */
	usleep(800000);

	/* the cable is pulled (socket closed without BYE): the PC keeps playing */
	gbahp_net_shutdown(ph.c.s);
	gbahp_conn_close(&ph.c);
	usleep(1500000);
	CHECK(pc_alive(&pc));
	CHECK(log_has(&pc, "Cabo desconectado: continuando so no PC"));

	/* the cable is back: the PC advanced from the handoff version -> it pushes by itself */
	REQUIRE(phone_connect(&ph, port, gbah_core_version()) == 0);
	REQUIRE(wait_type(&ph, GBAHP_MSG_LIBRARY, &m, 5000));
	{
		gbahp_library lib; gbahp_lib_entry e;
		CHECK(gbahp_decode_library(m.p, m.len, &lib) == 0 && lib.count == 1);
		gbahp_library_get(&lib, 0, &e);
		CHECK(e.has_data && !memcmp(e.version_dev, g_pc_dev, 16) && e.version > handoff_ver.counter);
		CHECK(e.base == handoff_ver.counter && !memcmp(e.base_dev, handoff_ver.dev, 16));
	}
	msg_free(&m);
	send_library(&ph, &ph.meta, g_rom_len);
	REQUIRE(wait_push_flag(&ph, GBAHP_PUSH_MIRROR, &m, 5000));
	REQUIRE(gbahp_decode_state_push(m.p, m.len, &sp) == 0);
	CHECK(!memcmp(sp.origin_device, g_pc_dev, 16) && sp.base == handoff_ver.counter && !memcmp(sp.base_device, handoff_ver.dev, 16));
	phone_handle_mirror(&ph, &m);
	msg_free(&m);
	CHECK(ph.mirrors == 2);
	phone_close(&ph);
	CHECK(pc_stop(&pc) == 0);   /* final checkpoint */
	store_meta(data, &mm);
	CHECK(mm.has_data && !memcmp(mm.version.dev, g_pc_dev, 16) && mm.version.counter > handoff_ver.counter);
	l = slurp(pc.log);
	if (failures) fprintf(stderr, "--- D: desktop log ---\n%s", l);
	free(l);
}

/* ------------------------------------------------------------------ scenario E (phone without ROM, .sav trigger) */

static void scenario_e(const char *data)
{
	Pc pc;
	Phone ph;
	Msg m;
	uint16_t port = free_port();
	const char *extra[] = { "--mirror-ms", "600000", "--sav-min-ms", "500", NULL, NULL };
	gbahp_state_push sp;
	uint64_t v1;
	size_t got = 0;
	uint8_t *rom = NULL;
	uint8_t sha[20];
	char *l;

	extra[4] = g_rom;
	phone_reset(&ph, 0x6B);
	REQUIRE(pc_start(&pc, "e", data, port, extra) == 0);
	REQUIRE(phone_connect(&ph, port, gbah_core_version()) == 0);
	send_library(&ph, NULL, 0);   /* the phone has nothing */

	/* the PC mirrors the game it plays even though the phone does not list it; the phone lacks the ROM */
	REQUIRE(wait_push_flag(&ph, GBAHP_PUSH_MIRROR, &m, 8000));
	REQUIRE(gbahp_decode_state_push(m.p, m.len, &sp) == 0);
	v1 = sp.version;
	{ gbah_ver v; memcpy(v.dev, sp.origin_device, 16); v.counter = sp.version; send_ack(&ph, g_sha, &v, GBAHP_STATE_ACK_NO_ROM); }
	msg_free(&m);
	/* ... so the PC streams the ROM, then pushes again (same version) */
	for (;;) {
		gbahp_rom_push rp;
		if (!wait_type(&ph, GBAHP_MSG_ROM_PUSH, &m, 5000)) break;
		if (gbahp_decode_rom_push(m.p, m.len, &rp) != 0) { msg_free(&m); break; }
		if (!rom) rom = (uint8_t *)malloc((size_t)rp.total_size);
		CHECK(rp.offset == got);
		memcpy(rom + got, rp.data, rp.data_len);
		got += rp.data_len;
		if (got == rp.total_size) { msg_free(&m); break; }
		msg_free(&m);
	}
	CHECK(got == g_rom_len && rom);
	if (rom) { gbah_sha1(rom, got, sha); CHECK(!memcmp(sha, g_sha, 20) && !memcmp(rom, g_rom_bytes, got)); }
	free(rom);
	REQUIRE(wait_push_flag(&ph, GBAHP_PUSH_MIRROR, &m, 5000));
	REQUIRE(gbahp_decode_state_push(m.p, m.len, &sp) == 0);
	CHECK(sp.version == v1);
	{ gbah_ver v; memcpy(v.dev, sp.origin_device, 16); v.counter = sp.version; send_ack(&ph, g_sha, &v, GBAHP_STATE_ACK_OK);
	  memcpy(ph.meta.version.dev, v.dev, 16); ph.meta.version.counter = v.counter; ph.meta.base = ph.meta.version; ph.meta.has_data = 1; }
	msg_free(&m);

	/* the test ROM writes its SRAM every frame: the "save changed" trigger (period is 10 min) must send another one soon */
	REQUIRE(wait_push_flag(&ph, GBAHP_PUSH_MIRROR, &m, 8000));
	REQUIRE(gbahp_decode_state_push(m.p, m.len, &sp) == 0);
	CHECK(sp.version > v1);
	msg_free(&m);
	phone_close(&ph);
	CHECK(pc_stop(&pc) == 0);
	l = slurp(pc.log);
	if (failures) fprintf(stderr, "--- E: desktop log ---\n%s", l);
	free(l);
}

/* ------------------------------------------------------------------ scenario G (keys, one owner) */

static void scenario_g(const char *data)
{
	Pc pc;
	Phone ph;
	Msg m;
	uint16_t port = free_port();
	const char *extra[] = { "--mirror-ms", "700", "--sav-min-ms", "600000", NULL, NULL };
	uint32_t frame_resolved = 0;
	char *l;

	extra[4] = g_rom;
	phone_reset(&ph, 0x7C);
	REQUIRE(pc_start(&pc, "g", data, port, extra) == 0);   /* plays the stored game */
	REQUIRE(phone_connect(&ph, port, gbah_core_version()) == 0);
	REQUIRE(wait_type(&ph, GBAHP_MSG_LIBRARY, &m, 5000));
	msg_free(&m);
	send_library(&ph, NULL, 0);
	/* CONTROL: with no key pressed the test ROM reports 0/0 in SRAM[1..2] */
	CHECK(wait_keys_in_mirror(&ph, 0x00, 0x00, 8000));
	/* the phone presses A + L + R (bits 0, 8, 9): they reach the PC (L/R too) */
	send_input(&ph, 0x301);
	CHECK(wait_keys_in_mirror(&ph, 0x01, 0x03, 8000));
	/* conflict while the key is still "held": the PC pauses and forgets the phone's keys */
	REQUIRE(conflict_fetch_pc(&ph, &frame_resolved) == 0);
	CHECK(pushes_within(&ph, 1800) == 0);
	CHECK(log_has(&pc, "Conflict -> AwaitPlace"));
	/* the phone chooses the PC: it runs again from the stored version; the stale key must be gone */
	send_control(&ph, GBAHP_CTL_PLACE_PC, g_sha, 0);
	CHECK(wait_keys_in_mirror(&ph, 0x00, 0x00, 8000));
	/* and the measurement can see keys again when they ARE pressed (the check above is not vacuous) */
	send_input(&ph, 0x201);
	CHECK(wait_keys_in_mirror(&ph, 0x01, 0x02, 8000));
	send_input(&ph, 0);
	CHECK(wait_keys_in_mirror(&ph, 0x00, 0x00, 8000));
	phone_close(&ph);
	CHECK(pc_stop(&pc) == 0);
	l = slurp(pc.log);
	if (failures) fprintf(stderr, "--- G: desktop log ---\n%s", l);
	free(l);
}

/* ------------------------------------------------------------------ scenario H (PLACE_PHONE, cable lost while paused) */

static void scenario_h(const char *data)
{
	Pc pc;
	Phone ph;
	Msg m;
	uint16_t port = free_port();
	const char *extra[] = { "--mirror-ms", "700", "--sav-min-ms", "600000", NULL, NULL };
	uint32_t frame_resolved = 0;
	char *l;

	extra[4] = g_rom;
	/* 1: PLACE_PHONE -> the PC keeps paused on the game ("Jogando no celular") */
	phone_reset(&ph, 0x7D);
	REQUIRE(pc_start(&pc, "h1", data, port, extra) == 0);
	REQUIRE(phone_connect(&ph, port, gbah_core_version()) == 0);
	REQUIRE(wait_type(&ph, GBAHP_MSG_LIBRARY, &m, 5000));
	msg_free(&m);
	send_library(&ph, NULL, 0);
	REQUIRE(wait_push_flag(&ph, GBAHP_PUSH_MIRROR, &m, 8000));   /* it plays and mirrors (control) */
	phone_handle_mirror(&ph, &m);
	msg_free(&m);
	REQUIRE(conflict_fetch_pc(&ph, &frame_resolved) == 0);
	send_control(&ph, GBAHP_CTL_PLACE_PHONE, g_sha, 0);
	CHECK(pushes_within(&ph, 2500) == 0);
	CHECK(log_has(&pc, "AwaitPlace -> WaitPhone"));
	CHECK(!log_has(&pc, "AwaitPlace -> Playing") && !log_has(&pc, "AwaitPlace -> Menu"));
	phone_close(&ph);
	CHECK(pc_stop(&pc) == 0);
	l = slurp(pc.log);
	if (failures) fprintf(stderr, "--- H1: desktop log ---\n%s", l);
	free(l);
	kill_all_pcs();

	/* 2: the cable is lost while the PC waits for the answer: it must NOT start playing alone */
	port = free_port();
	phone_reset(&ph, 0x7E);
	REQUIRE(pc_start(&pc, "h2", data, port, extra) == 0);
	REQUIRE(phone_connect(&ph, port, gbah_core_version()) == 0);
	REQUIRE(wait_type(&ph, GBAHP_MSG_LIBRARY, &m, 5000));
	msg_free(&m);
	send_library(&ph, NULL, 0);
	REQUIRE(wait_push_flag(&ph, GBAHP_PUSH_MIRROR, &m, 8000));
	phone_handle_mirror(&ph, &m);
	msg_free(&m);
	REQUIRE(conflict_fetch_pc(&ph, &frame_resolved) == 0);
	usleep(300000);
	CHECK(log_has(&pc, "Conflict -> AwaitPlace"));
	gbahp_net_shutdown(ph.c.s);
	gbahp_conn_close(&ph.c);
	usleep(2000000);
	CHECK(pc_alive(&pc));
	CHECK(log_has(&pc, "Cabo desconectado"));
	CHECK(!log_has(&pc, "AwaitPlace -> Playing") && !log_has(&pc, "AwaitPlace -> Menu"));   /* still paused on the game */
	CHECK(pc_stop(&pc) == 0);
	l = slurp(pc.log);
	if (failures) fprintf(stderr, "--- H2: desktop log ---\n%s", l);
	free(l);
}

/* ------------------------------------------------------------------ scenario I (settings) */

static int wait_settings(Phone *ph, uint32_t *bits, int timeout_ms)
{
	uint64_t end = mono_ms() + (uint64_t)timeout_ms;
	while (mono_ms() < end) {
		Msg m;
		gbahp_control c;
		if (!wait_type(ph, GBAHP_MSG_CONTROL, &m, (int)(end - mono_ms()))) return 0;
		if (gbahp_decode_control(m.p, m.len, &c) == 0 && c.cmd == GBAHP_CTL_SETTINGS) { *bits = c.arg; msg_free(&m); return 1; }
		msg_free(&m);
	}
	return 0;
}

static void scenario_i(const char *data)
{
	Pc pc;
	Phone ph;
	uint16_t port = free_port();
	const char *extra[] = { NULL };
	uint32_t bits = 99;
	char ini[400];
	char *l;

	snprintf(ini, sizeof ini, "%s/settings.ini", data);
	phone_reset(&ph, 0x7F);
	CHECK(!file_exists(ini));
	REQUIRE(pc_start(&pc, "i1", data, port, extra) == 0);
	REQUIRE(phone_connect(&ph, port, gbah_core_version()) == 0);
	REQUIRE(wait_settings(&ph, &bits, 5000));
	CHECK(bits == 0);                                              /* default: ask on conflict */
	send_control(&ph, GBAHP_CTL_SETTINGS, g_sha, GBAHP_SET_PREFER_NEWEST);   /* the phone says: "newest" */
	usleep(500000);
	CHECK(log_has(&pc, "Configuracoes diferentes"));
	CHECK(!file_exists(ini));                                      /* an announcement alone changes nothing */
	send_control(&ph, GBAHP_CTL_SETTINGS_SET, g_sha, GBAHP_SET_PREFER_NEWEST);   /* the user chose "newest" for both */
	usleep(500000);
	CHECK(file_exists(ini));
	phone_close(&ph);
	CHECK(pc_stop(&pc) == 0);
	l = slurp(pc.log);
	if (failures) fprintf(stderr, "--- I1: desktop log ---\n%s", l);
	free(l);
	kill_all_pcs();

	/* after a restart the PC announces the saved choice */
	port = free_port();
	phone_reset(&ph, 0x7F);
	REQUIRE(pc_start(&pc, "i2", data, port, extra) == 0);
	REQUIRE(phone_connect(&ph, port, gbah_core_version()) == 0);
	bits = 99;
	REQUIRE(wait_settings(&ph, &bits, 5000));
	CHECK(bits == GBAHP_SET_PREFER_NEWEST);
	/* ... and back to "ask" persists too */
	send_control(&ph, GBAHP_CTL_SETTINGS_SET, g_sha, 0);
	usleep(500000);
	phone_close(&ph);
	CHECK(pc_stop(&pc) == 0);
	kill_all_pcs();
	port = free_port();
	phone_reset(&ph, 0x7F);
	REQUIRE(pc_start(&pc, "i3", data, port, extra) == 0);
	REQUIRE(phone_connect(&ph, port, gbah_core_version()) == 0);
	bits = 99;
	REQUIRE(wait_settings(&ph, &bits, 5000));
	CHECK(bits == 0);
	phone_close(&ph);
	CHECK(pc_stop(&pc) == 0);
}

/* ------------------------------------------------------------------ scenario F (second instance) */

static void scenario_f(const char *data1, const char *data2)
{
	Pc a, b;
	uint16_t port = free_port();
	const char *e1[] = { NULL };
	const char *e2[] = { "--frames", "20", NULL };
	gbahp_sock probe;
	REQUIRE(pc_start(&a, "f1", data1, port, e1) == 0);
	{   /* wait until the first instance listens */
		Phone ph;
		phone_reset(&ph, 0x01);
		REQUIRE(phone_connect(&ph, port, gbah_core_version()) == 0);
		phone_close(&ph);
	}
	/* the primitive itself: a second listener on the same port must fail */
	probe = gbahp_net_listen_loopback(port);
	CHECK(probe == GBAHP_BAD_SOCK);
	gbahp_net_close(probe);
	/* the second instance (ROM given so it plays and exits after 20 frames) reports it and still works */
	{
		const char *e3[] = { "--frames", "20", g_rom, NULL };
		(void)e2;
		REQUIRE(pc_start(&b, "f2", data2, port, e3) == 0);
	}
	{
		int st = 0, i;
		for (i = 0; i < 100; i++) { if (waitpid(b.pid, &st, WNOHANG) == b.pid) { reg_del(b.pid); break; } usleep(100000); }
		CHECK(i < 100 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
	}
	CHECK(log_has(&b, "PORT_BUSY"));
	CHECK(!log_has(&a, "PORT_BUSY"));
	CHECK(pc_stop(&a) == 0);
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
	char data[300], data_f[300];
	int i;
	if (argc < 5) { fprintf(stderr, "usage: %s gbahandoff rom sha1hex fake_adb\n", argv[0]); return 2; }
	g_exe = argv[1]; g_rom = argv[2]; g_sha_hex = argv[3]; g_fake_adb = argv[4];
	for (i = 0; i < 20; i++) g_sha[i] = (uint8_t)(hexval(g_sha_hex[2 * i]) * 16 + hexval(g_sha_hex[2 * i + 1]));
	{
		void *b = NULL;
		if (gbah_read_file(g_rom, &b, &g_rom_len) != 0) { fprintf(stderr, "cannot read the ROM\n"); return 2; }
		g_rom_bytes = (uint8_t *)b;
	}
	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_signal); signal(SIGINT, on_signal); signal(SIGALRM, on_signal);
	atexit(kill_all_pcs);
	alarm(170);   /* below the ctest timeout (180): we clean up ourselves */
	snprintf(g_base, sizeof g_base, "/tmp/gbah_e2e_XXXXXX");
	if (!mkdtemp(g_base)) { perror("mkdtemp"); return 2; }
	snprintf(data, sizeof data, "%s/data", g_base);
	snprintf(data_f, sizeof data_f, "%s/data_f", g_base);
	setenv("SDL_VIDEO_DRIVER", "dummy", 1);   /* no display in CI / WSL */
	setenv("SDL_AUDIO_DRIVER", "dummy", 1);
	setenv("XDG_DATA_HOME", g_base, 1);

	scenario_a(data); kill_all_pcs();
	if (!failures) { scenario_b(data); kill_all_pcs(); }
	if (!failures) { scenario_c(data); kill_all_pcs(); }
	if (!failures) { scenario_d(data); kill_all_pcs(); }
	if (!failures) { scenario_g(data); kill_all_pcs(); }
	if (!failures) { scenario_h(data); kill_all_pcs(); }
	{
		char d3[320];
		snprintf(d3, sizeof d3, "%s/data_i", g_base);
		scenario_i(d3); kill_all_pcs();
	}
	scenario_e(data_f); kill_all_pcs();
	{
		char d1[320], d2[320];
		snprintf(d1, sizeof d1, "%s/f1", g_base);
		snprintf(d2, sizeof d2, "%s/f2", g_base);
		scenario_f(d1, d2);
		kill_all_pcs();
	}
	free(g_rom_bytes);
	if (failures) { fprintf(stderr, "(logs kept in %s)\n", g_base); return 1; }
	rmrf(g_base);
	puts("handoff_e2e_test: ok");
	return 0;
}
