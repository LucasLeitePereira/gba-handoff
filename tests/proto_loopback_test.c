/* Real TCP loopback: server and client in threads speaking gbahp over 127.0.0.1.
 * HELLO -> HELLO_ACK -> LIBRARY <-> LIBRARY -> STATE_PUSH/ACK -> PING/PONG -> BYE,
 * plus a second session where the core versions differ and the server must refuse. */
#include "gbahp_net.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static const uint8_t SERVER_ID[16] = { 0xAA, 1 };
static const uint8_t CLIENT_ID[16] = { 0xBB, 2 };
#define STATE_SIZE 400000

static gbahp_sock listener;
static int server_sessions_ok;

static int send_buf(gbahp_conn *c, const uint8_t *b, size_t n) { return n ? gbahp_conn_send(c, b, n) : -1; }

static void *server_main(void *arg)
{
	int sessions = (int)(intptr_t)arg, s;
	for (s = 0; s < sessions; s++) {
		gbahp_conn c;
		gbahp_frame f;
		gbahp_hello mine, theirs;
		gbahp_hello_ack ack;
		uint8_t out[1024];
		int timed_out = 0, r, ok = 1, accepted;
		gbahp_sock cs = GBAHP_BAD_SOCK;
		for (accepted = 0; accepted < 50 && cs == GBAHP_BAD_SOCK; accepted++)
			cs = gbahp_net_accept(listener, 200, &timed_out);
		if (cs == GBAHP_BAD_SOCK) { CHECK(!"accept"); return NULL; }
		gbahp_conn_init(&c, cs);

		memset(&mine, 0, sizeof mine);
		mine.proto_version = GBAHP_VERSION; memcpy(mine.device_id, SERVER_ID, 16);
		mine.core_version = gbahp_str_c("0.10.5"); mine.name = gbahp_str_c("PC");

		r = gbahp_conn_recv(&c, &f, 3000);
		CHECK(r == 1 && f.type == GBAHP_MSG_HELLO);
		if (r != 1 || gbahp_decode_hello(f.payload, f.len, &theirs) != 0) { gbahp_conn_close(&c); continue; }
		ack.hello = mine;
		ack.status = (uint8_t)gbahp_hello_compat(&mine, &theirs);
		CHECK(send_buf(&c, out, gbahp_encode_hello_ack(&ack, out, sizeof out)) == 0);
		if (ack.status != GBAHP_ACK_OK) {   /* refuse: close, as the real server does */
			gbahp_net_shutdown(c.s);
			gbahp_conn_close(&c);
			continue;
		}
		for (;;) {
			r = gbahp_conn_recv(&c, &f, 3000);
			if (r != 1) { ok = 0; break; }
			if (f.type == GBAHP_MSG_LIBRARY) {
				gbahp_library l;
				gbahp_lib_entry e;
				CHECK(gbahp_decode_library(f.payload, f.len, &l) == 0 && l.count == 1);
				gbahp_library_get(&l, 0, &e);
				CHECK(e.size == 4194304 && e.sha1[0] == 0x42);
				CHECK(send_buf(&c, out, gbahp_encode_library(&e, 1, out, sizeof out)) == 0);
			} else if (f.type == GBAHP_MSG_STATE_PUSH) {
				gbahp_state_push sp;
				gbahp_state_ack sa;
				CHECK(gbahp_decode_state_push(f.payload, f.len, &sp) == 0);
				CHECK(sp.state_len == STATE_SIZE && sp.state[STATE_SIZE - 1] == (uint8_t)((STATE_SIZE - 1) * 7));
				memcpy(sa.sha1_rom, sp.sha1_rom, 20); sa.version = sp.version; sa.status = GBAHP_STATE_ACK_OK;
				CHECK(send_buf(&c, out, gbahp_encode_state_ack(&sa, out, sizeof out)) == 0);
			} else if (f.type == GBAHP_MSG_PING) {
				gbahp_ping p;
				CHECK(gbahp_decode_ping(f.payload, f.len, &p) == 0);
				CHECK(send_buf(&c, out, gbahp_encode_pong(&p, out, sizeof out)) == 0);
			} else if (f.type == GBAHP_MSG_BYE) {
				break;
			} else { ok = 0; break; }
		}
		CHECK(ok);
		server_sessions_ok += ok;
		gbahp_conn_close(&c);
	}
	return NULL;
}

static gbahp_sock connect_retry(uint16_t port)
{
	int i;
	for (i = 0; i < 20; i++) {
		gbahp_sock s = gbahp_net_connect_loopback(port, 500);
		if (s != GBAHP_BAD_SOCK) return s;
	}
	return GBAHP_BAD_SOCK;
}

