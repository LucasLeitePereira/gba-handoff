/* Desktop server <-> simulated phone, over real TCP on 127.0.0.1.
 * Starts the real `gbahandoff` binary (with a fake adb) and plays the phone side:
 *   session 1: HELLO with another mGBA version -> must be refused (ACK core mismatch + close)
 *   session 2: HELLO -> HELLO_ACK(ok) -> LIBRARY both ways -> INPUT -> PING/PONG -> BYE
 * then checks the desktop log (status "conectado: ...") and that the adb manager
 * ran `adb -s FAKE123 reverse tcp:PORT tcp:PORT` (and ignored the unauthorized one).
 * POSIX only (fork/exec); the Windows build is cross-compiled and not run here. */
#include "gbah_core.h"
#include "gbahp_net.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>


static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

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

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

static gbahp_sock connect_retry(uint16_t port, int seconds)
{
	time_t end = time(NULL) + seconds;
	while (time(NULL) < end) {
		gbahp_sock s = gbahp_net_connect_loopback(port, 300);
		if (s != GBAHP_BAD_SOCK) return s;
		usleep(100000);
	}
	return GBAHP_BAD_SOCK;
}

static void do_hello(gbahp_conn *c, const char *core, gbahp_hello_ack *ack, uint8_t *keep, size_t keep_cap)
{
	gbahp_hello h;
	gbahp_frame f;
	uint8_t out[512];
	memset(&h, 0, sizeof h);
	h.proto_version = GBAHP_VERSION; memset(h.device_id, 0x5A, 16);
	h.core_version = gbahp_str_c(core); h.name = gbahp_str_c("TestPhone");
	CHECK(gbahp_conn_send(c, out, gbahp_encode_hello(&h, out, sizeof out)) == 0);
	CHECK(gbahp_conn_recv(c, &f, 5000) == 1 && f.type == GBAHP_MSG_HELLO_ACK);
	CHECK(gbahp_decode_hello_ack(f.payload, f.len, ack) == 0);
	/* the ack's string views die with the next recv: keep a copy of the name */
	if (keep_cap && ack->hello.name.n < keep_cap) {
		memcpy(keep, ack->hello.name.p, ack->hello.name.n);
		keep[ack->hello.name.n] = 0;
	}
}

