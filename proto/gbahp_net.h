/*
 * gbahp_net - minimal blocking TCP helpers (POSIX + Winsock) shared by the
 * desktop server, the Android client and the tests. Loopback only: the PC
 * listens on 127.0.0.1 and `adb reverse` forwards the phone's 127.0.0.1:PORT
 * to it, so nothing is ever exposed to the network.
 * All waits are bounded by a timeout so threads can notice a stop flag.
 */
#ifndef GBAHP_NET_H
#define GBAHP_NET_H

#include "gbahp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef intptr_t gbahp_sock;
#define GBAHP_BAD_SOCK ((gbahp_sock)-1)

/* Winsock startup (no-op elsewhere). Safe to call many times. 0 = ok. */
int gbahp_net_init(void);

/* Listen on 127.0.0.1:port (0 = pick a free port; see gbahp_net_local_port). */
gbahp_sock gbahp_net_listen_loopback(uint16_t port);
uint16_t gbahp_net_local_port(gbahp_sock s);
/* Accept with timeout. Returns the new socket (TCP_NODELAY on), or
 * GBAHP_BAD_SOCK with *timed_out = 1 on timeout / 0 on a real error. */
gbahp_sock gbahp_net_accept(gbahp_sock listener, int timeout_ms, int *timed_out);
/* Connect to 127.0.0.1:port with a timeout (TCP_NODELAY on). */
gbahp_sock gbahp_net_connect_loopback(uint16_t port, int timeout_ms);
void gbahp_net_shutdown(gbahp_sock s);   /* wakes a thread blocked on s */
void gbahp_net_close(gbahp_sock s);

/* A connection = socket + incremental parser + read-ahead buffer. */
typedef struct {
	gbahp_sock s;
	gbahp_parser parser;
	uint8_t rbuf[4096];
	size_t rpos, rlen;
	int proto_error;     /* GBAHP_E_* of the last recv returning -2 */
} gbahp_conn;

void gbahp_conn_init(gbahp_conn *c, gbahp_sock s);
void gbahp_conn_close(gbahp_conn *c);                       /* closes the socket, frees the parser */
int gbahp_conn_send(gbahp_conn *c, const void *data, size_t n);   /* 0 ok, -1 error */
/* Same on a bare socket: lets a writer thread send while another thread owns the gbahp_conn
 * (one writer at a time; reading and writing one socket from two threads is fine). */
int gbahp_net_send(gbahp_sock s, const void *data, size_t n);
/* 1 = frame ready (valid until the next recv), 0 = timeout, -1 = closed/socket
 * error, -2 = protocol error (c->proto_error; drop the connection). */
int gbahp_conn_recv(gbahp_conn *c, gbahp_frame *f, int timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