static void client_session(uint16_t port, const char *core_version, int expect_ok)
{
	gbahp_conn c;
	gbahp_frame f;
	gbahp_hello h;
	gbahp_hello_ack ack;
	uint8_t out[1024];
	gbahp_sock s = connect_retry(port);
	int r;
	CHECK(s != GBAHP_BAD_SOCK);
	if (s == GBAHP_BAD_SOCK) return;
	gbahp_conn_init(&c, s);
	memset(&h, 0, sizeof h);
	h.proto_version = GBAHP_VERSION; memcpy(h.device_id, CLIENT_ID, 16);
	h.core_version = gbahp_str_c(core_version); h.name = gbahp_str_c("Telefone");
	CHECK(send_buf(&c, out, gbahp_encode_hello(&h, out, sizeof out)) == 0);
	r = gbahp_conn_recv(&c, &f, 3000);
	CHECK(r == 1 && f.type == GBAHP_MSG_HELLO_ACK);
	CHECK(gbahp_decode_hello_ack(f.payload, f.len, &ack) == 0);
	CHECK(ack.hello.name.n == 2 && !memcmp(ack.hello.name.p, "PC", 2));
	CHECK(!memcmp(ack.hello.device_id, SERVER_ID, 16));
	if (!expect_ok) {
		CHECK(ack.status == GBAHP_ACK_CORE_MISMATCH);
		CHECK(gbahp_conn_recv(&c, &f, 3000) == -1);   /* server closes after refusing */
		gbahp_conn_close(&c);
		return;
	}
	CHECK(ack.status == GBAHP_ACK_OK);

	{ /* LIBRARY both ways */
		gbahp_lib_entry e;
		gbahp_library l;
		memset(e.sha1, 0x42, 20); e.size = 4194304;
		CHECK(send_buf(&c, out, gbahp_encode_library(&e, 1, out, sizeof out)) == 0);
		CHECK(gbahp_conn_recv(&c, &f, 3000) == 1 && f.type == GBAHP_MSG_LIBRARY);
		CHECK(gbahp_decode_library(f.payload, f.len, &l) == 0 && l.count == 1);
	}
	{ /* a 400 KB STATE_PUSH crossing many TCP segments */
		gbahp_state_push sp;
		gbahp_state_ack sa;
		size_t i, n, cap = STATE_SIZE + 1024;
		uint8_t *state = (uint8_t *)malloc(STATE_SIZE), *buf = (uint8_t *)malloc(cap);
		for (i = 0; i < STATE_SIZE; i++) state[i] = (uint8_t)(i * 7);
		memset(&sp, 0, sizeof sp);
		memset(sp.sha1_rom, 0x42, 20); sp.version = 5; sp.state = state; sp.state_len = STATE_SIZE;
		n = gbahp_encode_state_push(&sp, buf, cap);
		CHECK(n && gbahp_conn_send(&c, buf, n) == 0);
		CHECK(gbahp_conn_recv(&c, &f, 3000) == 1 && f.type == GBAHP_MSG_STATE_ACK);
		CHECK(gbahp_decode_state_ack(f.payload, f.len, &sa) == 0 && sa.version == 5 && sa.status == GBAHP_STATE_ACK_OK);
		free(state); free(buf);
	}
	{ /* PING / PONG */
		gbahp_ping p = { 0x1234567890ull }, q;
		CHECK(send_buf(&c, out, gbahp_encode_ping(&p, out, sizeof out)) == 0);
		CHECK(gbahp_conn_recv(&c, &f, 3000) == 1 && f.type == GBAHP_MSG_PONG);
		CHECK(gbahp_decode_ping(f.payload, f.len, &q) == 0 && q.token == p.token);
	}
	{ /* BYE */
		gbahp_bye b = { 0 };
		CHECK(send_buf(&c, out, gbahp_encode_bye(&b, out, sizeof out)) == 0);
	}
	gbahp_conn_close(&c);
}

int main(void)
{
	pthread_t th;
	uint16_t port;
	CHECK(gbahp_net_init() == 0);
	listener = gbahp_net_listen_loopback(0);
	CHECK(listener != GBAHP_BAD_SOCK);
	if (listener == GBAHP_BAD_SOCK) return 1;
	port = gbahp_net_local_port(listener);
	CHECK(port != 0);
	pthread_create(&th, NULL, server_main, (void *)(intptr_t)2);
	client_session(port, "0.10.4", 0);   /* incompatible core -> refused */
	client_session(port, "0.10.5", 1);   /* full happy path */
	pthread_join(th, NULL);
	CHECK(server_sessions_ok == 1);
	gbahp_net_close(listener);
	if (failures) return 1;
	puts("proto_loopback_test: ok");
	return 0;
}
