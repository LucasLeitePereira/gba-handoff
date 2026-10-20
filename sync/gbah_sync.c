#include "gbah_sync.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------ versions */

int gbah_ver_none(const gbah_ver *v) { return v->counter == 0; }

int gbah_ver_eq(const gbah_ver *a, const gbah_ver *b)
{
	return a->counter == b->counter && memcmp(a->dev, b->dev, 16) == 0;
}

int gbah_sync_decide(const gbah_meta *l, const gbah_meta *r)
{
	if (!l->has_data && !r->has_data) return GBAH_SYNC_NOTHING;
	if (!l->has_data) return GBAH_SYNC_RECEIVE;
	if (!r->has_data) return GBAH_SYNC_SEND;
	if (gbah_ver_eq(&l->version, &r->version)) return GBAH_SYNC_NOTHING;
	if (!gbah_ver_none(&r->version) && gbah_ver_eq(&l->base, &r->version)) return GBAH_SYNC_SEND;
	if (!gbah_ver_none(&l->version) && gbah_ver_eq(&r->base, &l->version)) return GBAH_SYNC_RECEIVE;
	return GBAH_SYNC_CONFLICT;
}

void gbah_sync_advance(gbah_meta *m, const uint8_t dev[16], uint64_t ts_ms, uint32_t frame)
{
	memcpy(m->version.dev, dev, 16);
	m->version.counter = m->has_data ? m->version.counter + 1 : 1;
	if (!m->has_data) memset(&m->base, 0, sizeof m->base);
	m->has_data = 1;
	m->timestamp_ms = ts_ms;
	m->frame = frame;
}

void gbah_sync_mark_synced(gbah_meta *m) { m->base = m->version; }

void gbah_sync_adopt(gbah_meta *local, const gbah_meta *in)
{
	*local = *in;
	local->has_data = 1;
	local->base = in->version;
}

/* ------------------------------------------------------------ meta codec */

static void p32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void p64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t g32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint64_t g64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

size_t gbah_meta_encode(const gbah_meta *m, uint32_t thumb_len, uint8_t out[GBAH_META_HEADER])
{
	memset(out, 0, GBAH_META_HEADER);
	memcpy(out, "GBAM", 4);
	out[4] = 1;                                   /* format */
	out[6] = (uint8_t)(m->has_data ? 1 : 0);
	memcpy(out + 8, m->sha1, 20);
	memcpy(out + 28, m->version.dev, 16);
	p64(out + 44, m->version.counter);
	memcpy(out + 52, m->base.dev, 16);
	p64(out + 68, m->base.counter);
	p64(out + 76, m->timestamp_ms);
	p32(out + 84, m->frame);
	p32(out + 88, thumb_len);
	return GBAH_META_HEADER;
}

int gbah_meta_decode(const uint8_t *b, size_t n, gbah_meta *m, uint32_t *thumb_len)
{
	if (n < GBAH_META_HEADER || memcmp(b, "GBAM", 4) != 0 || b[4] != 1) return -1;
	memset(m, 0, sizeof *m);
	m->has_data = b[6] & 1;
	memcpy(m->sha1, b + 8, 20);
	memcpy(m->version.dev, b + 28, 16);
	m->version.counter = g64(b + 44);
	memcpy(m->base.dev, b + 52, 16);
	m->base.counter = g64(b + 68);
	m->timestamp_ms = g64(b + 76);
	m->frame = g32(b + 84);
	if (thumb_len) *thumb_len = g32(b + 88);
	if (m->has_data && m->version.counter == 0) return -1;
	if (g32(b + 88) > GBAH_THUMB_BYTES) return -1;
	return 0;
}

/* ------------------------------------------------------------ filesystem */

#ifdef _WIN32
static wchar_t *to_wide(const char *u8)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, u8, -1, NULL, 0);
	wchar_t *w;
	if (n <= 0) return NULL;
	w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
	if (w) MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, n);
	return w;
}
static char *to_utf8(const wchar_t *w)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	char *s;
	if (n <= 0) return NULL;
	s = (char *)malloc((size_t)n);
	if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
	return s;
}
#endif

