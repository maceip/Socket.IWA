/*
 * test_session_ticket.c — demonstrate QUIC session ticket + 0-RTT resumption
 *
 * 1. connects to the echo server, completes handshake, receives session ticket
 * 2. disconnects
 * 3. reconnects using the saved ticket — sends 0-RTT early data
 * 4. verifies the echo comes back
 *
 * build (native):
 *   cc -O2 -o test_session_ticket test_session_ticket.c \
 *     -I<deps>/include -L<deps>/lib \
 *     -lngtcp2 -lngtcp2_crypto_wolfssl -lwolfssl -lnghttp3 -lm
 *
 * build (wasm): via docker_build_quic.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/quic.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_wolfssl.h>

#define SERVER_HOST "127.0.0.1"
#define SERVER_PORT 4433
#define ECHO_MSG    "hello from 0-RTT"
#define BUF_SIZE    65536

/* saved session state between connections */
static unsigned char *g_ticket_data = NULL;
static int            g_ticket_len  = 0;
static unsigned char  g_tp_data[4096];
static size_t         g_tp_len = 0;

static uint64_t timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ngtcp2 log callback */
static void log_printf(void *user_data, const char *fmt, ...) {
    (void)user_data;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

/* connection ref for wolfssl <-> ngtcp2 bridge */
typedef struct {
    ngtcp2_conn *conn;
    ngtcp2_crypto_conn_ref conn_ref;
    WOLFSSL     *ssl;
    int          fd;
    int64_t      stream_id;
    int          handshake_done;
    int          got_ticket;
    int          got_echo;
    unsigned char echo_buf[BUF_SIZE];
    size_t       echo_len;
} client_conn;

static ngtcp2_conn *get_conn_from_ref(ngtcp2_crypto_conn_ref *ref) {
    client_conn *cc = (client_conn *)ref->user_data;
    return cc->conn;
}

/* wolfssl new session callback — saves the serialized session */
static int new_session_cb(WOLFSSL *ssl, WOLFSSL_SESSION *session) {
    ngtcp2_crypto_conn_ref *ref =
        (ngtcp2_crypto_conn_ref *)wolfSSL_get_app_data(ssl);
    client_conn *cc = (client_conn *)ref->user_data;

    /* serialize the session */
    int sz = wolfSSL_i2d_SSL_SESSION(session, NULL);
    if (sz <= 0) return 0;

    if (g_ticket_data) free(g_ticket_data);
    g_ticket_data = (unsigned char *)malloc(sz);
    if (!g_ticket_data) return 0;

    unsigned char *p = g_ticket_data;
    sz = wolfSSL_i2d_SSL_SESSION(session, &p);
    g_ticket_len = sz;
    cc->got_ticket = 1;

    fprintf(stderr, "[TICKET] saved session (%d bytes)\n", sz);
    return 0;
}

/* ngtcp2 callbacks */
static void rand_cb(uint8_t *dest, size_t destlen,
                    const ngtcp2_rand_ctx *rand_ctx) {
    (void)rand_ctx;
    for (size_t i = 0; i < destlen; i++)
        dest[i] = (uint8_t)(rand() & 0xff);
}

static int get_new_cid_cb(ngtcp2_conn *conn, ngtcp2_cid *cid, uint8_t *token,
                          size_t cidlen, void *user_data) {
    (void)conn; (void)user_data;
    for (size_t i = 0; i < cidlen; i++)
        cid->data[i] = (uint8_t)(rand() & 0xff);
    cid->datalen = cidlen;
    for (size_t i = 0; i < NGTCP2_STATELESS_RESET_TOKENLEN; i++)
        token[i] = (uint8_t)(rand() & 0xff);
    return 0;
}

static int recv_stream_data_cb(ngtcp2_conn *conn, uint32_t flags,
                               int64_t stream_id, uint64_t offset,
                               const uint8_t *data, size_t datalen,
                               void *user_data, void *stream_user_data) {
    (void)conn; (void)flags; (void)stream_id; (void)offset;
    (void)stream_user_data;
    client_conn *cc = (client_conn *)user_data;

    if (datalen > 0 && cc->echo_len + datalen < BUF_SIZE) {
        memcpy(cc->echo_buf + cc->echo_len, data, datalen);
        cc->echo_len += datalen;
        cc->got_echo = 1;
    }
    return 0;
}

static int handshake_completed_cb(ngtcp2_conn *conn, void *user_data) {
    (void)conn;
    client_conn *cc = (client_conn *)user_data;
    cc->handshake_done = 1;
    fprintf(stderr, "[QUIC] handshake completed\n");
    return 0;
}

static int extend_max_local_streams_bidi_cb(ngtcp2_conn *conn,
                                            uint64_t max_streams,
                                            void *user_data) {
    (void)conn; (void)max_streams; (void)user_data;
    return 0;
}

/* create udp socket */
static int create_udp_socket(struct sockaddr_in *local_addr) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    memset(local_addr, 0, sizeof(*local_addr));
    local_addr->sin_family = AF_INET;
    local_addr->sin_addr.s_addr = INADDR_ANY;
    local_addr->sin_port = 0;

    if (bind(fd, (struct sockaddr *)local_addr, sizeof(*local_addr)) < 0) {
        close(fd);
        return -1;
    }

    socklen_t len = sizeof(*local_addr);
    getsockname(fd, (struct sockaddr *)local_addr, &len);
    return fd;
}