int main(int argc, char **argv)
{
	char dir[] = "/tmp/gbah_link_XXXXXX";
	char logp[256], adblog[256], portarg[16], cmd_log[256], xdg[256];
	const char *exe, *rom, *sha_hex, *fake_adb;
	uint8_t want_sha[20];
	uint16_t port;
	gbahp_sock probe;
	pid_t pid;
	int i, status = 0;

	if (argc < 5) { fprintf(stderr, "usage: %s gbahandoff rom sha1hex fake_adb\n", argv[0]); return 2; }
	exe = argv[1]; rom = argv[2]; sha_hex = argv[3]; fake_adb = argv[4];
	for (i = 0; i < 20; i++) want_sha[i] = (uint8_t)(hexval(sha_hex[2 * i]) * 16 + hexval(sha_hex[2 * i + 1]));
	signal(SIGPIPE, SIG_IGN);
	if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
	snprintf(logp, sizeof logp, "%s/desktop.log", dir);
	snprintf(adblog, sizeof adblog, "%s/adb.log", dir);
	snprintf(xdg, sizeof xdg, "%s/xdg", dir);
	snprintf(cmd_log, sizeof cmd_log, "rm -rf %s", dir);

	/* free port */
	probe = gbahp_net_listen_loopback(0);
	port = gbahp_net_local_port(probe);
	gbahp_net_close(probe);
	snprintf(portarg, sizeof portarg, "%u", port);

	setenv("ADB_FAKE_LOG", adblog, 1);
	setenv("SDL_VIDEO_DRIVER", "dummy", 1);   /* no display in CI / WSL */
	setenv("SDL_AUDIO_DRIVER", "dummy", 1);
	setenv("XDG_DATA_HOME", xdg, 1);   /* device_id file goes to the temp dir, not to $HOME */
	{
		char *args[] = { (char *)exe, "--exit-after-sessions", "2", "--port", portarg, "--adb", (char *)fake_adb,
		                 "--software", (char *)rom, NULL };
		pid_t parent = getpid();
		pid = fork();
		if (pid < 0) { perror("fork"); return 2; }
		if (pid == 0) {   /* dies with this test (ctest timeout) instead of becoming an orphan */
			int fd = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
			if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
			prctl(PR_SET_PDEATHSIG, SIGKILL);
			if (getppid() != parent) _exit(99);
			execv(exe, args);
			_exit(127);
		}
	}

	/* session 1: wrong mGBA version */
	{
		gbahp_sock s = connect_retry(port, 20);
		gbahp_conn c;
		gbahp_hello_ack ack;
		gbahp_frame f;
		uint8_t nm[300];
		CHECK(s != GBAHP_BAD_SOCK);
		if (s != GBAHP_BAD_SOCK) {
			gbahp_conn_init(&c, s);
			do_hello(&c, "0.0.0-incompatible", &ack, nm, sizeof nm);
			CHECK(ack.status == GBAHP_ACK_CORE_MISMATCH);
			CHECK(!strcmp((const char *)nm, nm[0] ? (const char *)nm : "x"));   /* server named itself */
			CHECK(gbahp_conn_recv(&c, &f, 5000) == -1);   /* closed by the server */
			gbahp_conn_close(&c);
		}
	}
	/* session 2: full handshake */
	{
		gbahp_sock s = connect_retry(port, 20);
		gbahp_conn c;
		gbahp_hello_ack ack;
		gbahp_frame f;
		uint8_t out[512], nm[300];
		CHECK(s != GBAHP_BAD_SOCK);
		if (s != GBAHP_BAD_SOCK) {
			gbahp_conn_init(&c, s);
			do_hello(&c, gbah_core_version(), &ack, nm, sizeof nm);
			CHECK(ack.status == GBAHP_ACK_OK);
			CHECK(ack.hello.proto_version == GBAHP_VERSION);
			CHECK(ack.hello.core_version.n == strlen(gbah_core_version()));
			{ /* server LIBRARY: our ROM, identified by the SHA-1 given by CMake */
				gbahp_library l; gbahp_lib_entry e;
				int got_settings = 0, r, k = 0;
				do {   /* v3: the PC announces its settings (CONTROL SETTINGS) before the LIBRARY */
					r = gbahp_conn_recv(&c, &f, 5000);
					if (r == 1 && f.type == GBAHP_MSG_CONTROL) {
						gbahp_control cc;
						if (gbahp_decode_control(f.payload, f.len, &cc) == 0 && cc.cmd == GBAHP_CTL_SETTINGS) got_settings = 1;
					}
				} while (r == 1 && f.type != GBAHP_MSG_LIBRARY && ++k < 10);
				CHECK(got_settings);
				CHECK(r == 1 && f.type == GBAHP_MSG_LIBRARY);
				CHECK(gbahp_decode_library(f.payload, f.len, &l) == 0 && l.count == 1);
				if (l.count == 1) {
					gbahp_library_get(&l, 0, &e);
					CHECK(!memcmp(e.sha1, want_sha, 20) && e.size > 0);
				}
			}
			{ /* phone LIBRARY (empty), INPUT, PING */
				gbahp_lib_entry none; gbahp_input in = { 1, 0x0009 }; gbahp_ping p = { 777 }, q;
				CHECK(gbahp_conn_send(&c, out, gbahp_encode_library(&none, 0, out, sizeof out)) == 0);
				CHECK(gbahp_conn_send(&c, out, gbahp_encode_input(&in, out, sizeof out)) == 0);
				CHECK(gbahp_conn_send(&c, out, gbahp_encode_ping(&p, out, sizeof out)) == 0);
				{   /* the PC also sends CONTROL(PC_STATUS) etc.: skip until the PONG */
					int r, k = 0;
					do { r = gbahp_conn_recv(&c, &f, 5000); } while (r == 1 && f.type != GBAHP_MSG_PONG && ++k < 20);
					CHECK(r == 1 && f.type == GBAHP_MSG_PONG);
				}
				CHECK(gbahp_decode_ping(f.payload, f.len, &q) == 0 && q.token == 777);
			}
			{ gbahp_bye b = { 0 }; CHECK(gbahp_conn_send(&c, out, gbahp_encode_bye(&b, out, sizeof out)) == 0); }
			gbahp_conn_close(&c);
		}
	}

	/* wait for the desktop to exit by itself (--exit-after-sessions 2) */
	for (i = 0; i < 150; i++) {
		pid_t r = waitpid(pid, &status, WNOHANG);
		if (r == pid) break;
		usleep(100000);
	}
	if (i == 150) { kill(pid, SIGKILL); waitpid(pid, &status, 0); CHECK(!"desktop did not exit"); }
	else CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	{
		char *log = slurp(logp), *ad = slurp(adblog), want[128];
		CHECK(strstr(log, "conectado: TestPhone") != NULL);
		CHECK(strstr(log, "refused 'TestPhone'") != NULL);
		CHECK(strstr(log, "desconectado") != NULL);
		snprintf(want, sizeof want, "FAKE123 reverse tcp:%u tcp:%u", port, port);
		CHECK(strstr(ad, want) != NULL);
		CHECK(strstr(ad, "UNAUTH9") == NULL);   /* unauthorized devices are not touched */
		if (failures) fprintf(stderr, "--- desktop log ---\n%s--- fake adb log ---\n%s", log, ad);
		free(log); free(ad);
	}
	if (system(cmd_log) != 0) { /* best effort cleanup */ }
	if (failures) return 1;
	puts("link_integration_test: ok");
	return 0;
}