static FILE *fopen_u(const char *path, const char *mode)
{
#ifdef _WIN32
	wchar_t *w = to_wide(path), wm[8];
	FILE *f;
	size_t i;
	if (!w) return NULL;
	for (i = 0; mode[i] && i < 7; i++) wm[i] = (wchar_t)mode[i];
	wm[i] = 0;
	f = _wfopen(w, wm);
	free(w);
	return f;
#else
	return fopen(path, mode);
#endif
}

static int rename_u(const char *from, const char *to)
{
#ifdef _WIN32
	wchar_t *a = to_wide(from), *b = to_wide(to);
	int ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	free(a); free(b);
	return ok ? 0 : -1;
#else
	return rename(from, to);
#endif
}

int gbah_fs_remove(const char *path)
{
#ifdef _WIN32
	wchar_t *w = to_wide(path);
	int ok;
	if (!w) return -1;
	ok = DeleteFileW(w) || GetLastError() == ERROR_FILE_NOT_FOUND;
	free(w);
	return ok ? 0 : -1;
#else
	return (unlink(path) == 0 || errno == ENOENT) ? 0 : -1;
#endif
}

int gbah_fs_exists(const char *path)
{
	FILE *f = fopen_u(path, "rb");
	if (!f) return 0;
	fclose(f);
	return 1;
}

int gbah_fs_size(const char *path, uint64_t *size)
{
	FILE *f = fopen_u(path, "rb");
	long long n;
	if (!f) return -1;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
	n = (long long)ftell(f);
	fclose(f);
	if (n < 0) return -1;
	*size = (uint64_t)n;
	return 0;
}

void gbah_fs_free(void *p) { free(p); }

int gbah_fs_read_all(const char *path, void **out, size_t *n)
{
	FILE *f = fopen_u(path, "rb");
	long sz;
	void *b;
	*out = NULL; *n = 0;
	if (!f) return -1;
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
	b = malloc(sz ? (size_t)sz : 1);
	if (!b) { fclose(f); return -1; }
	if (sz && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return -1; }
	fclose(f);
	*out = b; *n = (size_t)sz;
	return 0;
}

int gbah_fs_read_prefix(const char *path, void *buf, size_t n, size_t *got)
{
	FILE *f = fopen_u(path, "rb");
	size_t r;
	if (got) *got = 0;
	if (!f) return -1;
	r = fread(buf, 1, n, f);
	fclose(f);
	if (got) *got = r;
	return 0;
}

int gbah_fs_write_atomic(const char *path, const void *data, size_t n)
{
	size_t pl = strlen(path);
	char *tmp = (char *)malloc(pl + 5);
	FILE *f;
	int ok;
	if (!tmp) return -1;
	memcpy(tmp, path, pl); memcpy(tmp + pl, ".tmp", 5);
	f = fopen_u(tmp, "wb");
	if (!f) { free(tmp); return -1; }
	ok = (n == 0 || fwrite(data, 1, n, f) == n) && fflush(f) == 0;
#ifdef _WIN32
	if (ok) ok = _commit(_fileno(f)) == 0;
#else
	if (ok) ok = fsync(fileno(f)) == 0;
#endif
	ok = (fclose(f) == 0) && ok;
	if (ok) ok = rename_u(tmp, path) == 0;
	if (!ok) gbah_fs_remove(tmp);
	free(tmp);
	return ok ? 0 : -1;
}

