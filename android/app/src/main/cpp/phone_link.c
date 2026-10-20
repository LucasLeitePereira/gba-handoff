/* See phone_link.h. */
#define _GNU_SOURCE
#include "phone_link.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "gbah_core.h"
#include "gbah_sync.h"
#include "gbahp_net.h"

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "gbah-phone", __VA_ARGS__)
#else
#define LOGI(...) do { if (getenv("PHONE_LINK_LOG")) { fprintf(stderr, "[phone] " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#endif

#define MAX_GAMES 128
#define MAX_CONF 8
#define MAX_NOTICES 8
#define MAX_HELD 4
#define MAX_INFLIGHT 16
#define MAX_CMDS 8
#define RETRY_MIN_MS 2000
#define RETRY_MAX_MS 10000
#define PING_EVERY_MS 5000
#define IDLE_TIMEOUT_MS 15000
#define LIB_WAIT_STATUS_MS 1000
#define MAX_ROM_BYTES (33u << 20)

enum { K_SYNC = 0, K_HANDOFF, K_RESOLVED, K_MIRROR };
enum { CMD_RETURN = 1, CMD_RESOLVE, CMD_FETCH, CMD_HANDOFF, CMD_PLACE, CMD_SETTINGS_SET, CMD_SETTINGS_ANNOUNCE };
#define PLACE_WAIT_MS 6000    /* the PC must confirm PLACE_PC with PC_STATUS within this */
#define LEAVE_WAIT_MS 5000    /* reconnect: wait this long for the PC's newer copy before resuming */
#define INPUT_RESEND_MS 100   /* re-send period while a key is held (recovers loss across a reconnect) */
#define FAST_POLL_MS 8        /* link thread poll while the game is on the PC (input latency) */

uint64_t phone_hash(const void *d, size_t n)
{
	const uint8_t *p = (const uint8_t *)d;
	uint64_t h = 1469598103934665603ULL;
	size_t i;
	for (i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ULL;
	return h;
}

void phone_snap_free(phone_snap *s)
{
	free(s->state); free(s->sav); free(s->thumb);
	memset(s, 0, sizeof *s);
}

typedef struct {
	uint8_t sha1[20];
	gbah_meta local, remote;
	int have_remote;
	uint8_t *rthumb;
	int waiting_pc;       /* user chose the PC's version: FETCH sent, waiting for the RESOLVED push */
} conflict_t;

typedef struct { uint8_t sha1[20]; gbah_ver ver; int kind; uint64_t t_ms; } inflight_t;

/* "Continue on: phone | PC" question after a conflict was resolved: the game is held (on both sides)
 * until the user answers. */
typedef struct { uint8_t sha1[20]; int waiting; uint64_t t_ms; } place_t;

typedef struct {
	uint8_t sha1[20];
	gbah_ver ver, base;
	uint8_t flags;
	uint64_t ts;
	uint32_t frame;
	uint8_t *state; size_t sn;
	uint8_t *sav; size_t vn;
	uint8_t *thumb; size_t tn;
} pushcopy;

typedef struct { int type; uint8_t sha1[20]; int arg; } cmd_t;

struct phone_link {
	phone_cfg cfg;
	char games_dir[512], roms_dir[512], name[128], core_ver[64];
	pthread_mutex_t mtx, store_mtx, hold_mtx;
	pthread_cond_t cv;
	pthread_t link_th, save_th;
	int link_valid, save_valid;
	atomic_int run_link, run_save, status, running, mirror_req;
	gbahp_sock sock;                    /* mtx */
	char peer[96];                      /* mtx */
	uint8_t peer_dev[16];               /* mtx */
	uint16_t port;

	phone_rom roms[MAX_GAMES];          /* mtx */
	int nroms;

	/* UI-visible state (mtx) */
	int on_pc_has; uint8_t on_pc_sha[20];
	int handoff_pending; uint8_t handoff_sha[20];
	int pc_playing; uint8_t pc_sha[20];
	conflict_t conf[MAX_CONF]; int nconf;
	place_t place[MAX_CONF]; int nplace;
	atomic_int prefer_newest;           /* setting "always keep the newest version" */
	int pc_pref_known, pc_pref;         /* the PC's setting, this session (mtx) */
	atomic_int control_on;              /* control mode: INPUT goes to the PC only then */
	atomic_uint latest_keys; atomic_int keys_dirty;
	int refuse_status; unsigned refuse_proto;   /* last HELLO refusal (mtx) */
	char notices[MAX_NOTICES][200]; int nnotice;
	int open_has; uint8_t open_sha[20];
	int rx_active, rx_pct; uint8_t rx_sha[20];
	int held;                           /* what the host was last asked (hold_mtx) */

	/* autosave bookkeeping (mtx) */
	int last_valid; uint8_t last_sha[20]; uint32_t last_frame;
	uint64_t last_sav_hash;
	uint64_t last_save_mono;

	cmd_t cmds[MAX_CMDS]; int ncmd;     /* mtx */
};

/* ------------------------------------------------------------------ small helpers */

static uint64_t mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* waits up to ms on p->cv (woken by stop or by a command) */
static void nap(phone_link *p, atomic_int *flag, int ms)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
	pthread_mutex_lock(&p->mtx);
	if (atomic_load(flag)) pthread_cond_timedwait(&p->cv, &p->mtx, &ts);
	pthread_mutex_unlock(&p->mtx);
}

static void notice(phone_link *p, const char *fmt, ...)
{
	va_list ap;
	char tmp[200];
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	LOGI("notice: %s", tmp);
	pthread_mutex_lock(&p->mtx);
	if (p->nnotice == MAX_NOTICES) {
		memmove(p->notices[0], p->notices[1], sizeof p->notices[0] * (MAX_NOTICES - 1));
		p->nnotice--;
	}
	memcpy(p->notices[p->nnotice++], tmp, sizeof tmp);
	pthread_mutex_unlock(&p->mtx);
}

static const phone_host *H(phone_link *p) { return &p->cfg.host; }

static int loaded_sha(phone_link *p, uint8_t sha[20])
{
	return H(p)->loaded(H(p)->user, sha);
}

static void fmt_time(uint64_t ms, char *out, size_t cap)
{
	time_t t = (time_t)(ms / 1000);
	struct tm tmv;
	if (ms == 0 || !localtime_r(&t, &tmv)) { snprintf(out, cap, "?"); return; }
	strftime(out, cap, "%d/%m %H:%M", &tmv);
}

/* ------------------------------------------------------------------ hold (freeze the emulation) */

/* The emulation must stand still while this phone's copy of a game is not the one in play:
 * waiting for the PC's answer to a handoff, the game is on the PC, or a conflict is open. */
static void update_hold(phone_link *p)
{
	uint8_t cur[20];
	int want = 0, has = loaded_sha(p, cur), i;
	pthread_mutex_lock(&p->hold_mtx);
	pthread_mutex_lock(&p->mtx);
	if (has) {
		if (p->handoff_pending && !memcmp(cur, p->handoff_sha, 20)) want = 1;
		if (p->on_pc_has && !memcmp(cur, p->on_pc_sha, 20)) want = 1;
		for (i = 0; i < p->nconf; i++) if (!memcmp(cur, p->conf[i].sha1, 20)) want = 1;
		for (i = 0; i < p->nplace; i++) if (!memcmp(cur, p->place[i].sha1, 20)) want = 1;
	}
	pthread_mutex_unlock(&p->mtx);
	if (want != p->held) {
		p->held = want;
		H(p)->hold(H(p)->user, want);
		LOGI("hold %d", want);
	}
	pthread_mutex_unlock(&p->hold_mtx);
}

/* ------------------------------------------------------------------ store access (store_mtx) */

static int store_meta_locked(phone_link *p, const uint8_t sha[20], gbah_meta *m, uint8_t **thumb, uint32_t *tl)
{
	int rc;
	pthread_mutex_lock(&p->store_mtx);
	rc = gbah_store_meta(p->games_dir, sha, p->cfg.device_id, m, thumb, tl);
	pthread_mutex_unlock(&p->store_mtx);
	return rc;
}

static gbah_meta meta_of_push(const pushcopy *pc)
{
	gbah_meta m;
	memset(&m, 0, sizeof m);
	memcpy(m.sha1, pc->sha1, 20);
	m.has_data = 1;
	m.version = pc->ver;
	m.base = pc->base;
	m.timestamp_ms = pc->ts;
	m.frame = pc->frame;
	return m;
}

static gbah_meta meta_of_entry(const gbahp_lib_entry *e)
{
	gbah_meta r;
	memset(&r, 0, sizeof r);
	memcpy(r.sha1, e->sha1, 20);
	r.has_data = e->has_data;
	if (e->has_data) {
		memcpy(r.version.dev, e->version_dev, 16); r.version.counter = e->version;
		memcpy(r.base.dev, e->base_dev, 16);       r.base.counter = e->base;
		r.timestamp_ms = e->timestamp_ms;
		r.frame = e->frame;
	}
	return r;
}

/* ------------------------------------------------------------------ checkpoint (autosave / onPause / pre-sync) */

static void remember_saved(phone_link *p, const uint8_t sha[20], uint32_t frame, uint64_t sav_hash)
{
	pthread_mutex_lock(&p->mtx);
	memcpy(p->last_sha, sha, 20);
	p->last_valid = 1;
	p->last_frame = frame;
	p->last_sav_hash = sav_hash;
	p->last_save_mono = mono_ms();
	pthread_mutex_unlock(&p->mtx);
}

/* Saves the loaded game if it has progress the store does not have. 0 saved, 1 skipped, <0 error.
 * ignore_hold: the caller froze the game itself (handoff) and needs the store to match it. */
static int checkpoint(phone_link *p, int ignore_hold, gbah_meta *out)
{
	uint8_t cur[20];
	phone_snap s;
	gbah_meta m;
	uint32_t fr;
	int rc, same;
	if (!loaded_sha(p, cur)) return 1;
	if (!ignore_hold) {
		int blocked;
		pthread_mutex_lock(&p->mtx);
		blocked = (p->on_pc_has && !memcmp(cur, p->on_pc_sha, 20)) ||
		          (p->handoff_pending && !memcmp(cur, p->handoff_sha, 20));
		pthread_mutex_unlock(&p->mtx);
		if (blocked) return 1;
	}
	fr = H(p)->frame(H(p)->user);
	pthread_mutex_lock(&p->mtx);
	same = p->last_valid && !memcmp(p->last_sha, cur, 20) && p->last_frame == fr;
	pthread_mutex_unlock(&p->mtx);
	if (same) {
		if (out) store_meta_locked(p, cur, out, NULL, NULL);
		return 1;
	}
	memset(&s, 0, sizeof s);
	if (H(p)->snapshot(H(p)->user, &s) != 0 || !s.state_n) { phone_snap_free(&s); return -1; }
	pthread_mutex_lock(&p->store_mtx);
	gbah_store_meta(p->games_dir, s.sha1, p->cfg.device_id, &m, NULL, NULL);
	gbah_sync_advance(&m, p->cfg.device_id, gbah_now_ms(), s.frame);
	rc = gbah_store_commit(p->games_dir, &m, s.state, s.state_n, s.sav, s.sav_n, s.thumb, s.thumb ? PHONE_THUMB_BYTES : 0);
	pthread_mutex_unlock(&p->store_mtx);
	if (rc == 0) {
		remember_saved(p, s.sha1, s.frame, phone_hash(s.sav, s.sav_n));
		atomic_store(&p->mirror_req, 1);
		if (out) *out = m;
		LOGI("saved v%llu frame %u", (unsigned long long)m.version.counter, s.frame);
	} else LOGI("COMMIT FAILED");
	phone_snap_free(&s);
	return rc == 0 ? 0 : -1;
}