/* run one connection attempt, optionally with 0-RTT */
static int run_connection(int attempt, int use_0rtt) {
    fprintf(stderr, "\n=== Connection %d %s ===\n",
            attempt, use_0rtt ? "(0-RTT resumption)" : "(full handshake)");

    client_conn cc = {0};
    cc.stream_id = -1;

    /* udp socket */
    struct sockaddr_in local_addr, remote_addr;
    cc.fd = create_udp_socket(&local_addr);
    if (cc.fd < 0) {
        fprintf(stderr, "socket failed: %s\n", strerror(errno));
        return -1;
    }

    memset(&remote_addr, 0, sizeof(remote_addr));
    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_HOST, &remote_addr.sin_addr);

    /* wolfssl */
    WOLFSSL_CTX *ssl_ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    if (!ssl_ctx) { close(cc.fd); return -1; }

    ngtcp2_crypto_wolfssl_configure_client_context(ssl_ctx);
    wolfSSL_CTX_set_verify(ssl_ctx, WOLFSSL_VERIFY_NONE, NULL);
    wolfSSL_CTX_UseSessionTicket(ssl_ctx);
    wolfSSL_CTX_sess_set_new_cb(ssl_ctx, new_session_cb);

    cc.ssl = wolfSSL_new(ssl_ctx);
    if (!cc.ssl) { wolfSSL_CTX_free(ssl_ctx); close(cc.fd); return -1; }

    wolfSSL_set_connect_state(cc.ssl);
    wolfSSL_set_quic_use_legacy_codepoint(cc.ssl, 0);
    wolfSSL_UseSessionTicket(cc.ssl);

    /* set ALPN */
    static const unsigned char alpn[] = "\x04""echo";
    wolfSSL_set_alpn_protos(cc.ssl, alpn, sizeof(alpn) - 1);

    /* restore session ticket for 0-RTT */
    if (use_0rtt && g_ticket_data) {
        const unsigned char *pdata = g_ticket_data;
        WOLFSSL_SESSION *session = wolfSSL_d2i_SSL_SESSION(
            NULL, &pdata, g_ticket_len);
        if (session) {
            wolfSSL_set_session(cc.ssl, session);
#ifdef WOLFSSL_EARLY_DATA
            if (wolfSSL_SESSION_get_max_early_data(session)) {
                wolfSSL_set_quic_early_data_enabled(cc.ssl, 1);
                fprintf(stderr, "[TICKET] restored session, 0-RTT enabled\n");
            } else {
                fprintf(stderr, "[TICKET] restored session, no early data\n");
            }
#else
            fprintf(stderr, "[TICKET] restored session\n");
#endif
            wolfSSL_SESSION_free(session);
        }
    }

    /* ngtcp2 connection */
    ngtcp2_path path;
    path.local.addr = (struct sockaddr *)&local_addr;
    path.local.addrlen = sizeof(local_addr);
    path.remote.addr = (struct sockaddr *)&remote_addr;
    path.remote.addrlen = sizeof(remote_addr);

    ngtcp2_cid dcid, scid;
    dcid.datalen = scid.datalen = 16;
    for (int i = 0; i < 16; i++) {
        dcid.data[i] = (uint8_t)(rand() & 0xff);
        scid.data[i] = (uint8_t)(rand() & 0xff);
    }

    ngtcp2_callbacks callbacks = {0};
    callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
    callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
    callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
    callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
    callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;
    callbacks.update_key = ngtcp2_crypto_update_key_cb;
    callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    callbacks.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;
    callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    callbacks.rand = rand_cb;
    callbacks.get_new_connection_id = get_new_cid_cb;
    callbacks.recv_stream_data = recv_stream_data_cb;
    callbacks.handshake_completed = handshake_completed_cb;
    callbacks.extend_max_local_streams_bidi = extend_max_local_streams_bidi_cb;

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = timestamp_ns();
    settings.log_printf = NULL; /* set to log_printf for debug */

    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_streams_bidi = 4;
    params.initial_max_streams_uni = 4;
    params.initial_max_data = 1 << 20;
    params.initial_max_stream_data_bidi_local = 256 * 1024;
    params.initial_max_stream_data_bidi_remote = 256 * 1024;
    params.initial_max_stream_data_uni = 256 * 1024;

    int rv = ngtcp2_conn_client_new(&cc.conn, &dcid, &scid, &path,
                                     NGTCP2_PROTO_VER_V1, &callbacks,
                                     &settings, &params, NULL, &cc);
    if (rv != 0) {
        fprintf(stderr, "ngtcp2_conn_client_new failed: %s\n", ngtcp2_strerror(rv));
        wolfSSL_free(cc.ssl);
        wolfSSL_CTX_free(ssl_ctx);
        close(cc.fd);
        return -1;
    }

    /* restore 0-RTT transport params after conn creation */
    if (use_0rtt && g_tp_len > 0) {
        rv = ngtcp2_conn_decode_and_set_0rtt_transport_params(
            cc.conn, g_tp_data, g_tp_len);
        if (rv == 0)
            fprintf(stderr, "[0-RTT] restored transport params (%zu bytes)\n", g_tp_len);
        else
            fprintf(stderr, "[0-RTT] transport params restore failed: %s\n", ngtcp2_strerror(rv));
    }

    cc.conn_ref.get_conn = get_conn_from_ref;
    cc.conn_ref.user_data = &cc;
    wolfSSL_set_app_data(cc.ssl, &cc.conn_ref);
    ngtcp2_conn_set_tls_native_handle(cc.conn, cc.ssl);

    /* main loop */
    unsigned char buf[BUF_SIZE];
    int sent_data = 0;
    int loops = 0;
    int max_loops = 200; /* ~2 seconds */

    while (loops++ < max_loops) {
        /* write outgoing packets */
        ngtcp2_path_storage ps;
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_pkt_info pi;
        ngtcp2_ssize nwrite;

        for (;;) {
            ngtcp2_vec datav;
            int64_t stream_id_out = -1;
            int fin = 0;

            /* try to send echo data on a stream */
            if (!sent_data && (cc.handshake_done || use_0rtt) && cc.stream_id < 0) {
                rv = ngtcp2_conn_open_bidi_stream(cc.conn, &cc.stream_id, NULL);
                if (rv == 0) {
                    fprintf(stderr, "[QUIC] opened stream %lld\n",
                            (long long)cc.stream_id);
                }
            }

            if (!sent_data && cc.stream_id >= 0) {
                datav.base = (uint8_t *)ECHO_MSG;
                datav.len = strlen(ECHO_MSG);
                stream_id_out = cc.stream_id;
                fin = 1;
            }

            if (stream_id_out >= 0) {
                nwrite = ngtcp2_conn_writev_stream(
                    cc.conn, &ps.path, &pi, buf, sizeof(buf),
                    NULL, NGTCP2_WRITE_STREAM_FLAG_FIN,
                    stream_id_out, &datav, 1, timestamp_ns());
            } else {
                nwrite = ngtcp2_conn_write_pkt(cc.conn, &ps.path, &pi,
                                                buf, sizeof(buf), timestamp_ns());
            }

            if (nwrite <= 0) break;

            if (stream_id_out >= 0) {
                sent_data = 1;
                fprintf(stderr, "[QUIC] sent '%s' (%zu bytes)%s\n",
                        ECHO_MSG, strlen(ECHO_MSG),
                        use_0rtt ? " [0-RTT]" : "");
            }

            sendto(cc.fd, buf, nwrite, 0,
                   (struct sockaddr *)&remote_addr, sizeof(remote_addr));
        }

        /* poll for incoming */
        struct pollfd pfd = { .fd = cc.fd, .events = POLLIN };
        int ready = poll(&pfd, 1, 10);

        if (ready > 0 && (pfd.revents & POLLIN)) {
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            ssize_t nread = recvfrom(cc.fd, buf, sizeof(buf), 0,
                                      (struct sockaddr *)&from, &fromlen);
            if (nread > 0) {
                ngtcp2_path recv_path = path;
                ngtcp2_pkt_info recv_pi = {0};
                ngtcp2_conn_read_pkt(cc.conn, &recv_path, &recv_pi,
                                      buf, nread, timestamp_ns());
            }
        }

        /* check if we got the echo back */
        if (cc.got_echo) {
            cc.echo_buf[cc.echo_len] = '\0';
            fprintf(stderr, "[ECHO] received: '%s' (%zu bytes)\n",
                    cc.echo_buf, cc.echo_len);
            break;
        }

        /* check for errors */
        if (ngtcp2_conn_in_draining_period(cc.conn)) {
            fprintf(stderr, "[QUIC] connection draining\n");
            break;
        }
    }

    /* save transport params for next 0-RTT attempt */
    if (cc.handshake_done && !use_0rtt) {
        ngtcp2_ssize tplen = ngtcp2_conn_encode_0rtt_transport_params(
            cc.conn, g_tp_data, sizeof(g_tp_data));
        if (tplen > 0) {
            g_tp_len = (size_t)tplen;
            fprintf(stderr, "[TICKET] saved transport params (%zu bytes)\n", g_tp_len);
        }
    }

    /* wait a bit for the session ticket if we haven't gotten one yet */
    if (!cc.got_ticket && !use_0rtt) {
        fprintf(stderr, "[TICKET] waiting for session ticket...\n");
        for (int i = 0; i < 50 && !cc.got_ticket; i++) {
            /* pump packets */
            ngtcp2_path_storage ps;
            ngtcp2_path_storage_zero(&ps);
            ngtcp2_pkt_info pi;
            ngtcp2_ssize nwrite = ngtcp2_conn_write_pkt(
                cc.conn, &ps.path, &pi, buf, sizeof(buf), timestamp_ns());
            if (nwrite > 0) {
                sendto(cc.fd, buf, nwrite, 0,
                       (struct sockaddr *)&remote_addr, sizeof(remote_addr));
            }

            struct pollfd pfd = { .fd = cc.fd, .events = POLLIN };
            if (poll(&pfd, 1, 20) > 0 && (pfd.revents & POLLIN)) {
                struct sockaddr_in from;
                socklen_t fromlen = sizeof(from);
                ssize_t nread = recvfrom(cc.fd, buf, sizeof(buf), 0,
                                          (struct sockaddr *)&from, &fromlen);
                if (nread > 0) {
                    ngtcp2_pkt_info recv_pi = {0};
                    ngtcp2_conn_read_pkt(cc.conn, &path, &recv_pi,
                                          buf, nread, timestamp_ns());
                }
            }
        }
    }

    int success = cc.got_echo &&
                  cc.echo_len == strlen(ECHO_MSG) &&
                  memcmp(cc.echo_buf, ECHO_MSG, cc.echo_len) == 0;

    fprintf(stderr, "[RESULT] connection %d: echo=%s ticket=%s\n",
            attempt, success ? "OK" : "FAIL",
            cc.got_ticket ? "saved" : (use_0rtt ? "reused" : "none"));

    /* Send CONNECTION_CLOSE so server can clean up */
    {
        ngtcp2_path_storage ps;
        ngtcp2_path_storage_zero(&ps);
        ngtcp2_pkt_info pi;
        ngtcp2_ccerr ccerr;
        ngtcp2_ccerr_default(&ccerr);
        ngtcp2_ssize nwrite = ngtcp2_conn_write_connection_close(
            cc.conn, &ps.path, &pi, buf, sizeof(buf), &ccerr, timestamp_ns());
        if (nwrite > 0) {
            sendto(cc.fd, buf, nwrite, 0,
                   (struct sockaddr *)&remote_addr, sizeof(remote_addr));
        }
    }

    ngtcp2_conn_del(cc.conn);
    wolfSSL_free(cc.ssl);
    wolfSSL_CTX_free(ssl_ctx);
    close(cc.fd);

    return success ? 0 : -1;
}