int gbah_fs_mkdirs(const char *dir)
{
	size_t n = strlen(dir), i;
	char *p = (char *)malloc(n + 2);
	int rc = 0;
	if (!p) return -1;
	memcpy(p, dir, n + 1);
	for (i = 1; i <= n; i++) {
		if (p[i] == '/' || p[i] == '\\' || p[i] == 0) {
			char c = p[i];
			p[i] = 0;
#ifdef _WIN32
			{
				wchar_t *w = to_wide(p);
				if (w && !(i == 2 && p[1] == ':')) CreateDirectoryW(w, NULL);
				free(w);
			}
#else
			mkdir(p, 0700);
#endif
			p[i] = c;
		}
	}
	{   /* verify the directory is really usable by writing a probe file */
		char *probe = (char *)malloc(n + 8);
		if (probe) {
			memcpy(probe, dir, n); memcpy(probe + n, "/.probe", 8);
			if (gbah_fs_write_atomic(probe, "", 0) != 0) rc = -1;
			else gbah_fs_remove(probe);
			free(probe);
		}
	}
	free(p);
	return rc;
}

uint64_t gbah_now_ms(void)
{
#ifdef _WIN32
	FILETIME ft;
	ULARGE_INTEGER u;
	GetSystemTimeAsFileTime(&ft);
	u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
	return (u.QuadPart - 116444736000000000ULL) / 10000ULL;
#else
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

int gbah_fs_list(const char *dir, void (*cb)(const char *name, void *u), void *u)
{
#ifdef _WIN32
	size_t n = strlen(dir);
	char *pat = (char *)malloc(n + 3);
	wchar_t *w;
	WIN32_FIND_DATAW fd;
	HANDLE h;
	if (!pat) return -1;
	memcpy(pat, dir, n); memcpy(pat + n, "\\*", 3);
	w = to_wide(pat);
	free(pat);
	if (!w) return -1;
	h = FindFirstFileW(w, &fd);
	free(w);
	if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
	do {
		char *s = to_utf8(fd.cFileName);
		if (s) { if (strcmp(s, ".") && strcmp(s, "..")) cb(s, u); free(s); }
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return 0;
#else
	DIR *d = opendir(dir);
	struct dirent *e;
	if (!d) return errno == ENOENT ? 0 : -1;
	while ((e = readdir(d)) != NULL)
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) cb(e->d_name, u);
	closedir(d);
	return 0;
#endif
}

/* ------------------------------------------------------------ store */

void gbah_sha1_hex(const uint8_t s[20], char out[41])
{
	static const char hx[] = "0123456789abcdef";
	for (int i = 0; i < 20; i++) { out[2 * i] = hx[s[i] >> 4]; out[2 * i + 1] = hx[s[i] & 15]; }
	out[40] = 0;
}

static int hexv(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

int gbah_sha1_from_hex(const char *h, uint8_t out[20])
{
	for (int i = 0; i < 20; i++) {
		int a = hexv(h[2 * i]), b = a < 0 ? -1 : hexv(h[2 * i + 1]);
		if (a < 0 || b < 0) return -1;
		out[i] = (uint8_t)(a * 16 + b);
	}
	return 0;
}

static void path_of(char *out, size_t cap, const char *dir, const uint8_t sha1[20], const char *suffix)
{
	char hex[41];
	gbah_sha1_hex(sha1, hex);
	snprintf(out, cap, "%s/%s%s", dir, hex, suffix);
}

int gbah_store_meta(const char *dir, const uint8_t sha1[20], const uint8_t dev_self[16],
                    gbah_meta *m, uint8_t **thumb, uint32_t *thumb_len)
{
	char p[1100];
	void *buf = NULL;
	size_t n = 0;
	uint32_t tl = 0;
	if (thumb) *thumb = NULL;
	if (thumb_len) *thumb_len = 0;
	memset(m, 0, sizeof *m);
	memcpy(m->sha1, sha1, 20);
	path_of(p, sizeof p, dir, sha1, ".meta");
	if (gbah_fs_read_all(p, &buf, &n) == 0) {
		int r = gbah_meta_decode((const uint8_t *)buf, n, m, &tl);
		if (r == 0) {
			memcpy(m->sha1, sha1, 20);
			if (thumb && tl && n >= GBAH_META_HEADER + tl) {
				*thumb = (uint8_t *)malloc(tl);
				if (*thumb) { memcpy(*thumb, (uint8_t *)buf + GBAH_META_HEADER, tl); if (thumb_len) *thumb_len = tl; }
			}
			gbah_fs_free(buf);
			return m->has_data ? 0 : 1;
		}
		gbah_fs_free(buf);
		/* corrupt meta: fall through to the legacy rule if a state exists */
	}
	path_of(p, sizeof p, dir, sha1, ".state");
	if (gbah_fs_exists(p)) {
		m->has_data = 1;
		memcpy(m->version.dev, dev_self, 16);
		m->version.counter = 1;
		return 0;
	}
	return 1;
}

int gbah_store_commit(const char *dir, const gbah_meta *m, const void *state, size_t state_len,
                      const void *sav, size_t sav_len, const void *thumb, uint32_t thumb_len)
{
	char p[1100];
	uint8_t *mb;
	int rc;
	if (!state || !state_len || thumb_len > GBAH_THUMB_BYTES || (thumb_len && !thumb)) return -1;
	if (gbah_fs_mkdirs(dir) != 0) return -1;
	path_of(p, sizeof p, dir, m->sha1, ".state");
	if (gbah_fs_write_atomic(p, state, state_len) != 0) return -1;
	path_of(p, sizeof p, dir, m->sha1, ".sav");
	if (sav_len) { if (gbah_fs_write_atomic(p, sav, sav_len) != 0) return -1; }
	else if (gbah_fs_remove(p) != 0) return -1;
	mb = (uint8_t *)malloc(GBAH_META_HEADER + thumb_len);
	if (!mb) return -1;
	gbah_meta_encode(m, thumb_len, mb);
	if (thumb_len) memcpy(mb + GBAH_META_HEADER, thumb, thumb_len);
	path_of(p, sizeof p, dir, m->sha1, ".meta");
	rc = gbah_fs_write_atomic(p, mb, GBAH_META_HEADER + thumb_len);
	free(mb);
	return rc;
}

int gbah_store_read(const char *dir, const uint8_t sha1[20], void **state, size_t *state_len,
                    void **sav, size_t *sav_len)
{
	char p[1100];
	*state = NULL; *state_len = 0;
	if (sav) *sav = NULL;
	if (sav_len) *sav_len = 0;
	path_of(p, sizeof p, dir, sha1, ".state");
	if (gbah_fs_read_all(p, state, state_len) != 0 || *state_len == 0) { gbah_fs_free(*state); *state = NULL; return -1; }
	if (sav) {
		size_t n = 0;
		path_of(p, sizeof p, dir, sha1, ".sav");
		if (gbah_fs_read_all(p, sav, &n) != 0) { *sav = NULL; n = 0; }
		else if (n == 0) { gbah_fs_free(*sav); *sav = NULL; }
		if (sav_len) *sav_len = n;
	}
	return 0;
}

int gbah_store_mark_synced(const char *dir, const uint8_t sha1[20], const gbah_ver *v)
{
	char p[1100];
	void *buf = NULL;
	size_t n = 0;
	gbah_meta m;
	uint32_t tl = 0;
	int rc;
	path_of(p, sizeof p, dir, sha1, ".meta");
	if (gbah_fs_read_all(p, &buf, &n) != 0) return -1;
	if (gbah_meta_decode((const uint8_t *)buf, n, &m, &tl) != 0 || !m.has_data) { gbah_fs_free(buf); return -1; }
	if (!gbah_ver_eq(&m.version, v)) { gbah_fs_free(buf); return 1; }
	m.base = m.version;
	gbah_meta_encode(&m, tl, (uint8_t *)buf);   /* rewrite header in place, keep the thumbnail */
	memcpy(((uint8_t *)buf) + 8, sha1, 20);
	rc = gbah_fs_write_atomic(p, buf, n);
	gbah_fs_free(buf);
	return rc;
}

/* ---- backups ---- */

typedef struct {
	char prefix[64];     /* "<sha1>.backup-" */
	uint64_t ts[64];
	int count;
} bk_list;

static void bk_cb(const char *name, void *u)
{
	bk_list *l = (bk_list *)u;
	size_t pl = strlen(l->prefix);
	uint64_t v = 0;
	const char *p;
	int i;
	if (strncmp(name, l->prefix, pl) != 0) return;
	p = name + pl;
	if (*p < '0' || *p > '9') return;
	while (*p >= '0' && *p <= '9') v = v * 10 + (uint64_t)(*p++ - '0');
	if (strcmp(p, ".state") && strcmp(p, ".sav") && strcmp(p, ".meta")) return;
	for (i = 0; i < l->count; i++) if (l->ts[i] == v) return;
	if (l->count < 64) l->ts[l->count++] = v;
}

static int bk_collect(const char *dir, const uint8_t sha1[20], bk_list *l)
{
	char hex[41];
	int i, j;
	gbah_sha1_hex(sha1, hex);
	snprintf(l->prefix, sizeof l->prefix, "%s.backup-", hex);
	l->count = 0;
	if (gbah_fs_list(dir, bk_cb, l) != 0) return -1;
	for (i = 0; i < l->count; i++)            /* newest first */
		for (j = i + 1; j < l->count; j++)
			if (l->ts[j] > l->ts[i]) { uint64_t t = l->ts[i]; l->ts[i] = l->ts[j]; l->ts[j] = t; }
	return 0;
}

int gbah_store_list_backups(const char *dir, const uint8_t sha1[20], uint64_t *ts, int max)
{
	bk_list l;
	int i;
	if (bk_collect(dir, sha1, &l) != 0) return -1;
	for (i = 0; i < l.count && i < max; i++) ts[i] = l.ts[i];
	return l.count;
}

int gbah_store_backup(const char *dir, const uint8_t sha1[20], uint64_t ts_ms, int keep)
{
	static const char *const ext[3] = { ".state", ".sav", ".meta" };
	char from[1100], to[1200], suffix[48];
	bk_list l;
	int i, e;
	/* a backup with the same timestamp must not clobber an existing one */
	if (bk_collect(dir, sha1, &l) != 0) return -1;
	for (i = 0; i < l.count; i++) if (l.ts[i] == ts_ms) ts_ms++;
	for (e = 0; e < 3; e++) {
		path_of(from, sizeof from, dir, sha1, ext[e]);
		if (!gbah_fs_exists(from)) continue;
		snprintf(suffix, sizeof suffix, ".backup-%llu%s", (unsigned long long)ts_ms, ext[e]);
		path_of(to, sizeof to, dir, sha1, suffix);
		if (rename_u(from, to) != 0) return -1;
	}
	if (bk_collect(dir, sha1, &l) != 0) return -1;
	for (i = keep < 0 ? 0 : keep; i < l.count; i++)
		for (e = 0; e < 3; e++) {
			snprintf(suffix, sizeof suffix, ".backup-%llu%s", (unsigned long long)l.ts[i], ext[e]);
			path_of(to, sizeof to, dir, sha1, suffix);
			gbah_fs_remove(to);
		}
	return 0;
}

typedef struct {
	void (*cb)(const uint8_t sha1[20], void *user);
	void *user;
	uint8_t seen[256][20];
	int n;
} lst;

static void list_cb(const char *name, void *u)
{
	lst *l = (lst *)u;
	uint8_t s[20];
	int i;
	if (strlen(name) < 40 || gbah_sha1_from_hex(name, s) != 0) return;
	if (strcmp(name + 40, ".meta") && strcmp(name + 40, ".state")) return;
	for (i = 0; i < l->n; i++) if (!memcmp(l->seen[i], s, 20)) return;
	if (l->n < 256) memcpy(l->seen[l->n++], s, 20);
	l->cb(s, l->user);
}

int gbah_store_list(const char *dir, void (*cb)(const uint8_t sha1[20], void *user), void *user)
{
	lst *l = (lst *)calloc(1, sizeof *l);
	int rc;
	if (!l) return -1;
	l->cb = cb; l->user = user;
	rc = gbah_fs_list(dir, list_cb, l);
	free(l);
	return rc;
}
