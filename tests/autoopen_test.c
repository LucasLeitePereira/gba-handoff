/* PC side of "open the app when the phone is plugged and the PC is playing" (real gbahandoff + fake adb).
 * Four desktops run side by side for ~12 s; each writes the fake adb's log, which is searched for
 * `shell am start -n io.gbahandoff/.MainActivity --ez autoOpen true`:
 *   A  PC playing, app installed and current, no session      -> launched EXACTLY once (not in a loop)
 *   B  PC at the menu                                          -> never (negative control: same phone, no game)
 *   C  PC playing, but the phone connects within the 3 s       -> never (the app is already open)
 *   D  PC playing, app not installed                           -> never
 * POSIX only. */
#include "gbah_core.h"
#include "gbahp_net.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define LAUNCH "FAKE123 shell am start -n io.gbahandoff/.MainActivity --ez autoOpen true"

typedef struct { pid_t pid; char adblog[300], log[300], data[300]; uint16_t port; } Run;

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

static int count_of(const char *hay, const char *needle)
{
	int n = 0;
	for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle)) n++;
	return n;
}

static void start(Run *r, const char *base, const char *name, const char *exe, const char *adb, const char *rom, int pkg)
{
	gbahp_sock probe = gbahp_net_listen_loopback(0);
	char portarg[16];
	pid_t parent = getpid();
	r->port = gbahp_net_local_port(probe);
	gbahp_net_close(probe);
	snprintf(portarg, sizeof portarg, "%u", r->port);
	snprintf(r->adblog, sizeof r->adblog, "%s/%s.adb", base, name);
	snprintf(r->log, sizeof r->log, "%s/%s.log", base, name);
	snprintf(r->data, sizeof r->data, "%s/%s.data", base, name);
	r->pid = fork();
	if (r->pid == 0) {
		char *args[16];
		int n = 0, fd;
		args[n++] = (char *)exe; args[n++] = "--port"; args[n++] = portarg; args[n++] = "--adb"; args[n++] = (char *)adb;
		args[n++] = "--software"; args[n++] = "--data-dir"; args[n++] = r->data;
		if (rom) args[n++] = (char *)rom;
		args[n] = NULL;
		setenv("ADB_FAKE_LOG", r->adblog, 1);
		if (pkg) setenv("FAKE_PKG_VERSION", "99", 1); else unsetenv("FAKE_PKG_VERSION");
		setenv("SDL_VIDEO_DRIVER", "dummy", 1);
		setenv("SDL_AUDIO_DRIVER", "dummy", 1);
		fd = open(r->log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		if (getppid() != parent) _exit(99);
		execv(exe, args);
		_exit(127);
	}
}

int main(int argc, char **argv)
{
	char base[] = "/tmp/gbah_autoopen_XXXXXX", cmd[300];
	Run a, b, c, d;
	char *la, *lb, *lc, *ld;
	gbahp_sock s = GBAHP_BAD_SOCK;
	gbahp_conn conn;
	int i;

	if (argc < 4) { fprintf(stderr, "uso: %s gbahandoff rom fake_adb\n", argv[0]); return 2; }
	signal(SIGPIPE, SIG_IGN);
	if (!mkdtemp(base)) return 2;
	setenv("XDG_DATA_HOME", base, 1);
	start(&a, base, "a", argv[1], argv[3], argv[2], 1);
	start(&b, base, "b", argv[1], argv[3], NULL, 1);
	start(&c, base, "c", argv[1], argv[3], argv[2], 1);
	start(&d, base, "d", argv[1], argv[3], argv[2], 0);

	/* C: the phone's app connects right away (HELLO) and stays */
	for (i = 0; i < 100 && s == GBAHP_BAD_SOCK; i++) {
		s = gbahp_net_connect_loopback(c.port, 300);
		if (s == GBAHP_BAD_SOCK) usleep(100000);
	}
	CHECK(s != GBAHP_BAD_SOCK);
	if (s != GBAHP_BAD_SOCK) {
		gbahp_hello h;
		uint8_t out[512];
		memset(&h, 0, sizeof h);
		h.proto_version = GBAHP_VERSION; memset(h.device_id, 0x5A, 16);
		h.core_version = gbahp_str_c(gbah_core_version()); h.name = gbahp_str_c("TestPhone");
		gbahp_conn_init(&conn, s);
		CHECK(gbahp_conn_send(&conn, out, gbahp_encode_hello(&h, out, sizeof out)) == 0);
	}
	sleep(14);   /* 3 s of waiting + up to two 2 s polls, then ~7 s more: a loop would show up */

	la = slurp(a.adblog); lb = slurp(b.adblog); lc = slurp(c.adblog); ld = slurp(d.adblog);
	CHECK(count_of(la, LAUNCH) == 1);
	CHECK(count_of(lb, LAUNCH) == 0);
	CHECK(count_of(lb, "dumpsys package") >= 1);      /* control: B did see the phone, ready, and still did not launch */
	CHECK(count_of(lc, LAUNCH) == 0);
	CHECK(count_of(ld, LAUNCH) == 0);
	CHECK(count_of(ld, "dumpsys package") >= 1);
	if (failures) fprintf(stderr, "--- A ---\n%s--- B ---\n%s--- C ---\n%s--- D ---\n%s", la, lb, lc, ld);
	kill(a.pid, SIGKILL); kill(b.pid, SIGKILL); kill(c.pid, SIGKILL); kill(d.pid, SIGKILL);
	while (waitpid(-1, NULL, WNOHANG) > 0) { }
	snprintf(cmd, sizeof cmd, "rm -rf %s", base);
	if (system(cmd) != 0) { }
	free(la); free(lb); free(lc); free(ld);
	if (failures) return 1;
	puts("autoopen_test: ok");
	return 0;
}
