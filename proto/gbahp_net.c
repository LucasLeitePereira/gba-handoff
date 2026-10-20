#include "gbahp_net.h"

#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_compat;
#define SOCK_ERR_WOULDBLOCK (WSAGetLastError() == WSAEWOULDBLOCK)
#define POLL(f, n, t) WSAPoll((f), (n), (t))
#define SEND_FLAGS 0
static void set_nonblock(SOCKET s, int on) { u_long v = on; ioctlsocket(s, FIONBIO, &v); }
static void close_raw(SOCKET s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
typedef socklen_t socklen_compat;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define SOCK_ERR_WOULDBLOCK (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN)
#define POLL(f, n, t) poll((f), (n), (t))
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif
static void set_nonblock(int s, int on)
{
	int fl = fcntl(s, F_GETFL, 0);
	fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}
static void close_raw(int s) { close(s); }
#endif

#ifdef _WIN32
typedef WSAPOLLFD pollfd_t;
#else
typedef struct pollfd pollfd_t;
#endif

#define S(x) ((SOCKET)(x))

int gbahp_net_init(void)
{
#ifdef _WIN32
	WSADATA d;
	return WSAStartup(MAKEWORD(2, 2), &d) == 0 ? 0 : -1;   /* refcounted by Winsock */
#else
	return 0;
#endif
}

/* Never leak sockets into child processes (adb daemon!): it would keep the port busy. */
static void no_inherit(SOCKET s)
{
#ifdef _WIN32
	SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
#else
	fcntl(s, F_SETFD, fcntl(s, F_GETFD, 0) | FD_CLOEXEC);
#endif
}

static void tune(SOCKET s)
{
	no_inherit(s);
	int one = 1;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
#ifdef SO_NOSIGPIPE
	setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&one, sizeof one);
#endif
}

static struct sockaddr_in loopback(uint16_t port)
{
	struct sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	return a;
}

gbahp_sock gbahp_net_listen_loopback(uint16_t port)
{
	struct sockaddr_in a = loopback(port);
	SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == INVALID_SOCKET) return GBAHP_BAD_SOCK;
	no_inherit(s);
#ifndef _WIN32
	{ /* not on Windows: there SO_REUSEADDR lets another process steal the port */
		int one = 1;
		setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
	}
#endif
	if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 4) != 0) {
		close_raw(s);
		return GBAHP_BAD_SOCK;
	}
	return (gbahp_sock)s;
}

uint16_t gbahp_net_local_port(gbahp_sock s)
{
	struct sockaddr_in a;
	socklen_compat n = sizeof a;
	if (getsockname(S(s), (struct sockaddr *)&a, &n) != 0) return 0;
	return ntohs(a.sin_port);
}

gbahp_sock gbahp_net_accept(gbahp_sock l, int timeout_ms, int *timed_out)
{
	pollfd_t p;
	int r;
	SOCKET c;
	if (timed_out) *timed_out = 0;
	p.fd = S(l); p.events = POLLIN; p.revents = 0;
	r = POLL(&p, 1, timeout_ms);
	if (r == 0) { if (timed_out) *timed_out = 1; return GBAHP_BAD_SOCK; }
	if (r < 0) {
#ifndef _WIN32
		if (errno == EINTR) { if (timed_out) *timed_out = 1; return GBAHP_BAD_SOCK; }
#endif
		return GBAHP_BAD_SOCK;
	}
	c = accept(S(l), NULL, NULL);
	if (c == INVALID_SOCKET) return GBAHP_BAD_SOCK;
	tune(c);
	return (gbahp_sock)c;
}

gbahp_sock gbahp_net_connect_loopback(uint16_t port, int timeout_ms)
{
	struct sockaddr_in a = loopback(port);
	SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
	int r;
	if (s == INVALID_SOCKET) return GBAHP_BAD_SOCK;
	set_nonblock(s, 1);
	r = connect(s, (struct sockaddr *)&a, sizeof a);
	if (r != 0) {
		pollfd_t p;
		int err = 0;
		socklen_compat n = sizeof err;
		if (!SOCK_ERR_WOULDBLOCK) { close_raw(s); return GBAHP_BAD_SOCK; }
		p.fd = s; p.events = POLLOUT; p.revents = 0;
		if (POLL(&p, 1, timeout_ms) <= 0 ||
		    getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &n) != 0 || err != 0) {
			close_raw(s);
			return GBAHP_BAD_SOCK;
		}
	}
	set_nonblock(s, 0);
	tune(s);
	return (gbahp_sock)s;
}

void gbahp_net_shutdown(gbahp_sock s)
{
#ifdef _WIN32
	shutdown(S(s), SD_BOTH);
#else
	shutdown(S(s), SHUT_RDWR);
#endif
}

void gbahp_net_close(gbahp_sock s) { if (s != GBAHP_BAD_SOCK) close_raw(S(s)); }

/* ------------------------------------------------------------ connection */

void gbahp_conn_init(gbahp_conn *c, gbahp_sock s)
{
	memset(c, 0, sizeof *c);
	c->s = s;
	gbahp_parser_init(&c->parser);
}

void gbahp_conn_close(gbahp_conn *c)
{
	gbahp_net_close(c->s);
	c->s = GBAHP_BAD_SOCK;
	gbahp_parser_free(&c->parser);
}

int gbahp_conn_send(gbahp_conn *c, const void *data, size_t n)
{
	const char *p = (const char *)data;
	while (n) {
		int r = (int)send(S(c->s), p, n > 65536 ? 65536 : (int)n, SEND_FLAGS);
		if (r <= 0) {
#ifndef _WIN32
			if (r < 0 && errno == EINTR) continue;
#endif
			return -1;
		}
		p += r;
		n -= (size_t)r;
	}
	return 0;
}

int gbahp_conn_recv(gbahp_conn *c, gbahp_frame *f, int timeout_ms)
{
	for (;;) {
		int r;
		if (c->rpos < c->rlen) {
			size_t used = 0;
			r = gbahp_parser_feed(&c->parser, c->rbuf + c->rpos, c->rlen - c->rpos, &used, f);
			c->rpos += used;
			if (r < 0) { c->proto_error = r; return -2; }
			if (r == 1) return 1;
		}
		{
			pollfd_t p;
			int got;
			p.fd = S(c->s); p.events = POLLIN; p.revents = 0;
			r = POLL(&p, 1, timeout_ms);
			if (r == 0) return 0;
			if (r < 0) {
#ifndef _WIN32
				if (errno == EINTR) return 0;
#endif
				return -1;
			}
			got = (int)recv(S(c->s), (char *)c->rbuf, sizeof c->rbuf, 0);
			if (got <= 0) {
#ifndef _WIN32
				if (got < 0 && errno == EINTR) return 0;
#endif
				return -1;
			}
			c->rpos = 0;
			c->rlen = (size_t)got;
		}
	}
}