int phone_save_now(phone_link *p) { return checkpoint(p, 0, NULL); }

/* ------------------------------------------------------------------ autosave thread */

static void *save_main(void *arg)
{
	phone_link *p = (phone_link *)arg;
	unsigned tick = p->cfg.tick_ms ? p->cfg.tick_ms : 1000;
	unsigned full = p->cfg.autosave_ms ? p->cfg.autosave_ms : 30000;
	unsigned min_gap = p->cfg.sav_min_ms ? p->cfg.sav_min_ms : 5000;
	uint64_t next_full = mono_ms() + full, seen_at = 0, first_change = 0;
	uint64_t seen_hash = 0;
	int changing = 0;
	while (atomic_load(&p->run_save)) {
		uint8_t cur[20];
		uint64_t now, h, disk_hash, last_save;
		int due = 0, held;
		nap(p, &p->run_save, (int)tick);
		if (!atomic_load(&p->run_save)) break;
		now = mono_ms();
		if (!atomic_load(&p->running) || !loaded_sha(p, cur)) { next_full = now + full; changing = 0; continue; }
		pthread_mutex_lock(&p->hold_mtx);
		held = p->held;
		pthread_mutex_unlock(&p->hold_mtx);
		if (held) { next_full = now + full; changing = 0; continue; }

		/* did the game write its cartridge save? (cheap: one hash of <= 128 KB) */
		h = H(p)->sav_hash(H(p)->user);
		pthread_mutex_lock(&p->mtx);
		disk_hash = p->last_sav_hash;
		last_save = p->last_save_mono;
		pthread_mutex_unlock(&p->mtx);
		if (h != disk_hash) {
			if (!changing) { changing = 1; first_change = now; seen_hash = h; seen_at = now; }
			else if (h != seen_hash) { seen_hash = h; seen_at = now; }
			/* wait until the game stops writing for a moment, but never more than 10 s */
			if ((now - seen_at >= min_gap / 2 || now - first_change >= 10000) && now - last_save >= min_gap) due = 1;
		} else changing = 0;
		if ((int64_t)(now - next_full) >= 0) due = 1;
		if (due) {
			checkpoint(p, 0, NULL);
			next_full = mono_ms() + full;
			changing = 0;
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------ ROM table */

static void hex_of(const uint8_t sha[20], char hex[41]) { gbah_sha1_hex(sha, hex); }

static void rom_file_path(phone_link *p, const uint8_t sha[20], char *out, size_t cap)
{
	char hex[41];
	hex_of(sha, hex);
	snprintf(out, cap, "%s/%s.gba", p->roms_dir, hex);
}

/* A source we can try: the received file, or the table entry. */
static int rom_source(phone_link *p, const uint8_t sha[20], char *out, size_t cap)
{
	int i, found = 0;
	rom_file_path(p, sha, out, cap);
	if (gbah_fs_exists(out)) return 1;
	pthread_mutex_lock(&p->mtx);
	for (i = 0; i < p->nroms; i++)
		if (!memcmp(p->roms[i].sha1, sha, 20)) { snprintf(out, cap, "%s", p->roms[i].source); found = 1; break; }
	pthread_mutex_unlock(&p->mtx);
	return found;
}

static int open_rom_fd(phone_link *p, const uint8_t sha[20])
{
	char src[600];
	if (!rom_source(p, sha, src, sizeof src)) return -1;
	if (!strncmp(src, "content://", 10)) return H(p)->open_rom ? H(p)->open_rom(H(p)->user, src) : -1;
	return open(!strncmp(src, "file://", 7) ? src + 7 : src, O_RDONLY | O_CLOEXEC);
}

void phone_set_roms(phone_link *p, const phone_rom *roms, int n)
{
	pthread_mutex_lock(&p->mtx);
	if (n > MAX_GAMES) {
		LOGI("phone_set_roms: %d ROMs, only the first %d are used", n, MAX_GAMES);
		n = MAX_GAMES;
	}
	if (n < 0) n = 0;
	if (n) memcpy(p->roms, roms, sizeof *roms * (size_t)n);
	p->nroms = n;
	pthread_mutex_unlock(&p->mtx);
}

/* ------------------------------------------------------------------ catalog / LIBRARY */

typedef struct { uint8_t sha[MAX_GAMES][20]; int n; } shalist;

static void sl_add(shalist *l, const uint8_t sha[20])
{
	int i;
	for (i = 0; i < l->n; i++) if (!memcmp(l->sha[i], sha, 20)) return;
	if (l->n < MAX_GAMES) memcpy(l->sha[l->n++], sha, 20);
}
static void sl_store_cb(const uint8_t sha[20], void *u) { sl_add((shalist *)u, sha); }
static void sl_dir_cb(const char *name, void *u)
{
	uint8_t s[20];
	if (strlen(name) == 44 && !strcmp(name + 40, ".gba") && gbah_sha1_from_hex(name, s) == 0) sl_add((shalist *)u, s);
}

static uint64_t rom_size_of(phone_link *p, const uint8_t sha[20])
{
	char path[600];
	uint64_t sz = 0;
	int i;
	rom_file_path(p, sha, path, sizeof path);
	if (gbah_fs_size(path, &sz) == 0) return sz;
	pthread_mutex_lock(&p->mtx);
	for (i = 0; i < p->nroms; i++) if (!memcmp(p->roms[i].sha1, sha, 20)) { sz = p->roms[i].size; break; }
	pthread_mutex_unlock(&p->mtx);
	return sz;
}

static void catalog(phone_link *p, shalist *l)
{
	int i;
	l->n = 0;
	pthread_mutex_lock(&p->store_mtx);
	gbah_store_list(p->games_dir, sl_store_cb, l);
	pthread_mutex_unlock(&p->store_mtx);
	pthread_mutex_lock(&p->mtx);
	for (i = 0; i < p->nroms; i++) sl_add(l, p->roms[i].sha1);
	pthread_mutex_unlock(&p->mtx);
	gbah_fs_list(p->roms_dir, sl_dir_cb, l);
}

/* ------------------------------------------------------------------ session */

typedef struct {
	phone_link *p;
	gbahp_conn c;
	gbahp_lib_entry plib[MAX_GAMES]; int nplib;
	int got_plib, got_status, lib_done;
	uint64_t plib_at;
	inflight_t inf[MAX_INFLIGHT]; int ninf;
	pushcopy held[MAX_HELD]; int nheld;
	uint8_t asked[MAX_HELD * 2][20]; int nasked;
	uint8_t rx_sha[20]; uint64_t rx_total; uint8_t *rx_buf; size_t rx_len; int rx_on;
	struct { uint8_t sha[20]; uint8_t *th; } tcache[4]; int tnext;
	uint32_t in_seq; uint16_t in_keys; uint64_t in_sent_ms;   /* last INPUT sent (link thread only) */
	int leave_pending;                         /* reconnect: the game was on the PC and the PC does not play it now */
	int leave_set; uint8_t leave_sha[20]; uint64_t leave_at;   /* ... and its newer copy is on the way */
} sess_t;

static int send_frame(sess_t *s, const void *buf, size_t n)
{
	return n && gbahp_conn_send(&s->c, buf, n) == 0 ? 0 : -1;
}

static void pc_free(pushcopy *pc)
{
	free(pc->state); free(pc->sav); free(pc->thumb);
	memset(pc, 0, sizeof *pc);
}

static void send_ack(sess_t *s, const uint8_t sha[20], const gbah_ver *v, uint8_t status)
{
	gbahp_state_ack a;
	uint8_t out[128];
	memset(&a, 0, sizeof a);
	memcpy(a.sha1_rom, sha, 20);
	a.version = v->counter;
	memcpy(a.origin_device, v->dev, 16);
	a.status = status;
	send_frame(s, out, gbahp_encode_state_ack(&a, out, sizeof out));
}

static void send_control(sess_t *s, uint8_t cmd, const uint8_t sha[20], uint32_t arg)
{
	gbahp_control c;
	uint8_t out[128];
	memset(&c, 0, sizeof c);
	c.cmd = cmd;
	if (sha) memcpy(c.sha1, sha, 20);
	c.arg = arg;
	send_frame(s, out, gbahp_encode_control(&c, out, sizeof out));
}

static void inflight_add(sess_t *s, const uint8_t sha[20], const gbah_ver *v, int kind)
{
	inflight_t *f;
	if (s->ninf == MAX_INFLIGHT) { memmove(&s->inf[0], &s->inf[1], sizeof s->inf[0] * (MAX_INFLIGHT - 1)); s->ninf--; }
	f = &s->inf[s->ninf++];
	memcpy(f->sha1, sha, 20);
	f->ver = *v;
	f->kind = kind;
	f->t_ms = mono_ms();
}

static int inflight_take(sess_t *s, const uint8_t sha[20], const gbah_ver *v, inflight_t *out)
{
	int i;
	for (i = 0; i < s->ninf; i++)
		if (!memcmp(s->inf[i].sha1, sha, 20) && gbah_ver_eq(&s->inf[i].ver, v)) {
			*out = s->inf[i];
			memmove(&s->inf[i], &s->inf[i + 1], sizeof s->inf[0] * (size_t)(s->ninf - i - 1));
			s->ninf--;
			return 1;
		}
	return 0;
}

static int mirror_inflight(sess_t *s)
{
	int i;
	uint64_t now = mono_ms();
	for (i = 0; i < s->ninf; i++)
		if (s->inf[i].kind == K_MIRROR && now - s->inf[i].t_ms < 30000) return 1;
	for (i = 0; i < s->ninf; i++)
		if (s->inf[i].kind == K_MIRROR) { memmove(&s->inf[i], &s->inf[i + 1], sizeof s->inf[0] * (size_t)(s->ninf - i - 1)); s->ninf--; i--; }
	return 0;
}

/* Sends the STORED version of a game as a STATE_PUSH. 0 ok. */
static int push_game(sess_t *s, const uint8_t sha[20], uint8_t flags, int kind)
{
	phone_link *p = s->p;
	void *st = NULL, *sv = NULL;
	size_t sn = 0, vn = 0, n;
	gbah_meta m;
	uint8_t *th = NULL;
	uint32_t tl = 0;
	gbahp_state_push sp;
	uint8_t *buf;
	int rc = -1;

	pthread_mutex_lock(&p->store_mtx);
	if (gbah_store_read(p->games_dir, sha, &st, &sn, &sv, &vn) == 0)
		rc = gbah_store_meta(p->games_dir, sha, p->cfg.device_id, &m, &th, &tl);
	pthread_mutex_unlock(&p->store_mtx);
	if (rc != 0 || !m.has_data) { gbah_fs_free(st); gbah_fs_free(sv); free(th); return -1; }
	if (tl != PHONE_THUMB_BYTES) tl = 0;

	memset(&sp, 0, sizeof sp);
	memcpy(sp.sha1_rom, sha, 20);
	sp.version = m.version.counter; memcpy(sp.origin_device, m.version.dev, 16);
	sp.base = m.base.counter;       memcpy(sp.base_device, m.base.dev, 16);
	sp.flags = flags;
	sp.timestamp_ms = m.timestamp_ms;
	sp.frame = m.frame;
	sp.state = (const uint8_t *)st; sp.state_len = (uint32_t)sn;
	sp.sav = (const uint8_t *)sv;   sp.sav_len = (uint32_t)vn;
	sp.thumb = th;                  sp.thumb_len = tl;
	n = gbahp_encode_state_push(&sp, NULL, 0);
	buf = n ? (uint8_t *)malloc(n) : NULL;
	rc = -1;
	if (buf && gbahp_encode_state_push(&sp, buf, n) == n && send_frame(s, buf, n) == 0) {
		inflight_add(s, sha, &m.version, kind);
		rc = 0;
	}
	free(buf);
	gbah_fs_free(st); gbah_fs_free(sv); free(th);
	LOGI("push flags=%d v%llu -> %d", flags, (unsigned long long)m.version.counter, rc);
	return rc;
}

/* The game open on the phone goes to the PC: freeze, make the store match, push HANDOFF. */
static void start_handoff(sess_t *s, const uint8_t sha[20])
{
	phone_link *p = s->p;
	pthread_mutex_lock(&p->mtx);
	p->handoff_pending = 1;
	memcpy(p->handoff_sha, sha, 20);
	pthread_mutex_unlock(&p->mtx);
	update_hold(p);
	if (checkpoint(p, 1, NULL) < 0 || push_game(s, sha, GBAHP_PUSH_HANDOFF, K_HANDOFF) != 0) {
		pthread_mutex_lock(&p->mtx);
		p->handoff_pending = 0;
		pthread_mutex_unlock(&p->mtx);
		update_hold(p);
		notice(p, "Nao foi possivel enviar o jogo ao PC");
	}
}

static void conflict_remove(phone_link *p, const uint8_t sha[20])
{
	int i;
	pthread_mutex_lock(&p->mtx);
	for (i = 0; i < p->nconf; i++)
		if (!memcmp(p->conf[i].sha1, sha, 20)) {
			free(p->conf[i].rthumb);
			memmove(&p->conf[i], &p->conf[i + 1], sizeof p->conf[0] * (size_t)(p->nconf - i - 1));
			p->nconf--;
			break;
		}
	pthread_mutex_unlock(&p->mtx);
}

static void conflict_add(sess_t *s, const uint8_t sha[20], const gbah_meta *local, const gbah_meta *remote, int have_remote)
{
	phone_link *p = s->p;
	conflict_t *c = NULL;
	int i, t;
	pthread_mutex_lock(&p->mtx);
	for (i = 0; i < p->nconf; i++) if (!memcmp(p->conf[i].sha1, sha, 20)) c = &p->conf[i];
	if (!c && p->nconf < MAX_CONF) {
		c = &p->conf[p->nconf++];
		memset(c, 0, sizeof *c);
		memcpy(c->sha1, sha, 20);
		for (t = 0; t < 4; t++)    /* a THUMB may have arrived before the conflict was known */
			if (s->tcache[t].th && !memcmp(s->tcache[t].sha, sha, 20)) { c->rthumb = s->tcache[t].th; s->tcache[t].th = NULL; }
	}
	if (c) {
		c->local = *local;
		if (have_remote) c->remote = *remote;
		c->have_remote = have_remote;
		c->waiting_pc = 0;
	}
	pthread_mutex_unlock(&p->mtx);
	update_hold(p);
}

static void place_remove(phone_link *p, const uint8_t sha[20])
{
	int i;
	pthread_mutex_lock(&p->mtx);
	for (i = 0; i < p->nplace; i++)
		if (!memcmp(p->place[i].sha1, sha, 20)) {
			memmove(&p->place[i], &p->place[i + 1], sizeof p->place[0] * (size_t)(p->nplace - i - 1));
			p->nplace--;
			break;
		}
	pthread_mutex_unlock(&p->mtx);
}

/* A conflict was resolved and both sides hold the same winner: ask where to continue. If the PC is
 * busy with another game the answer is obvious (the phone) and given right away. */
static void place_add(sess_t *s, const uint8_t sha[20])
{
	phone_link *p = s->p;
	int i, busy_pc, found = 0;
	pthread_mutex_lock(&p->mtx);
	busy_pc = p->pc_playing && memcmp(p->pc_sha, sha, 20) != 0;
	if (!busy_pc) {
		for (i = 0; i < p->nplace; i++) if (!memcmp(p->place[i].sha1, sha, 20)) { p->place[i].waiting = 0; found = 1; }
		if (!found && p->nplace < MAX_CONF) {
			place_t *q = &p->place[p->nplace++];
			memset(q, 0, sizeof *q);
			memcpy(q->sha1, sha, 20);
		}
	}
	pthread_mutex_unlock(&p->mtx);
	if (busy_pc) {
		send_control(s, GBAHP_CTL_PLACE_PHONE, sha, 0);
		notice(p, "O PC esta jogando outro jogo: este continua no celular");
	}
	update_hold(p);
}

static int have_rom(phone_link *p, const uint8_t sha[20])
{
	char src[600];
	return rom_source(p, sha, src, sizeof src);
}

static void leave_on_pc(phone_link *p, const char *why);

/* Applies the stored version of the loaded game to the core. */
static int apply_stored(phone_link *p, const uint8_t sha[20])
{
	void *st = NULL, *sv = NULL;
	size_t sn = 0, vn = 0;
	int rc = -1;
	pthread_mutex_lock(&p->store_mtx);
	if (gbah_store_read(p->games_dir, sha, &st, &sn, &sv, &vn) == 0) rc = 0;
	pthread_mutex_unlock(&p->store_mtx);
	if (rc == 0) {
		rc = H(p)->apply(H(p)->user, sha, st, sn, sv, vn);
		if (rc == 0) remember_saved(p, sha, H(p)->frame(H(p)->user), H(p)->sav_hash(H(p)->user));
	}
	gbah_fs_free(st); gbah_fs_free(sv);
	return rc;
}

/* The PC no longer plays the game it had (cable pulled, closed, switched): back to the phone from
 * the last copy it stored. */
static void leave_on_pc(phone_link *p, const char *why)
{
	uint8_t sha[20], cur[20];
	gbah_meta m;
	char when[40], msg[200];
	int had, is_cur;
	pthread_mutex_lock(&p->mtx);
	had = p->on_pc_has;
	memcpy(sha, p->on_pc_sha, 20);
	pthread_mutex_unlock(&p->mtx);
	if (!had) return;
	store_meta_locked(p, sha, &m, NULL, NULL);
	fmt_time(m.timestamp_ms, when, sizeof when);
	is_cur = loaded_sha(p, cur) && !memcmp(cur, sha, 20);
	/* the game stays "on the PC" (held, never saved) until the stored copy is back in the core, so the
	 * UI never shows a phone game that has not been resumed yet */
	if (is_cur && apply_stored(p, sha) != 0) notice(p, "Nao foi possivel retomar o jogo guardado");
	pthread_mutex_lock(&p->mtx);
	p->on_pc_has = 0;
	pthread_mutex_unlock(&p->mtx);
	update_hold(p);
	snprintf(msg, sizeof msg, "%s: retomando do ultimo espelho (%s)", why, when);
	notice(p, "%s", msg);
}

/* ---- incoming STATE_PUSH -------------------------------------------------------------- */

static int hold_push(sess_t *s, pushcopy *pc)
{
	int i;
	for (i = 0; i < s->nheld; i++)   /* a newer push of the same game replaces the old one */
		if (!memcmp(s->held[i].sha1, pc->sha1, 20)) { pc_free(&s->held[i]); s->held[i] = *pc; memset(pc, 0, sizeof *pc); return 0; }
	if (s->nheld == MAX_HELD) return -1;
	s->held[s->nheld++] = *pc;
	memset(pc, 0, sizeof *pc);
	return 0;
}

static void ask_rom(sess_t *s, const uint8_t sha[20])
{
	int i;
	gbahp_rom_request rq;
	uint8_t out[64];
	for (i = 0; i < s->nasked; i++) if (!memcmp(s->asked[i], sha, 20)) return;
	if (s->nasked < MAX_HELD * 2) memcpy(s->asked[s->nasked++], sha, 20);
	memcpy(rq.sha1, sha, 20);
	send_frame(s, out, gbahp_encode_rom_request(&rq, out, sizeof out));
	{ char hx[41]; gbah_sha1_hex(sha, hx); LOGI("ROM_REQUEST sent %s", hx); }
}

/* pc stays owned by the caller unless the push was held (then pc is zeroed). */
static void handle_push(sess_t *s, pushcopy *pc)
{
	phone_link *p = s->p;
	uint8_t cur[20];
	gbah_meta m, r, nm;
	int d, is_loaded, resolved, handoff, store = 0, take, on_pc_this, leave_now = 0;

	if (!have_rom(p, pc->sha1)) {
		uint8_t sha[20];
		gbah_ver ver = pc->ver;
		memcpy(sha, pc->sha1, 20);                 /* hold_push zeroes *pc when it keeps the push */
		if (hold_push(s, pc) == 0) ask_rom(s, sha);
		else send_ack(s, sha, &ver, GBAHP_STATE_ACK_ERROR);
		return;
	}
	if (!pc->sn) { send_ack(s, pc->sha1, &pc->ver, GBAHP_STATE_ACK_ERROR); return; }

	is_loaded = loaded_sha(p, cur) && !memcmp(cur, pc->sha1, 20);
	pthread_mutex_lock(&p->mtx);
	on_pc_this = p->on_pc_has && !memcmp(p->on_pc_sha, pc->sha1, 20);
	pthread_mutex_unlock(&p->mtx);
	if (is_loaded && !on_pc_this) checkpoint(p, 0, NULL);   /* compare against what the game really is now */

	store_meta_locked(p, pc->sha1, &m, NULL, NULL);
	r = meta_of_push(pc);
	d = gbah_sync_decide(&m, &r);
	resolved = (pc->flags & GBAHP_PUSH_RESOLVED) != 0;
	handoff = (pc->flags & GBAHP_PUSH_HANDOFF) != 0;
	LOGI("push flags=%d decide=%d", pc->flags, d);

	if (resolved) store = d != GBAH_SYNC_NOTHING;
	else if (d == GBAH_SYNC_RECEIVE) store = 1;
	else if (d == GBAH_SYNC_SEND) {
		send_ack(s, pc->sha1, &pc->ver, GBAHP_STATE_ACK_STALE);
		if (!on_pc_this) push_game(s, pc->sha1, 0, K_SYNC);   /* the PC is behind: bring it up to date */
		return;
	} else if (d == GBAH_SYNC_CONFLICT) {
		send_ack(s, pc->sha1, &pc->ver, GBAHP_STATE_ACK_CONFLICT);
		conflict_add(s, pc->sha1, &m, &r, 1);
		if (pc->tn == PHONE_THUMB_BYTES) {
			pthread_mutex_lock(&p->mtx);
			{
				int i;
				for (i = 0; i < p->nconf; i++)
					if (!memcmp(p->conf[i].sha1, pc->sha1, 20) && !p->conf[i].rthumb) {
						p->conf[i].rthumb = (uint8_t *)malloc(pc->tn);
						if (p->conf[i].rthumb) memcpy(p->conf[i].rthumb, pc->thumb, pc->tn);
					}
			}
			pthread_mutex_unlock(&p->mtx);
		}
		notice(p, "Conflito de versoes: escolha qual manter");
		return;
	}

	/* The core follows the store on a takeover, or when this is the game loaded in it and the push
	 * replaces what we hold (a loaded game whose copy is older had no new frames: the checkpoint
	 * above would have turned it into a conflict otherwise). A game that is on the PC is only stored. */
	leave_now = on_pc_this && store && !handoff && s->leave_set && !memcmp(s->leave_sha, pc->sha1, 20);
	take = handoff || (is_loaded && store && (!on_pc_this || leave_now));

	if (store) {
		int rc;
		if (resolved && m.has_data) {   /* the loser becomes a backup */
			pthread_mutex_lock(&p->store_mtx);
			gbah_store_backup(p->games_dir, pc->sha1, gbah_now_ms(), 3);
			pthread_mutex_unlock(&p->store_mtx);
		}
		nm = m;
		gbah_sync_adopt(&nm, &r);
		pthread_mutex_lock(&p->store_mtx);
		rc = gbah_store_commit(p->games_dir, &nm, pc->state, pc->sn, pc->sav, pc->vn, pc->tn == PHONE_THUMB_BYTES ? pc->thumb : NULL,
		                       pc->tn == PHONE_THUMB_BYTES ? (uint32_t)pc->tn : 0);
		pthread_mutex_unlock(&p->store_mtx);
		send_ack(s, pc->sha1, &pc->ver, rc == 0 ? GBAHP_STATE_ACK_OK : GBAHP_STATE_ACK_ERROR);   /* ACK only after the disk has it */
		if (rc != 0) { notice(p, "Falha ao gravar o jogo recebido"); return; }
	} else {
		send_ack(s, pc->sha1, &pc->ver, GBAHP_STATE_ACK_OK);
	}

	if (resolved || handoff) conflict_remove(p, pc->sha1);
	if (take) {
		if (is_loaded) {
			int rc = H(p)->apply(H(p)->user, pc->sha1, pc->state, pc->sn, pc->vn ? pc->sav : NULL, pc->vn);
			if (rc == 0) remember_saved(p, pc->sha1, H(p)->frame(H(p)->user), H(p)->sav_hash(H(p)->user));
			else notice(p, "Nao foi possivel carregar o estado recebido");
		} else if (handoff) {
			pthread_mutex_lock(&p->mtx);
			p->open_has = 1;
			memcpy(p->open_sha, pc->sha1, 20);
			pthread_mutex_unlock(&p->mtx);
		}
	}
	if (handoff || leave_now) {   /* only now (core already following the store) does the game stop being "on the PC" */
		pthread_mutex_lock(&p->mtx);
		if (p->on_pc_has && !memcmp(p->on_pc_sha, pc->sha1, 20)) p->on_pc_has = 0;
		pthread_mutex_unlock(&p->mtx);
		if (leave_now) { s->leave_set = 0; notice(p, "O PC parou de jogar: continuando do ultimo estado dele"); }
	}
	if (resolved) place_add(s, pc->sha1);
	update_hold(p);
	if (resolved) notice(p, "Versao do PC mantida");
}

static void drop_held(sess_t *s, const uint8_t sha[20], int ack_no_rom)
{
	int i;
	for (i = 0; i < s->nheld; i++)
		if (!memcmp(s->held[i].sha1, sha, 20)) {
			if (ack_no_rom) send_ack(s, sha, &s->held[i].ver, GBAHP_STATE_ACK_NO_ROM);
			pc_free(&s->held[i]);
			memmove(&s->held[i], &s->held[i + 1], sizeof s->held[0] * (size_t)(s->nheld - i - 1));
			s->nheld--;
			return;
		}
}

/* ---- ROM transfer ----------------------------------------------------------------------- */

static void send_rom(sess_t *s, const uint8_t sha[20])
{
	phone_link *p = s->p;
	int fd = open_rom_fd(p, sha);
	struct stat st;
	uint8_t *chunk = NULL, *all = NULL;
	size_t total = 0, off = 0, cap = GBAHP_ROM_CHUNK + 256;
	if (fd < 0) { send_control(s, GBAHP_CTL_ROM_UNAVAILABLE, sha, 0); LOGI("ROM unavailable"); return; }
	if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && (uint64_t)st.st_size <= MAX_ROM_BYTES) {
		total = (size_t)st.st_size;
	} else {   /* a pipe / provider stream: the size is only known after reading it all */
		size_t n = 0, acap = 4u << 20;
		all = (uint8_t *)malloc(acap);
		while (all) {
			ssize_t r;
			if (n == acap) {
				uint8_t *nb;
				if (acap >= MAX_ROM_BYTES) { free(all); all = NULL; break; }
				nb = (uint8_t *)realloc(all, acap * 2);
				if (!nb) { free(all); all = NULL; break; }
				all = nb; acap *= 2;
			}
			r = read(fd, all + n, acap - n);
			if (r < 0 && errno == EINTR) continue;
			if (r <= 0) break;
			n += (size_t)r;
		}
		total = all ? n : 0;
	}
	chunk = (uint8_t *)malloc(cap);
	if (!total || !chunk) {
		close(fd); free(all); free(chunk);
		send_control(s, GBAHP_CTL_ROM_UNAVAILABLE, sha, 0);
		return;
	}
	while (off < total) {
		gbahp_rom_push rp;
		size_t k = total - off < GBAHP_ROM_CHUNK ? total - off : GBAHP_ROM_CHUNK, n;
		uint8_t *data;
		memset(&rp, 0, sizeof rp);
		if (all) data = all + off;
		else {
			size_t got = 0;
			data = (uint8_t *)malloc(k);
			if (!data) break;
			while (got < k) {
				ssize_t r = read(fd, data + got, k - got);
				if (r < 0 && errno == EINTR) continue;
				if (r <= 0) break;
				got += (size_t)r;
			}
			if (got != k) { free(data); LOGI("ROM read short"); break; }
		}
		memcpy(rp.sha1, sha, 20);
		rp.total_size = total; rp.offset = off; rp.data = data; rp.data_len = (uint32_t)k;
		n = gbahp_encode_rom_push(&rp, chunk, cap);
		if (!all) free(data);
		if (!n || send_frame(s, chunk, n) != 0) break;
		off += k;
		if (!atomic_load(&s->p->run_link)) break;
	}
	close(fd); free(all); free(chunk);
	LOGI("ROM sent %zu/%zu", off, total);
}

static void rom_chunk(sess_t *s, const gbahp_rom_push *r)
{
	phone_link *p = s->p;
	uint8_t got[20];
	char path[600];
	if (r->offset == 0) {
		free(s->rx_buf); s->rx_buf = NULL; s->rx_on = 0;
		if (!r->total_size || r->total_size > MAX_ROM_BYTES) return;
		s->rx_buf = (uint8_t *)malloc((size_t)r->total_size);
		if (!s->rx_buf) return;
		memcpy(s->rx_sha, r->sha1, 20);
		s->rx_total = r->total_size; s->rx_len = 0; s->rx_on = 1;
	}
	if (!s->rx_on || memcmp(s->rx_sha, r->sha1, 20) || r->total_size != s->rx_total || r->offset != s->rx_len ||
	    r->offset + r->data_len > s->rx_total) {
		free(s->rx_buf); s->rx_buf = NULL; s->rx_on = 0;
		LOGI("out-of-sequence ROM chunk");
		return;
	}
	memcpy(s->rx_buf + s->rx_len, r->data, r->data_len);
	s->rx_len += r->data_len;
	pthread_mutex_lock(&p->mtx);
	p->rx_active = 1; p->rx_pct = (int)(s->rx_len * 100 / s->rx_total); memcpy(p->rx_sha, s->rx_sha, 20);
	pthread_mutex_unlock(&p->mtx);
	if (s->rx_len < s->rx_total) return;

	s->rx_on = 0;
	pthread_mutex_lock(&p->mtx);
	p->rx_active = 0;
	pthread_mutex_unlock(&p->mtx);
	gbah_sha1(s->rx_buf, s->rx_len, got);
	if (memcmp(got, s->rx_sha, 20) != 0) {
		LOGI("received ROM does not match its SHA-1");
		notice(p, "ROM recebida corrompida");
		drop_held(s, s->rx_sha, 1);
	} else {
		rom_file_path(p, s->rx_sha, path, sizeof path);
		if (gbah_fs_mkdirs(p->roms_dir) == 0 && (gbah_fs_exists(path) || gbah_fs_write_atomic(path, s->rx_buf, s->rx_len) == 0)) {
			int i;
			uint8_t sha[20];
			memcpy(sha, s->rx_sha, 20);
			notice(p, "ROM recebida do PC");
			for (i = 0; i < s->nheld; i++)   /* now the held push can be applied */
				if (!memcmp(s->held[i].sha1, sha, 20)) {
					pushcopy pc = s->held[i];
					memmove(&s->held[i], &s->held[i + 1], sizeof s->held[0] * (size_t)(s->nheld - i - 1));
					s->nheld--;
					handle_push(s, &pc);
					pc_free(&pc);
					break;
				}
		} else {
			notice(p, "Nao foi possivel gravar a ROM recebida");
			drop_held(s, s->rx_sha, 1);
		}
	}
	free(s->rx_buf); s->rx_buf = NULL;
}

/* ---- ACK from the PC for something we pushed -------------------------------------------- */

static void handle_ack(sess_t *s, const gbahp_state_ack *a)
{
	phone_link *p = s->p;
	gbah_ver v;
	inflight_t in;
	memset(&v, 0, sizeof v);
	memcpy(v.dev, a->origin_device, 16);
	v.counter = a->version;
	if (!inflight_take(s, a->sha1_rom, &v, &in)) return;
	LOGI("ACK status=%d kind=%d", a->status, in.kind);

	switch (a->status) {
	case GBAHP_STATE_ACK_OK:
		pthread_mutex_lock(&p->store_mtx);
		gbah_store_mark_synced(p->games_dir, a->sha1_rom, &v);
		pthread_mutex_unlock(&p->store_mtx);
		if (in.kind == K_HANDOFF) {
			pthread_mutex_lock(&p->mtx);
			p->handoff_pending = 0;
			p->on_pc_has = 1;
			memcpy(p->on_pc_sha, a->sha1_rom, 20);
			pthread_mutex_unlock(&p->mtx);
			update_hold(p);
			notice(p, "Jogando no PC");
		} else if (in.kind == K_RESOLVED) {
			conflict_remove(p, a->sha1_rom);
			place_add(s, a->sha1_rom);
			update_hold(p);
			notice(p, "Versao do celular mantida");
		}
		break;
	case GBAHP_STATE_ACK_CONFLICT: {
		gbah_meta lm, rm;
		int i, have = 0;
		store_meta_locked(p, a->sha1_rom, &lm, NULL, NULL);
		memset(&rm, 0, sizeof rm);
		for (i = 0; i < s->nplib; i++)
			if (!memcmp(s->plib[i].sha1, a->sha1_rom, 20)) { rm = meta_of_entry(&s->plib[i]); have = 1; }
		pthread_mutex_lock(&p->mtx);
		if (in.kind == K_HANDOFF) p->handoff_pending = 0;
		pthread_mutex_unlock(&p->mtx);
		conflict_add(s, a->sha1_rom, &lm, &rm, have);
		notice(p, "Conflito de versoes: escolha qual manter");
		break;
	}
	case GBAHP_STATE_ACK_STALE:
		pthread_mutex_lock(&p->mtx);
		if (in.kind == K_HANDOFF) p->handoff_pending = 0;
		pthread_mutex_unlock(&p->mtx);
		update_hold(p);
		if (in.kind != K_MIRROR) notice(p, "O PC ja tem uma versao mais nova");
		break;
	default:
		pthread_mutex_lock(&p->mtx);
		if (in.kind == K_HANDOFF) p->handoff_pending = 0;
		pthread_mutex_unlock(&p->mtx);
		update_hold(p);
		if (in.kind != K_MIRROR) notice(p, a->status == GBAHP_STATE_ACK_NO_ROM ? "O PC nao recebeu a ROM" : "O PC recusou o jogo");
		break;
	}
}

/* ---- LIBRARY / decisions ------------------------------------------------------------------ */

static void send_library(sess_t *s)
{
	phone_link *p = s->p;
	shalist *l = (shalist *)malloc(sizeof *l);
	gbahp_lib_entry *e = (gbahp_lib_entry *)calloc(MAX_GAMES, sizeof *e);
	uint8_t *buf;
	size_t n;
	int i;
	if (!l || !e) { free(l); free(e); return; }
	catalog(p, l);
	for (i = 0; i < l->n; i++) {
		gbah_meta m;
		store_meta_locked(p, l->sha[i], &m, NULL, NULL);
		memcpy(e[i].sha1, l->sha[i], 20);
		e[i].size = rom_size_of(p, l->sha[i]);
		e[i].has_data = m.has_data ? 1 : 0;
		if (m.has_data) {
			memcpy(e[i].version_dev, m.version.dev, 16); e[i].version = m.version.counter;
			memcpy(e[i].base_dev, m.base.dev, 16);       e[i].base = m.base.counter;
			e[i].timestamp_ms = m.timestamp_ms;
			e[i].frame = m.frame;
		}
	}
	n = gbahp_encode_library(e, (uint32_t)l->n, NULL, 0);
	buf = n ? (uint8_t *)malloc(n) : NULL;
	if (buf && gbahp_encode_library(e, (uint32_t)l->n, buf, n) == n) send_frame(s, buf, n);
	free(buf); free(l); free(e);
	LOGI("LIBRARY sent (%d games)", i);
}

/* The PC's LIBRARY decides, per game, what the phone does. */
static void process_pc_library(sess_t *s)
{
	phone_link *p = s->p;
	uint8_t cur[20];
	int has_cur = loaded_sha(p, cur), i, cur_listed = 0, pc_playing;
	uint8_t pc_sha[20];
	s->lib_done = 1;
	if (has_cur) checkpoint(p, 0, NULL);          /* "pause, save, sync": the store must describe the game as it is now */
	pthread_mutex_lock(&p->mtx);
	pc_playing = p->pc_playing;
	memcpy(pc_sha, p->pc_sha, 20);
	pthread_mutex_unlock(&p->mtx);

	for (i = 0; i < s->nplib; i++) {
		const gbahp_lib_entry *e = &s->plib[i];
		gbah_meta m, r;
		int d, is_cur = has_cur && !memcmp(cur, e->sha1, 20), on_pc_this;
		store_meta_locked(p, e->sha1, &m, NULL, NULL);
		r = meta_of_entry(e);
		d = gbah_sync_decide(&m, &r);
		if (is_cur) cur_listed = 1;
		pthread_mutex_lock(&p->mtx);
		on_pc_this = p->on_pc_has && !memcmp(p->on_pc_sha, e->sha1, 20);
		pthread_mutex_unlock(&p->mtx);
		LOGI("library: decide=%d is_cur=%d", d, is_cur);
		switch (d) {
		case GBAH_SYNC_SEND:
		case GBAH_SYNC_NOTHING:
			if (on_pc_this && s->leave_pending) { s->leave_pending = 0; leave_on_pc(p, "O PC parou de jogar"); break; }
			if (on_pc_this) break;                                   /* it is on the PC already */
			if (is_cur && (!pc_playing || !memcmp(pc_sha, e->sha1, 20))) { start_handoff(s, e->sha1); break; }
			if (d == GBAH_SYNC_SEND) push_game(s, e->sha1, 0, K_SYNC);
			break;
		case GBAH_SYNC_CONFLICT:
			if (on_pc_this) s->leave_pending = 0;
			conflict_add(s, e->sha1, &m, &r, 1);
			notice(p, "Conflito de versoes: escolha qual manter");
			break;
		default:   /* RECEIVE: the PC pushes */
			if (on_pc_this && s->leave_pending) {   /* apply the PC's copy when it arrives, then play here */
				s->leave_pending = 0; s->leave_set = 1; s->leave_at = mono_ms();
				memcpy(s->leave_sha, e->sha1, 20);
			}
			break;
		}
	}
	if (s->leave_pending) { s->leave_pending = 0; leave_on_pc(p, "O PC parou de jogar"); }   /* not listed by the PC */
	/* The open game is not on the PC at all: hand it over (the PC will ask for the ROM). */
	if (has_cur && !cur_listed && !pc_playing) {
		gbah_meta m;
		pthread_mutex_lock(&p->mtx);
		cur_listed = p->on_pc_has && !memcmp(p->on_pc_sha, cur, 20);
		pthread_mutex_unlock(&p->mtx);
		store_meta_locked(p, cur, &m, NULL, NULL);
		if (!cur_listed && m.has_data) start_handoff(s, cur);
	}
	if (has_cur && pc_playing && memcmp(pc_sha, cur, 20)) notice(p, "O PC esta jogando outro jogo: este continua no celular");
}

static void handle_status(sess_t *s, const gbahp_control *c)
{
	phone_link *p = s->p;
	int playing = c->arg != 0, leave = 0, placed = 0, i;
	pthread_mutex_lock(&p->mtx);
	p->pc_playing = playing;
	if (playing) memcpy(p->pc_sha, c->sha1, 20); else memset(p->pc_sha, 0, 20);
	if (playing)   /* the PC took the game we asked it to continue (PLACE_PC) */
		for (i = 0; i < p->nplace; i++)
			if (p->place[i].waiting && !memcmp(p->place[i].sha1, c->sha1, 20)) {
				memmove(&p->place[i], &p->place[i + 1], sizeof p->place[0] * (size_t)(p->nplace - i - 1));
				p->nplace--;
				p->on_pc_has = 1;
				memcpy(p->on_pc_sha, c->sha1, 20);
				placed = 1;
				break;
			}
	if (p->on_pc_has && !p->handoff_pending && (!playing || memcmp(p->on_pc_sha, c->sha1, 20))) leave = 1;
	pthread_mutex_unlock(&p->mtx);
	s->got_status = 1;
	if (placed) { update_hold(p); notice(p, "Jogando no PC"); }
	if (leave && !s->lib_done) s->leave_pending = 1;   /* (re)connecting: first see what the PC has to give back */
	else if (leave) leave_on_pc(p, "O PC parou de jogar");
}

static void handle_thumb(sess_t *s, const gbahp_thumb *t)
{
	phone_link *p = s->p;
	int i, placed = 0;
	if (t->thumb_len != PHONE_THUMB_BYTES) return;
	pthread_mutex_lock(&p->mtx);
	for (i = 0; i < p->nconf; i++)
		if (!memcmp(p->conf[i].sha1, t->sha1, 20)) {
			if (!p->conf[i].rthumb) p->conf[i].rthumb = (uint8_t *)malloc(PHONE_THUMB_BYTES);
			if (p->conf[i].rthumb) memcpy(p->conf[i].rthumb, t->thumb, PHONE_THUMB_BYTES);
			placed = 1;
		}
	pthread_mutex_unlock(&p->mtx);
	if (!placed) {
		uint8_t **slot = &s->tcache[s->tnext].th;
		free(*slot);
		*slot = (uint8_t *)malloc(PHONE_THUMB_BYTES);
		if (*slot) { memcpy(*slot, t->thumb, PHONE_THUMB_BYTES); memcpy(s->tcache[s->tnext].sha, t->sha1, 20); }
		s->tnext = (s->tnext + 1) % 4;
	}
}

/* The game is on the PC (handed over and not given back)? */
static int game_on_pc(phone_link *p)
{
	int has;
	pthread_mutex_lock(&p->mtx);
	has = p->on_pc_has;
	pthread_mutex_unlock(&p->mtx);
	return has;
}

/* Phone as a controller (phase 5). Only while the game is on the PC; `force` = a change (send now),
 * otherwise only the periodic re-send of held keys. Link thread only: it owns the socket. */
static int input_allowed(phone_link *p)
{
	uint8_t cur[20];
	int ok;
	if (!atomic_load(&p->control_on) || !loaded_sha(p, cur)) return 0;
	pthread_mutex_lock(&p->mtx);
	ok = p->on_pc_has && !memcmp(p->on_pc_sha, cur, 20) && p->pc_playing && !memcmp(p->pc_sha, cur, 20);
	pthread_mutex_unlock(&p->mtx);
	return ok;
}

static void send_input(sess_t *s, uint16_t keys, int force)
{
	gbahp_input in;
	uint8_t out[64];
	uint64_t t = mono_ms();
	if (!input_allowed(s->p)) {   /* not in control mode / the open game is not the PC's: release whatever the PC still holds, once */
		if (s->in_keys) { in.seq = ++s->in_seq; in.keys = 0; s->in_keys = 0; send_frame(s, out, gbahp_encode_input(&in, out, sizeof out)); }
		return;
	}
	if (!force && (s->in_keys == 0 || t - s->in_sent_ms < INPUT_RESEND_MS)) return;
	in.seq = ++s->in_seq;
	in.keys = keys;
	s->in_keys = keys;
	s->in_sent_ms = t;
	send_frame(s, out, gbahp_encode_input(&in, out, sizeof out));
}

static void handle_cmd(sess_t *s, const cmd_t *c)
{
	phone_link *p = s->p;
	switch (c->type) {
	case CMD_RETURN: {
		uint8_t sha[20];
		int has;
		pthread_mutex_lock(&p->mtx);
		has = p->on_pc_has;
		memcpy(sha, p->on_pc_sha, 20);
		pthread_mutex_unlock(&p->mtx);
		if (has) send_control(s, GBAHP_CTL_RETURN, sha, 0);
		break;
	}
	case CMD_FETCH:
		send_control(s, GBAHP_CTL_FETCH, c->sha1, 0);
		break;
	case CMD_PLACE: {
		int i, have = 0;
		pthread_mutex_lock(&p->mtx);
		for (i = 0; i < p->nplace; i++)
			if (!memcmp(p->place[i].sha1, c->sha1, 20)) { have = 1; if (c->arg) { p->place[i].waiting = 1; p->place[i].t_ms = mono_ms(); } }
		pthread_mutex_unlock(&p->mtx);
		if (!have) break;
		if (c->arg) send_control(s, GBAHP_CTL_PLACE_PC, c->sha1, 0);
		else {
			send_control(s, GBAHP_CTL_PLACE_PHONE, c->sha1, 0);
			place_remove(p, c->sha1);
			update_hold(p);
		}
		break;
	}
	case CMD_SETTINGS_SET:
		send_control(s, GBAHP_CTL_SETTINGS_SET, NULL, c->arg ? GBAHP_SET_PREFER_NEWEST : 0u);
		pthread_mutex_lock(&p->mtx);
		p->pc_pref_known = 1; p->pc_pref = c->arg != 0;
		pthread_mutex_unlock(&p->mtx);
		break;
	case CMD_SETTINGS_ANNOUNCE:
		send_control(s, GBAHP_CTL_SETTINGS, NULL, atomic_load(&p->prefer_newest) ? GBAHP_SET_PREFER_NEWEST : 0u);
		break;
	case CMD_HANDOFF: {   /* "send to the PC" for the game open on the phone */
		uint8_t cur[20];
		int busy;
		if (!loaded_sha(p, cur)) break;
		pthread_mutex_lock(&p->mtx);
		busy = p->on_pc_has || p->handoff_pending || (p->pc_playing && memcmp(p->pc_sha, cur, 20));
		pthread_mutex_unlock(&p->mtx);
		if (!busy) start_handoff(s, cur);
		else notice(p, "O PC esta ocupado com outro jogo");
		break;
	}
	case CMD_RESOLVE:
		if (c->arg) {   /* keep the phone's */
			if (push_game(s, c->sha1, GBAHP_PUSH_RESOLVED, K_RESOLVED) != 0) notice(p, "Nao foi possivel enviar a versao do celular");
		} else {
			int i;
			pthread_mutex_lock(&p->mtx);
			for (i = 0; i < p->nconf; i++) if (!memcmp(p->conf[i].sha1, c->sha1, 20)) p->conf[i].waiting_pc = 1;
			pthread_mutex_unlock(&p->mtx);
			send_control(s, GBAHP_CTL_FETCH, c->sha1, 0);
		}
		break;
	}
}

static void sess_free(sess_t *s)
{
	int i;
	for (i = 0; i < s->nheld; i++) pc_free(&s->held[i]);
	for (i = 0; i < 4; i++) free(s->tcache[i].th);
	free(s->rx_buf);
}

static void copy_push(pushcopy *pc, const gbahp_state_push *sp)
{
	memset(pc, 0, sizeof *pc);
	memcpy(pc->sha1, sp->sha1_rom, 20);
	pc->ver.counter = sp->version; memcpy(pc->ver.dev, sp->origin_device, 16);
	pc->base.counter = sp->base;   memcpy(pc->base.dev, sp->base_device, 16);
	pc->flags = sp->flags; pc->ts = sp->timestamp_ms; pc->frame = sp->frame;
	pc->state = (uint8_t *)malloc(sp->state_len ? sp->state_len : 1); pc->sn = sp->state_len;
	pc->sav = (uint8_t *)malloc(sp->sav_len ? sp->sav_len : 1);       pc->vn = sp->sav_len;
	pc->thumb = (uint8_t *)malloc(sp->thumb_len ? sp->thumb_len : 1); pc->tn = sp->thumb_len;
	if (!pc->state || !pc->sav || !pc->thumb) { pc_free(pc); return; }
	memcpy(pc->state, sp->state, sp->state_len);
	memcpy(pc->sav, sp->sav, sp->sav_len);
	memcpy(pc->thumb, sp->thumb, sp->thumb_len);
}

/* "Keep the newest version": resolves open conflicts by itself when both devices have the setting on
 * (no pending difference). The loser still goes to the backup (handle_push / the PC's on_push). */
static void auto_resolve(sess_t *s)
{
	phone_link *p = s->p;
	int i;
	if (!atomic_load(&p->prefer_newest)) return;
	for (;;) {
		uint8_t sha[20];
		gbah_meta l, r;
		int pick = 0, found = 0;
		pthread_mutex_lock(&p->mtx);
		if (p->pc_pref_known && p->pc_pref) {
			for (i = 0; i < p->nconf; i++)
				if (p->conf[i].have_remote && !p->conf[i].waiting_pc) {
					memcpy(sha, p->conf[i].sha1, 20);
					l = p->conf[i].local; r = p->conf[i].remote;
					p->conf[i].waiting_pc = 1;   /* provisional: this one is being handled */
					found = 1;
					break;
				}
		}
		pthread_mutex_unlock(&p->mtx);
		if (!found) return;
		pick = gbah_sync_newest(&l, &r);
		{
			char when[40], msg[120];
			fmt_time(pick >= 0 ? l.timestamp_ms : r.timestamp_ms, when, sizeof when);
			snprintf(msg, sizeof msg, "Mantida a versao mais recente (%s, %s)", pick >= 0 ? "celular" : "PC", when);
			notice(p, "%s", msg);
		}
		if (pick >= 0) {   /* the phone's (also on an exact tie: nothing is lost either way) */
			if (push_game(s, sha, GBAHP_PUSH_RESOLVED, K_RESOLVED) != 0) {
				notice(p, "Nao foi possivel enviar a versao do celular");
				pthread_mutex_lock(&p->mtx);   /* the manual panel takes over */
				for (i = 0; i < p->nconf; i++) if (!memcmp(p->conf[i].sha1, sha, 20)) { p->conf[i].have_remote = 0; p->conf[i].waiting_pc = 0; }
				pthread_mutex_unlock(&p->mtx);
			}   /* else waiting_pc stays 1: not decided again before the ACK removes the conflict */
		} else {
			send_control(s, GBAHP_CTL_FETCH, sha, 0);   /* waiting_pc stays 1 until the RESOLVED push arrives */
		}
	}
}

/* Timeouts and automatic decisions, once per loop turn (link thread). */
static void housekeeping(sess_t *s, uint64_t t)
{
	phone_link *p = s->p;
	int i, timed_out = 0;
	if (s->lib_done) auto_resolve(s);
	pthread_mutex_lock(&p->mtx);
	for (i = 0; i < p->nplace; i++)
		if (p->place[i].waiting && t - p->place[i].t_ms > PLACE_WAIT_MS) { p->place[i].waiting = 0; timed_out = 1; }
	pthread_mutex_unlock(&p->mtx);
	if (timed_out) notice(p, "O PC nao respondeu: escolha de novo onde continuar");
	if (s->leave_set && t - s->leave_at > LEAVE_WAIT_MS) {   /* the PC's newer copy never came: play from the stored one */
		s->leave_set = 0;
		leave_on_pc(p, "O PC parou de jogar");
	}
}

/* Returns 1 if the session reached CONNECTED. */
static int session(phone_link *p, gbahp_sock sock)
{
	sess_t *s = (sess_t *)calloc(1, sizeof *s);
	gbahp_frame f;
	gbahp_hello me;
	gbahp_hello_ack ack;
	uint8_t out[512];
	size_t n;
	int r = 0, waited, established = 0;
	uint64_t last_rx, last_ping;
	uint64_t token = 0;
	if (!s) { gbahp_net_close(sock); return 0; }
	s->p = p;

	pthread_mutex_lock(&p->mtx);
	p->sock = sock;
	pthread_mutex_unlock(&p->mtx);
	gbahp_conn_init(&s->c, sock);

	memset(&me, 0, sizeof me);
	me.proto_version = GBAHP_VERSION;
	memcpy(me.device_id, p->cfg.device_id, 16);
	me.core_version = gbahp_str_c(p->core_ver);
	me.name = gbahp_str_c(p->name);
	n = gbahp_encode_hello(&me, out, sizeof out);
	if (!n || send_frame(s, out, n) != 0) goto done;
	for (waited = 0; waited < 5000 && atomic_load(&p->run_link); waited += 200) {
		r = gbahp_conn_recv(&s->c, &f, 200);
		if (r != 0) break;
	}
	if (r != 1 || f.type != GBAHP_MSG_HELLO_ACK || gbahp_decode_hello_ack(f.payload, f.len, &ack) != 0) goto done;
	if (ack.status != GBAHP_ACK_OK) {
		LOGI("PC refused the connection (status %d)", ack.status);
		pthread_mutex_lock(&p->mtx);
		p->refuse_status = ack.status;
		p->refuse_proto = ack.hello.proto_version;
		pthread_mutex_unlock(&p->mtx);
		atomic_store(&p->status, PL_ST_REFUSED);
		goto done;
	}
	pthread_mutex_lock(&p->mtx);
	{
		size_t k = ack.hello.name.n < sizeof p->peer - 1 ? ack.hello.name.n : sizeof p->peer - 1;
		memcpy(p->peer, ack.hello.name.p, k);
		p->peer[k] = 0;
		memcpy(p->peer_dev, ack.hello.device_id, 16);
	}
	pthread_mutex_unlock(&p->mtx);
	atomic_store(&p->status, PL_ST_CONNECTED);
	pthread_mutex_lock(&p->mtx);
	p->refuse_status = 0;
	pthread_mutex_unlock(&p->mtx);
	established = 1;
	LOGI("connected to PC '%s'", p->peer);

	{   /* the library must describe the open game as it is right now */
		uint8_t cur[20];
		if (loaded_sha(p, cur)) checkpoint(p, 0, NULL);
	}
	send_control(s, GBAHP_CTL_SETTINGS, NULL, atomic_load(&p->prefer_newest) ? GBAHP_SET_PREFER_NEWEST : 0u);
	send_library(s);

	last_rx = last_ping = mono_ms();
	while (atomic_load(&p->run_link)) {
		uint64_t t;
		cmd_t cmd;
		int have_cmd;
		r = gbahp_conn_recv(&s->c, &f, atomic_load(&p->control_on) && game_on_pc(p) ? FAST_POLL_MS : 100);
		t = mono_ms();
		if (r < 0) break;
		if (r == 1) {
			last_rx = t;
			switch (f.type) {
			case GBAHP_MSG_BYE: goto done;
			case GBAHP_MSG_PING: {
				gbahp_ping pg;
				if (gbahp_decode_ping(f.payload, f.len, &pg) == 0) {
					n = gbahp_encode_pong(&pg, out, sizeof out);
					if (send_frame(s, out, n) != 0) goto done;
				}
				break;
			}
			case GBAHP_MSG_LIBRARY: {
				gbahp_library l;
				if (gbahp_decode_library(f.payload, f.len, &l) == 0) {
					uint32_t i;
					s->nplib = 0;
					for (i = 0; i < l.count && s->nplib < MAX_GAMES; i++) gbahp_library_get(&l, i, &s->plib[s->nplib++]);
					s->got_plib = 1; s->lib_done = 0; s->plib_at = t;
				}
				break;
			}
			case GBAHP_MSG_CONTROL: {
				gbahp_control c;
				if (gbahp_decode_control(f.payload, f.len, &c) == 0) {
					if (c.cmd == GBAHP_CTL_PC_STATUS) handle_status(s, &c);
					else if (c.cmd == GBAHP_CTL_SETTINGS) {
						pthread_mutex_lock(&p->mtx);
						p->pc_pref_known = 1;
						p->pc_pref = (c.arg & GBAHP_SET_PREFER_NEWEST) != 0;
						pthread_mutex_unlock(&p->mtx);
					}
					else if (c.cmd == GBAHP_CTL_ROM_UNAVAILABLE) { drop_held(s, c.sha1, 1); notice(p, "O PC nao tem a ROM"); }
				}
				break;
			}
			case GBAHP_MSG_STATE_PUSH: {
				gbahp_state_push sp;
				if (gbahp_decode_state_push(f.payload, f.len, &sp) == 0) {
					pushcopy pc;
					copy_push(&pc, &sp);
					if (pc.state) { handle_push(s, &pc); pc_free(&pc); }
					else { gbah_ver v; memcpy(v.dev, sp.origin_device, 16); v.counter = sp.version; send_ack(s, sp.sha1_rom, &v, GBAHP_STATE_ACK_ERROR); }
				}
				break;
			}
			case GBAHP_MSG_STATE_ACK: {
				gbahp_state_ack a;
				if (gbahp_decode_state_ack(f.payload, f.len, &a) == 0) handle_ack(s, &a);
				break;
			}
			case GBAHP_MSG_ROM_REQUEST: {
				gbahp_rom_request rq;
				if (gbahp_decode_rom_request(f.payload, f.len, &rq) == 0) send_rom(s, rq.sha1);
				break;
			}
			case GBAHP_MSG_ROM_PUSH: {
				gbahp_rom_push rp;
				if (gbahp_decode_rom_push(f.payload, f.len, &rp) == 0) rom_chunk(s, &rp);
				break;
			}
			case GBAHP_MSG_THUMB: {
				gbahp_thumb th;
				if (gbahp_decode_thumb(f.payload, f.len, &th) == 0) handle_thumb(s, &th);
				break;
			}
			default: break;   /* PONG ... */
			}
		}
		if (s->got_plib && !s->lib_done && (s->got_status || t - s->plib_at > LIB_WAIT_STATUS_MS)) process_pc_library(s);

		for (;;) {   /* commands from the UI */
			pthread_mutex_lock(&p->mtx);
			have_cmd = p->ncmd > 0;
			if (have_cmd) { cmd = p->cmds[0]; memmove(&p->cmds[0], &p->cmds[1], sizeof p->cmds[0] * (size_t)(p->ncmd - 1)); p->ncmd--; }
			pthread_mutex_unlock(&p->mtx);
			if (!have_cmd) break;
			handle_cmd(s, &cmd);
		}
		if (atomic_exchange(&p->keys_dirty, 0)) send_input(s, (uint16_t)atomic_load(&p->latest_keys), 1);
		else if (s->in_keys) send_input(s, s->in_keys, 0);   /* held keys: periodic re-send (or one release if no longer allowed) */
		housekeeping(s, t);
		if (atomic_exchange(&p->mirror_req, 0) && s->lib_done && !mirror_inflight(s)) {
			uint8_t cur[20];
			int skip;
			if (loaded_sha(p, cur)) {
				pthread_mutex_lock(&p->mtx);
				skip = (p->on_pc_has && !memcmp(p->on_pc_sha, cur, 20)) || (p->handoff_pending && !memcmp(p->handoff_sha, cur, 20)) ||
				       (p->pc_playing && !memcmp(p->pc_sha, cur, 20));
				{ int i; for (i = 0; i < p->nconf; i++) if (!memcmp(p->conf[i].sha1, cur, 20)) skip = 1; }
				pthread_mutex_unlock(&p->mtx);
				if (!skip) push_game(s, cur, GBAHP_PUSH_MIRROR, K_MIRROR);
			}
		}
		if (t - last_rx > IDLE_TIMEOUT_MS) { LOGI("PC silent, dropping"); break; }
		if (t - last_ping >= PING_EVERY_MS) {
			gbahp_ping pg;
			pg.token = ++token;
			last_ping = t;
			n = gbahp_encode_ping(&pg, out, sizeof out);
			if (send_frame(s, out, n) != 0) break;
		}
	}
	if (atomic_load(&p->run_link) == 0) {   /* leaving on purpose: say goodbye */
		gbahp_bye b = { 0 };
		n = gbahp_encode_bye(&b, out, sizeof out);
		if (n) send_frame(s, out, n);
	}
done:
	pthread_mutex_lock(&p->mtx);
	p->sock = GBAHP_BAD_SOCK;
	p->peer[0] = 0;
	pthread_mutex_unlock(&p->mtx);
	sess_free(s);
	gbahp_conn_close(&s->c);
	free(s);
	return established;
}

/* Session-scoped state goes away with the session. */
static void session_cleanup(phone_link *p)
{
	int i;
	pthread_mutex_lock(&p->mtx);
	/* A handoff cut by the cable: the PC may or may not have started it. Never resume by ourselves (two
	 * sides playing is exactly what this avoids): the game counts as "on the PC, not synchronised" and
	 * the user may play it here anyway (phone_cmd_play_here) or reconnect (the PC's copy then wins/ties). */
	if (p->handoff_pending) { p->on_pc_has = 1; memcpy(p->on_pc_sha, p->handoff_sha, 20); }
	p->handoff_pending = 0;
	p->pc_playing = 0;
	p->pc_pref_known = 0;
	p->nplace = 0;
	atomic_store(&p->keys_dirty, 0);
	p->rx_active = 0;
	for (i = 0; i < p->nconf; i++) free(p->conf[i].rthumb);
	p->nconf = 0;
	p->ncmd = 0;
	pthread_mutex_unlock(&p->mtx);
	update_hold(p);
}

static void *link_main(void *arg)
{
	phone_link *p = (phone_link *)arg;
	int delay = RETRY_MIN_MS;
	gbahp_net_init();
	while (atomic_load(&p->run_link)) {
		gbahp_sock s;
		int refused, est;
		atomic_store(&p->status, PL_ST_SEARCHING);
		s = gbahp_net_connect_loopback(p->port, 1000);
		if (s == GBAHP_BAD_SOCK) {
			nap(p, &p->run_link, delay);
			delay = delay * 3 / 2;
			if (delay > RETRY_MAX_MS) delay = RETRY_MAX_MS;
			continue;
		}
		est = session(p, s);
		delay = est ? RETRY_MIN_MS : RETRY_MAX_MS;
		refused = atomic_load(&p->status) == PL_ST_REFUSED;
		session_cleanup(p);
		/* Cable gone: a game on the PC STAYS on the PC (held, "not synchronised"). The PC keeps playing it;
		 * resuming here by ourselves would make two sides play the same game. */
		if (est && atomic_load(&p->run_link) && game_on_pc(p)) notice(p, "Cabo desconectado: o jogo continua no PC");
		if (atomic_load(&p->run_link)) {
			if (!refused) atomic_store(&p->status, PL_ST_SEARCHING);
			nap(p, &p->run_link, refused ? RETRY_MAX_MS : delay);
		}
	}
	atomic_store(&p->status, PL_ST_STOPPED);
	return NULL;
}

/* ------------------------------------------------------------------ public API */

phone_link *phone_create(const phone_cfg *cfg)
{
	phone_link *p = (phone_link *)calloc(1, sizeof *p);
	if (!p) return NULL;
	p->cfg = *cfg;
	snprintf(p->games_dir, sizeof p->games_dir, "%s", cfg->games_dir);
	snprintf(p->roms_dir, sizeof p->roms_dir, "%s", cfg->roms_dir);
	snprintf(p->name, sizeof p->name, "%s", cfg->device_name ? cfg->device_name : "Android");
	snprintf(p->core_ver, sizeof p->core_ver, "%s", cfg->core_version ? cfg->core_version : gbah_core_version());
	p->port = cfg->port ? cfg->port : GBAHP_DEFAULT_PORT;
	p->sock = GBAHP_BAD_SOCK;
	pthread_mutex_init(&p->mtx, NULL);
	pthread_mutex_init(&p->store_mtx, NULL);
	pthread_mutex_init(&p->hold_mtx, NULL);
	pthread_cond_init(&p->cv, NULL);
	gbah_fs_mkdirs(p->games_dir);
	atomic_store(&p->run_save, 1);
	if (pthread_create(&p->save_th, NULL, save_main, p) == 0) p->save_valid = 1;
	else atomic_store(&p->run_save, 0);
	return p;
}

void phone_destroy(phone_link *p)
{
	int i;
	if (!p) return;
	phone_link_stop(p);
	if (p->link_valid) pthread_join(p->link_th, NULL);
	pthread_mutex_lock(&p->mtx);
	atomic_store(&p->run_save, 0);
	pthread_cond_broadcast(&p->cv);
	pthread_mutex_unlock(&p->mtx);
	if (p->save_valid) pthread_join(p->save_th, NULL);
	for (i = 0; i < p->nconf; i++) free(p->conf[i].rthumb);
	pthread_mutex_destroy(&p->mtx); pthread_mutex_destroy(&p->store_mtx); pthread_mutex_destroy(&p->hold_mtx);
	pthread_cond_destroy(&p->cv);
	free(p);
}

void phone_link_start(phone_link *p)
{
	if (atomic_load(&p->run_link)) return;
	if (p->link_valid) { pthread_join(p->link_th, NULL); p->link_valid = 0; }   /* a previous stop() */
	atomic_store(&p->run_link, 1);
	atomic_store(&p->status, PL_ST_SEARCHING);
	if (pthread_create(&p->link_th, NULL, link_main, p) == 0) p->link_valid = 1;
	else { atomic_store(&p->run_link, 0); atomic_store(&p->status, PL_ST_STOPPED); }
}

void phone_link_stop(phone_link *p)
{
	pthread_mutex_lock(&p->mtx);
	atomic_store(&p->run_link, 0);
	if (p->sock != GBAHP_BAD_SOCK) gbahp_net_shutdown(p->sock);
	pthread_cond_broadcast(&p->cv);
	pthread_mutex_unlock(&p->mtx);
}

int phone_link_status(phone_link *p) { return atomic_load(&p->status); }

void phone_set_running(phone_link *p, int running) { atomic_store(&p->running, running ? 1 : 0); }

void phone_game_loaded(phone_link *p)
{
	pthread_mutex_lock(&p->mtx);
	p->last_valid = 0;
	p->last_sav_hash = H(p)->sav_hash(H(p)->user);
	pthread_mutex_unlock(&p->mtx);
	update_hold(p);
}

void phone_game_unloaded(phone_link *p)
{
	pthread_mutex_lock(&p->mtx);
	p->last_valid = 0;
	pthread_mutex_unlock(&p->mtx);
	pthread_mutex_lock(&p->hold_mtx);
	p->held = 0;
	pthread_mutex_unlock(&p->hold_mtx);
}

int phone_resume_stored(phone_link *p)
{
	uint8_t sha[20];
	void *st = NULL, *sv = NULL;
	size_t sn = 0, vn = 0;
	int have_state, rc = 1;
	if (!loaded_sha(p, sha)) return -1;
	pthread_mutex_lock(&p->store_mtx);
	have_state = gbah_store_read(p->games_dir, sha, &st, &sn, &sv, &vn) == 0;
	if (!have_state) {   /* a bare .sav may still exist */
		char path[700], hex[41];
		size_t n = 0;
		hex_of(sha, hex);
		snprintf(path, sizeof path, "%s/%s.sav", p->games_dir, hex);
		if (gbah_fs_read_all(path, &sv, &n) == 0 && n) vn = n; else { gbah_fs_free(sv); sv = NULL; vn = 0; }
	}
	pthread_mutex_unlock(&p->store_mtx);
	if (have_state) {
		rc = H(p)->apply(H(p)->user, sha, st, sn, sv, vn);
		if (rc != 0) {
			LOGI("stored state could not be loaded (rc=%d): cold start with the .sav", rc);
			if (sv && vn) H(p)->cold_sav(H(p)->user, sv, vn);
			rc = -2;
		}
	} else if (sv && vn) {
		rc = H(p)->cold_sav(H(p)->user, sv, vn) == 0 ? 1 : -2;
	}
	if (have_state && rc == 0) remember_saved(p, sha, H(p)->frame(H(p)->user), H(p)->sav_hash(H(p)->user));
	else {
		pthread_mutex_lock(&p->mtx);
		p->last_sav_hash = H(p)->sav_hash(H(p)->user);
		pthread_mutex_unlock(&p->mtx);
	}
	gbah_fs_free(st); gbah_fs_free(sv);
	return have_state && rc == 0 ? 0 : (rc < 0 ? rc : 1);
}

static void enqueue(phone_link *p, int type, const uint8_t *sha, int arg)
{
	pthread_mutex_lock(&p->mtx);
	if (p->ncmd < MAX_CMDS) {
		cmd_t *c = &p->cmds[p->ncmd++];
		c->type = type; c->arg = arg;
		if (sha) memcpy(c->sha1, sha, 20); else memset(c->sha1, 0, 20);
	}
	pthread_mutex_unlock(&p->mtx);
}

void phone_cmd_return(phone_link *p) { enqueue(p, CMD_RETURN, NULL, 0); }
void phone_cmd_handoff(phone_link *p) { enqueue(p, CMD_HANDOFF, NULL, 0); }
void phone_cmd_resolve(phone_link *p, const uint8_t sha1[20], int keep_phone) { enqueue(p, CMD_RESOLVE, sha1, keep_phone); }
void phone_cmd_fetch(phone_link *p, const uint8_t sha1[20]) { enqueue(p, CMD_FETCH, sha1, 0); }

/* The latest keys live in an atomic, not in the command queue: a release (keys == 0) can never be
 * dropped by a full queue. The link thread sends them (and decides whether they are allowed). */
void phone_cmd_input(phone_link *p, uint16_t keys)
{
	if (!atomic_load(&p->control_on)) return;   /* only the control mode talks to the PC */
	atomic_store(&p->latest_keys, keys);
	atomic_store(&p->keys_dirty, 1);
}

void phone_set_control(phone_link *p, int on)
{
	atomic_store(&p->control_on, on ? 1 : 0);
	if (!on) { atomic_store(&p->latest_keys, 0); atomic_store(&p->keys_dirty, 1); }   /* release what the PC holds */
}

void phone_set_prefer_newest(phone_link *p, int on) { atomic_store(&p->prefer_newest, on ? 1 : 0); }

void phone_cmd_settings_set(phone_link *p, int on)
{
	atomic_store(&p->prefer_newest, on ? 1 : 0);
	enqueue(p, CMD_SETTINGS_SET, NULL, on ? 1 : 0);
}

void phone_cmd_settings_announce(phone_link *p) { enqueue(p, CMD_SETTINGS_ANNOUNCE, NULL, 0); }

void phone_cmd_place(phone_link *p, const uint8_t sha1[20], int on_pc) { enqueue(p, CMD_PLACE, sha1, on_pc ? 1 : 0); }

/* "Jogar aqui mesmo assim": only without the cable. The game comes back from the last stored copy;
 * the PC keeps its own, and the next connection reports a conflict if both advanced. */
int phone_cmd_play_here(phone_link *p)
{
	uint8_t sha[20], cur[20];
	int had, is_cur;
	if (atomic_load(&p->status) == PL_ST_CONNECTED) return 1;
	pthread_mutex_lock(&p->mtx);
	had = p->on_pc_has;
	memcpy(sha, p->on_pc_sha, 20);
	pthread_mutex_unlock(&p->mtx);
	if (!had) return 1;
	is_cur = loaded_sha(p, cur) && !memcmp(cur, sha, 20);
	if (is_cur && apply_stored(p, sha) != 0) { notice(p, "Nao foi possivel retomar o jogo guardado"); return -1; }
	pthread_mutex_lock(&p->mtx);
	p->on_pc_has = 0;
	pthread_mutex_unlock(&p->mtx);
	update_hold(p);
	return 0;
}

void phone_ack_open(phone_link *p, const uint8_t sha1[20])
{
	pthread_mutex_lock(&p->mtx);
	if (p->open_has && !memcmp(p->open_sha, sha1, 20)) p->open_has = 0;
	pthread_mutex_unlock(&p->mtx);
}

static int dev_kind(phone_link *p, const uint8_t dev[16])
{
	if (!memcmp(dev, p->cfg.device_id, 16)) return 0;
	if (!memcmp(dev, p->peer_dev, 16)) return 1;
	return 2;
}

size_t phone_ui_text(phone_link *p, char *buf, size_t cap)
{
	size_t len = 0;
	char hex[41], hex2[41];
	int i;
	uint64_t h_ts = 0;
#define EMIT(...) do { int w_ = snprintf(buf && len < cap ? buf + len : NULL, buf && len < cap ? cap - len : 0, __VA_ARGS__); if (w_ > 0) len += (size_t)w_; } while (0)
	{   /* the stored copy's time (store_mtx must not be taken under mtx) */
		uint8_t sha[20];
		int has;
		pthread_mutex_lock(&p->mtx);
		has = p->on_pc_has;
		memcpy(sha, p->on_pc_sha, 20);
		pthread_mutex_unlock(&p->mtx);
		if (has) { gbah_meta m; store_meta_locked(p, sha, &m, NULL, NULL); h_ts = m.has_data ? m.timestamp_ms : 0; }
	}
	pthread_mutex_lock(&p->mtx);
	EMIT("S %d %s\n", atomic_load(&p->status), p->peer);
	if (atomic_load(&p->status) == PL_ST_REFUSED && p->refuse_status) EMIT("X %d %u\n", p->refuse_status, p->refuse_proto);
	if (p->pc_pref_known && p->pc_pref != (atomic_load(&p->prefer_newest) != 0)) EMIT("D %d %d\n", atomic_load(&p->prefer_newest) != 0, p->pc_pref);
	for (i = 0; i < p->nplace; i++) { hex_of(p->place[i].sha1, hex2); EMIT("Q %s %d\n", hex2, p->place[i].waiting); }
	if (p->on_pc_has) { hex_of(p->on_pc_sha, hex); EMIT("H %s %llu\n", hex, (unsigned long long)h_ts); }
	if (p->handoff_pending) { hex_of(p->handoff_sha, hex); EMIT("W %s\n", hex); }
	if (p->pc_playing) { hex_of(p->pc_sha, hex); EMIT("P 1 %s\n", hex); } else EMIT("P 0 -\n");
	if (p->open_has) { hex_of(p->open_sha, hex); EMIT("O %s\n", hex); }
	if (p->rx_active) { hex_of(p->rx_sha, hex); EMIT("T %s %d\n", hex, p->rx_pct); }
	for (i = 0; i < p->nconf; i++) {
		const conflict_t *c = &p->conf[i];
		hex_of(c->sha1, hex2);
		EMIT("C %s %llu %u %d %llu %u %d %d %d %d\n", hex2, (unsigned long long)c->local.timestamp_ms, c->local.frame,
		     dev_kind(p, c->local.version.dev), (unsigned long long)(c->have_remote ? c->remote.timestamp_ms : 0),
		     c->have_remote ? c->remote.frame : 0, c->have_remote ? dev_kind(p, c->remote.version.dev) : 1,
		     c->rthumb ? 1 : 0, c->waiting_pc, c->have_remote);
	}
	for (i = 0; i < p->nnotice; i++) EMIT("N %s\n", p->notices[i]);
	if (len < cap) p->nnotice = 0;   /* notices are consumed only if the whole text fit (a truncated read can be retried) */
	pthread_mutex_unlock(&p->mtx);
#undef EMIT
	if (buf && cap) buf[len < cap ? len : cap - 1] = 0;
	return len;
}

int phone_conflict_thumb(phone_link *p, const uint8_t sha1[20], int remote, uint8_t *out)
{
	int i, ok = 0;
	if (remote) {
		pthread_mutex_lock(&p->mtx);
		for (i = 0; i < p->nconf; i++)
			if (!memcmp(p->conf[i].sha1, sha1, 20) && p->conf[i].rthumb) { memcpy(out, p->conf[i].rthumb, PHONE_THUMB_BYTES); ok = 1; }
		pthread_mutex_unlock(&p->mtx);
	} else {
		gbah_meta m;
		uint8_t *th = NULL;
		uint32_t tl = 0;
		store_meta_locked(p, sha1, &m, &th, &tl);
		if (th && tl == PHONE_THUMB_BYTES) { memcpy(out, th, tl); ok = 1; }
		free(th);
	}
	return ok;
}

/* ------------------------------------------------------------------ migration */

typedef struct { char names[MAX_GAMES][48]; int n; } namelist;
static void nl_cb(const char *name, void *u)
{
	namelist *l = (namelist *)u;
	if (strlen(name) == 46 && !strcmp(name + 40, ".state") && l->n < MAX_GAMES) snprintf(l->names[l->n++], sizeof l->names[0], "%s", name);
}

static int copy_if_absent(const char *src, const char *dst)
{
	void *b = NULL;
	size_t n = 0;
	int rc;
	if (!gbah_fs_exists(src) || gbah_fs_exists(dst)) return 0;
	if (gbah_fs_read_all(src, &b, &n) != 0) return -1;
	rc = gbah_fs_write_atomic(dst, b, n);
	gbah_fs_free(b);
	return rc;
}

int phone_migrate_store(const char *games_dir, const char *backup_dir, const uint8_t dev[16])
{
	namelist *l = (namelist *)calloc(1, sizeof *l);
	int i, done = 0, rc = 0;
	if (!l) return -1;
	if (gbah_fs_list(games_dir, nl_cb, l) != 0) { free(l); return -1; }
	for (i = 0; i < l->n; i++) {
		char hex[41], p_state[700], p_sav[700], p_meta[700], b_state[700], b_sav[700];
		uint8_t sha[20], hdr[GBAH_META_HEADER];
		void *mb = NULL;
		size_t mn = 0;
		gbah_meta m;
		uint32_t tl;
		struct stat st;
		memcpy(hex, l->names[i], 40); hex[40] = 0;
		if (gbah_sha1_from_hex(hex, sha) != 0) continue;
		snprintf(p_state, sizeof p_state, "%s/%s.state", games_dir, hex);
		snprintf(p_sav, sizeof p_sav, "%s/%s.sav", games_dir, hex);
		snprintf(p_meta, sizeof p_meta, "%s/%s.meta", games_dir, hex);
		if (gbah_fs_read_all(p_meta, &mb, &mn) == 0) {   /* already migrated (or written by the sync engine) */
			int ok = gbah_meta_decode((const uint8_t *)mb, mn, &m, &tl) == 0;
			gbah_fs_free(mb);
			if (ok) continue;
		}
		/* first: a copy of the legacy files, kept forever (never overwritten) */
		if (gbah_fs_mkdirs(backup_dir) != 0) { rc = -1; break; }
		snprintf(b_state, sizeof b_state, "%s/%s.state", backup_dir, hex);
		snprintf(b_sav, sizeof b_sav, "%s/%s.sav", backup_dir, hex);
		if (copy_if_absent(p_state, b_state) != 0 || copy_if_absent(p_sav, b_sav) != 0) { rc = -1; break; }
		memset(&m, 0, sizeof m);
		memcpy(m.sha1, sha, 20);
		m.has_data = 1;
		memcpy(m.version.dev, dev, 16);
		m.version.counter = 1;
		m.timestamp_ms = stat(p_state, &st) == 0 ? (uint64_t)st.st_mtime * 1000u : gbah_now_ms();
		gbah_meta_encode(&m, 0, hdr);
		if (gbah_fs_write_atomic(p_meta, hdr, sizeof hdr) != 0) { rc = -1; break; }
		done++;
	}
	free(l);
	return rc < 0 ? rc : done;
}