int main(void) {
    srand((unsigned)time(NULL));
    wolfSSL_Init();

    fprintf(stderr, "=== QUIC Session Ticket + 0-RTT Test ===\n");
    fprintf(stderr, "server: %s:%d\n\n", SERVER_HOST, SERVER_PORT);

    /* connection 1: full handshake, get session ticket */
    int rv1 = run_connection(1, 0);

    if (rv1 != 0) {
        fprintf(stderr, "\nFAIL: first connection failed\n");
        wolfSSL_Cleanup();
        return 1;
    }

    if (!g_ticket_data || g_ticket_len == 0) {
        fprintf(stderr, "\nFAIL: no session ticket received\n");
        wolfSSL_Cleanup();
        return 1;
    }

    /* brief pause */
    usleep(100000);

    /* connection 2: 0-RTT resumption with saved ticket */
    int rv2 = run_connection(2, 1);

    fprintf(stderr, "\n=== Summary ===\n");
    fprintf(stderr, "connection 1 (full handshake): %s\n", rv1 == 0 ? "PASS" : "FAIL");
    fprintf(stderr, "connection 2 (0-RTT resume):   %s\n", rv2 == 0 ? "PASS" : "FAIL");

    if (g_ticket_data) free(g_ticket_data);
    wolfSSL_Cleanup();

    return (rv1 == 0 && rv2 == 0) ? 0 : 1;
}
