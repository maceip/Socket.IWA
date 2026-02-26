/*
 * quic_echo_server.c — QUIC echo server with HTTP/3 + WebTransport support.
 *
 * Supports three modes:
 *   1. Raw QUIC echo (ALPN "echo") — echoes stream data back
 *   2. HTTP/3 (ALPN "h3") — serves simple responses
 *   3. WebTransport over H3 (ALPN "h3") — Extended CONNECT with :protocol=webtransport
 *   4. WebSocket over H3 (RFC 9220) — Extended CONNECT with :protocol=websocket
 *
 * Uses ngtcp2 + nghttp3 + wolfSSL, compiled to WASM via Emscripten.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_wolfssl.h>

#include <nghttp3/nghttp3.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/error-crypt.h>

/* Embedded cert+key generated at build time by gen_cert.sh */
#include "cert_data.h"

/* ============================================================
 * Constants
 * ============================================================ */

#define SERVER_PORT       4433
#define MAX_UDP_PAYLOAD   1200
#define SCID_LEN          16
#define MAX_STREAMS       128
#define STREAM_BUF_SIZE   (64 * 1024)

/* Static secret for stateless reset tokens */
static uint8_t static_secret[32];

/* ============================================================
 * Per-stream state
 * ============================================================ */

typedef enum {
    STREAM_TYPE_RAW_ECHO,     /* Raw QUIC echo (ALPN "echo") */
    STREAM_TYPE_H3_REQUEST,   /* HTTP/3 request stream */
    STREAM_TYPE_WT_BIDI,      /* WebTransport bidirectional stream */
    STREAM_TYPE_WT_UNI,       /* WebTransport unidirectional stream */
    STREAM_TYPE_WS,           /* WebSocket over H3 (RFC 9220) */
} stream_type_t;

typedef struct stream_data {
    int64_t      stream_id;
    stream_type_t type;
    uint8_t      sendbuf[STREAM_BUF_SIZE];
    size_t       sendlen;
    size_t       sendoff;
    int          fin_received;
    /* HTTP/3 request info */
    char         method[16];
    char         path[256];
    char         protocol[32];  /* :protocol pseudo-header for Extended CONNECT */
    int64_t      wt_session_id; /* WebTransport session stream ID (-1 if none) */
    struct stream_data *next;
} stream_data;

/* ============================================================
 * Connection protocol type
 * ============================================================ */

typedef enum {
    PROTO_ECHO,  /* Raw QUIC echo */
    PROTO_H3,    /* HTTP/3 (+ WebTransport/WebSocket) */
} proto_type_t;

/* ============================================================
 * Per-connection state
 * ============================================================ */

typedef struct {
    ngtcp2_conn              *conn;
    WOLFSSL                  *ssl;
    ngtcp2_crypto_conn_ref    conn_ref;
    nghttp3_conn             *h3conn;
    int                       fd;
    struct sockaddr_storage   local_addr;
    socklen_t                 local_addrlen;
    struct sockaddr_storage   remote_addr;
    socklen_t                 remote_addrlen;
    stream_data              *streams;
    ngtcp2_ccerr              last_error;
    int                       handshake_done;
    proto_type_t              proto;
    int64_t                   wt_session_stream; /* active WebTransport session, or -1 */
} server_conn;

static server_conn *g_sconn = NULL;

/* ============================================================
 * wolfSSL context (global)
 * ============================================================ */

static WOLFSSL_CTX *g_ssl_ctx = NULL;

/* ============================================================
 * Timestamp helper
 * ============================================================ */

static ngtcp2_tstamp timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ngtcp2_tstamp)ts.tv_sec * NGTCP2_SECONDS + (ngtcp2_tstamp)ts.tv_nsec;
}

/* ============================================================
 * Stream helpers
 * ============================================================ */

static stream_data *find_stream(server_conn *sc, int64_t stream_id) {
    for (stream_data *s = sc->streams; s; s = s->next) {
        if (s->stream_id == stream_id) return s;
    }
    return NULL;
}

static stream_data *create_stream(server_conn *sc, int64_t stream_id) {
    stream_data *s = find_stream(sc, stream_id);
    if (s) return s;
    s = calloc(1, sizeof(stream_data));
    if (!s) return NULL;
    s->stream_id = stream_id;
    s->wt_session_id = -1;
    s->next = sc->streams;
    sc->streams = s;
    return s;
}

static void remove_stream(server_conn *sc, int64_t stream_id) {
    stream_data **pp = &sc->streams;
    while (*pp) {
        if ((*pp)->stream_id == stream_id) {
            stream_data *tmp = *pp;
            *pp = tmp->next;
            free(tmp);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ============================================================
 * ngtcp2 callbacks — shared between echo and h3 modes
 * ============================================================ */

static ngtcp2_conn *get_conn_cb(ngtcp2_crypto_conn_ref *ref) {
    server_conn *sc = (server_conn *)ref->user_data;
    return sc->conn;
}

static int handshake_completed_cb(ngtcp2_conn *conn, void *user_data) {
    server_conn *sc = (server_conn *)user_data;
    sc->handshake_done = 1;
    fprintf(stderr, "[QUIC] Handshake completed!\n");
    return 0;
}

/* Forward declarations */
static int write_streams(server_conn *sc);
static int setup_h3_connection(server_conn *sc);

static int recv_stream_data_cb(ngtcp2_conn *conn, uint32_t flags,
                               int64_t stream_id, uint64_t offset,
                               const uint8_t *data, size_t datalen,
                               void *user_data, void *stream_user_data) {
    server_conn *sc = (server_conn *)user_data;
    (void)offset;
    (void)stream_user_data;

    if (sc->proto == PROTO_H3 && sc->h3conn) {
        /* Feed data to nghttp3 */
        nghttp3_ssize nconsumed = nghttp3_conn_read_stream(
            sc->h3conn, stream_id, data, datalen,
            (flags & NGTCP2_STREAM_DATA_FLAG_FIN) ? 1 : 0);
        if (nconsumed < 0) {
            fprintf(stderr, "[H3] read_stream error: %s\n",
                    nghttp3_strerror((int)nconsumed));
            ngtcp2_ccerr_set_application_error(
                &sc->last_error,
                nghttp3_err_infer_quic_app_error_code((int)nconsumed),
                NULL, 0);
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
        ngtcp2_conn_extend_max_stream_offset(conn, stream_id,
                                             (uint64_t)nconsumed);
        ngtcp2_conn_extend_max_offset(conn, (uint64_t)nconsumed);
        return 0;
    }

    /* Raw echo mode */
    stream_data *s = find_stream(sc, stream_id);
    if (!s) {
        s = create_stream(sc, stream_id);
        if (!s) return NGTCP2_ERR_CALLBACK_FAILURE;
        s->type = STREAM_TYPE_RAW_ECHO;
    }

    size_t space = STREAM_BUF_SIZE - s->sendlen;
    size_t copy = datalen < space ? datalen : space;
    if (copy > 0) {
        memcpy(s->sendbuf + s->sendlen, data, copy);
        s->sendlen += copy;
    }

    if (flags & NGTCP2_STREAM_DATA_FLAG_FIN) {
        s->fin_received = 1;
    }

    ngtcp2_conn_extend_max_stream_offset(conn, stream_id, datalen);
    ngtcp2_conn_extend_max_offset(conn, datalen);
    return 0;
}

static int stream_open_cb(ngtcp2_conn *conn, int64_t stream_id, void *user_data) {
    server_conn *sc = (server_conn *)user_data;
    (void)conn;

    if (sc->proto == PROTO_ECHO) {
        create_stream(sc, stream_id);
    }
    /* For H3 mode, nghttp3 handles stream lifecycle via begin_headers */
    return 0;
}

static int stream_close_cb(ngtcp2_conn *conn, uint32_t flags,
                           int64_t stream_id, uint64_t app_error_code,
                           void *user_data, void *stream_user_data) {
    server_conn *sc = (server_conn *)user_data;
    (void)stream_user_data;

    if (sc->h3conn) {
        if (!(flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET)) {
            app_error_code = NGHTTP3_H3_NO_ERROR;
        }
        int rv = nghttp3_conn_close_stream(sc->h3conn, stream_id, app_error_code);
        if (rv != 0 && rv != NGHTTP3_ERR_STREAM_NOT_FOUND) {
            fprintf(stderr, "[H3] close_stream error: %s\n", nghttp3_strerror(rv));
            ngtcp2_ccerr_set_application_error(
                &sc->last_error,
                nghttp3_err_infer_quic_app_error_code(rv), NULL, 0);
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
    }

    /* Clean up WebTransport session if this was the session stream */
    if (sc->wt_session_stream == stream_id) {
        fprintf(stderr, "[WT] WebTransport session closed (stream=%lld)\n",
                (long long)stream_id);
        sc->wt_session_stream = -1;
    }

    remove_stream(sc, stream_id);
    ngtcp2_conn_extend_max_streams_bidi(conn, 1);
    return 0;
}

static int stream_reset_cb(ngtcp2_conn *conn, int64_t stream_id,
                           uint64_t final_size, uint64_t app_error_code,
                           void *user_data, void *stream_user_data) {
    server_conn *sc = (server_conn *)user_data;
    (void)conn; (void)final_size; (void)app_error_code; (void)stream_user_data;

    if (sc->h3conn) {
        int rv = nghttp3_conn_shutdown_stream_read(sc->h3conn, stream_id);
        if (rv != 0) {
            fprintf(stderr, "[H3] shutdown_stream_read error: %s\n",
                    nghttp3_strerror(rv));
        }
    }
    return 0;
}

static int stream_stop_sending_cb(ngtcp2_conn *conn, int64_t stream_id,
                                  uint64_t app_error_code, void *user_data,
                                  void *stream_user_data) {
    server_conn *sc = (server_conn *)user_data;
    (void)conn; (void)app_error_code; (void)stream_user_data;

    if (sc->h3conn) {
        int rv = nghttp3_conn_shutdown_stream_read(sc->h3conn, stream_id);
        if (rv != 0) {
            fprintf(stderr, "[H3] shutdown_stream_read error: %s\n",
                    nghttp3_strerror(rv));
        }
    }
    return 0;
}

static int acked_stream_data_offset_cb(ngtcp2_conn *conn, int64_t stream_id,
                                       uint64_t offset, uint64_t datalen,
                                       void *user_data, void *stream_user_data) {
    server_conn *sc = (server_conn *)user_data;
    (void)conn; (void)offset; (void)stream_user_data;

    if (sc->h3conn) {
        int rv = nghttp3_conn_add_ack_offset(sc->h3conn, stream_id, datalen);
        if (rv != 0) {
            fprintf(stderr, "[H3] add_ack_offset error: %s\n",
                    nghttp3_strerror(rv));
            return NGTCP2_ERR_CALLBACK_FAILURE;
        }
    }
    return 0;
}

static int recv_datagram_cb(ngtcp2_conn *conn, uint32_t flags,
                            const uint8_t *data, size_t datalen,
                            void *user_data) {
    server_conn *sc = (server_conn *)user_data;
    (void)conn; (void)flags;

    fprintf(stderr, "[QUIC] Received DATAGRAM frame (%zu bytes)\n", datalen);

    /* Echo the datagram back (for WebTransport datagram echo) */
    /* The first quarter-stream-id varint identifies the WT session;
       for simplicity we just echo the whole frame back */
    ngtcp2_vec datav = { .base = (uint8_t *)data, .len = datalen };
    int accepted = 0;
    uint8_t txbuf[MAX_UDP_PAYLOAD];
    ngtcp2_path_storage ps;
    ngtcp2_pkt_info pi;
    ngtcp2_path_storage_zero(&ps);

    ngtcp2_ssize nwrite = ngtcp2_conn_writev_datagram(
        sc->conn, &ps.path, &pi,
        txbuf, sizeof(txbuf),
        &accepted,
        NGTCP2_WRITE_DATAGRAM_FLAG_NONE,
        0, /* dgram_id */
        &datav, 1,
        timestamp_ns());

    if (nwrite > 0 && accepted) {
        sendto(sc->fd, txbuf, (size_t)nwrite, 0,
               (struct sockaddr *)&sc->remote_addr, sc->remote_addrlen);
    }

    return 0;
}

static void rand_cb(uint8_t *dest, size_t destlen,
                    const ngtcp2_rand_ctx *rand_ctx) {
    (void)rand_ctx;
    WC_RNG rng;
    wc_InitRng(&rng);
    wc_RNG_GenerateBlock(&rng, dest, (word32)destlen);
    wc_FreeRng(&rng);
}

static int get_new_connection_id_cb(ngtcp2_conn *conn, ngtcp2_cid *cid,
                                    uint8_t *token, size_t cidlen,
                                    void *user_data) {
    (void)conn; (void)user_data;
    WC_RNG rng;
    wc_InitRng(&rng);
    wc_RNG_GenerateBlock(&rng, cid->data, (word32)cidlen);
    cid->datalen = cidlen;

    if (ngtcp2_crypto_generate_stateless_reset_token(
            token, static_secret, sizeof(static_secret), cid) != 0) {
        wc_FreeRng(&rng);
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    wc_FreeRng(&rng);
    return 0;
}

static int remove_connection_id_cb(ngtcp2_conn *conn, const ngtcp2_cid *cid,
                                   void *user_data) {
    (void)conn; (void)cid; (void)user_data;
    return 0;
}

static int extend_max_remote_streams_bidi_cb(ngtcp2_conn *conn,
                                             uint64_t max_streams,
                                             void *user_data) {
    server_conn *sc = (server_conn *)user_data;
    (void)conn;

    if (sc->h3conn) {
        nghttp3_conn_set_max_client_streams_bidi(sc->h3conn, max_streams);
    }
    return 0;
}

/* ============================================================
 * nghttp3 (HTTP/3) callbacks
 * ============================================================ */

static int h3_acked_stream_data(nghttp3_conn *conn, int64_t stream_id,
                                uint64_t datalen, void *conn_user_data,
                                void *stream_user_data) {
    (void)conn; (void)stream_id; (void)datalen;
    (void)conn_user_data; (void)stream_user_data;
    return 0;
}

static int h3_recv_data(nghttp3_conn *conn, int64_t stream_id,
                        const uint8_t *data, size_t datalen,
                        void *conn_user_data, void *stream_user_data) {
    server_conn *sc = (server_conn *)conn_user_data;
    (void)conn;
    (void)stream_user_data;

    stream_data *s = find_stream(sc, stream_id);
    if (!s) return 0;

    if (s->type == STREAM_TYPE_WT_BIDI || s->type == STREAM_TYPE_WS) {
        /* Echo data back on WebTransport / WebSocket streams */
        fprintf(stderr, "[WT/WS] recv_data stream=%lld len=%zu\n",
                (long long)stream_id, datalen);

        size_t space = STREAM_BUF_SIZE - s->sendlen;
        size_t copy = datalen < space ? datalen : space;
        if (copy > 0) {
            memcpy(s->sendbuf + s->sendlen, data, copy);
            s->sendlen += copy;
        }
    }
    return 0;
}

static int h3_deferred_consume(nghttp3_conn *conn, int64_t stream_id,
                               size_t consumed, void *conn_user_data,
                               void *stream_user_data) {
    server_conn *sc = (server_conn *)conn_user_data;
    (void)conn; (void)stream_user_data;
    ngtcp2_conn_extend_max_stream_offset(sc->conn, stream_id, consumed);
    ngtcp2_conn_extend_max_offset(sc->conn, consumed);
    return 0;
}

static int h3_begin_headers(nghttp3_conn *conn, int64_t stream_id,
                            void *conn_user_data, void *stream_user_data) {
    server_conn *sc = (server_conn *)conn_user_data;
    (void)conn; (void)stream_user_data;

    stream_data *s = create_stream(sc, stream_id);
    if (!s) return NGHTTP3_ERR_CALLBACK_FAILURE;
    s->type = STREAM_TYPE_H3_REQUEST;
    nghttp3_conn_set_stream_user_data(sc->h3conn, stream_id, s);

    fprintf(stderr, "[H3] begin_headers stream=%lld\n", (long long)stream_id);
    return 0;
}

static int h3_recv_header(nghttp3_conn *conn, int64_t stream_id, int32_t token,
                          nghttp3_rcbuf *name, nghttp3_rcbuf *value,
                          uint8_t flags, void *conn_user_data,
                          void *stream_user_data) {
    server_conn *sc = (server_conn *)conn_user_data;
    (void)conn; (void)flags;
    stream_data *s = (stream_data *)stream_user_data;
    if (!s) s = find_stream(sc, stream_id);
    if (!s) return 0;

    nghttp3_vec namev = nghttp3_rcbuf_get_buf(name);
    nghttp3_vec valuev = nghttp3_rcbuf_get_buf(value);

    fprintf(stderr, "[H3]   header: %.*s: %.*s\n",
            (int)namev.len, namev.base, (int)valuev.len, valuev.base);

    /* Capture important pseudo-headers */
    if (token == NGHTTP3_QPACK_TOKEN__METHOD) {
        size_t len = valuev.len < sizeof(s->method) - 1 ? valuev.len : sizeof(s->method) - 1;
        memcpy(s->method, valuev.base, len);
        s->method[len] = 0;
    } else if (token == NGHTTP3_QPACK_TOKEN__PATH) {
        size_t len = valuev.len < sizeof(s->path) - 1 ? valuev.len : sizeof(s->path) - 1;
        memcpy(s->path, valuev.base, len);
        s->path[len] = 0;
    } else if (token == NGHTTP3_QPACK_TOKEN__PROTOCOL) {
        size_t len = valuev.len < sizeof(s->protocol) - 1 ? valuev.len : sizeof(s->protocol) - 1;
        memcpy(s->protocol, valuev.base, len);
        s->protocol[len] = 0;
    }

    return 0;
}

/* Helper: submit an HTTP/3 response */
static int submit_response(server_conn *sc, int64_t stream_id,
                           int status_code, const char *content_type,
                           const uint8_t *body, size_t bodylen) {
    char status_str[4];
    snprintf(status_str, sizeof(status_str), "%d", status_code);

    nghttp3_nv nva[3];
    size_t nvlen = 0;

    nva[nvlen].name = (uint8_t *)":status";
    nva[nvlen].namelen = 7;
    nva[nvlen].value = (uint8_t *)status_str;
    nva[nvlen].valuelen = strlen(status_str);
    nva[nvlen].flags = NGHTTP3_NV_FLAG_NONE;
    nvlen++;

    if (content_type) {
        nva[nvlen].name = (uint8_t *)"content-type";
        nva[nvlen].namelen = 12;
        nva[nvlen].value = (uint8_t *)content_type;
        nva[nvlen].valuelen = strlen(content_type);
        nva[nvlen].flags = NGHTTP3_NV_FLAG_NONE;
        nvlen++;
    }

    /* For responses with body, we need a data provider */
    /* For simplicity, send headers-only for now and let stream data be handled elsewhere */
    int rv = nghttp3_conn_submit_response(sc->h3conn, stream_id, nva, nvlen, NULL);
    if (rv != 0) {
        fprintf(stderr, "[H3] submit_response error: %s\n", nghttp3_strerror(rv));
        return -1;
    }

    return 0;
}

static int h3_end_headers(nghttp3_conn *conn, int64_t stream_id, int fin,
                          void *conn_user_data, void *stream_user_data) {
    server_conn *sc = (server_conn *)conn_user_data;
    stream_data *s = (stream_data *)stream_user_data;
    (void)conn; (void)fin;
    if (!s) s = find_stream(sc, stream_id);
    if (!s) return 0;

    fprintf(stderr, "[H3] end_headers stream=%lld method=%s path=%s protocol=%s\n",
            (long long)stream_id, s->method, s->path, s->protocol);

    /* ── WebTransport Extended CONNECT ── */
    if (strcmp(s->method, "CONNECT") == 0 &&
        strcmp(s->protocol, "webtransport") == 0) {
        fprintf(stderr, "[WT] WebTransport session request on stream %lld\n",
                (long long)stream_id);
        s->type = STREAM_TYPE_WT_BIDI;
        sc->wt_session_stream = stream_id;

        /* Accept with 200 */
        nghttp3_nv nva[] = {
            { (uint8_t *)":status", (uint8_t *)"200", 7, 3, NGHTTP3_NV_FLAG_NONE },
            { (uint8_t *)"sec-webtransport-http3-draft", (uint8_t *)"draft02",
              28, 7, NGHTTP3_NV_FLAG_NONE },
        };
        int rv = nghttp3_conn_submit_response(sc->h3conn, stream_id,
                                              nva, 2, NULL);
        if (rv != 0) {
            fprintf(stderr, "[WT] submit_response error: %s\n",
                    nghttp3_strerror(rv));
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
        fprintf(stderr, "[WT] WebTransport session established!\n");
        return 0;
    }

    /* ── WebSocket Extended CONNECT (RFC 9220) ── */
    if (strcmp(s->method, "CONNECT") == 0 &&
        strcmp(s->protocol, "websocket") == 0) {
        fprintf(stderr, "[WS] WebSocket-over-H3 request on stream %lld path=%s\n",
                (long long)stream_id, s->path);
        s->type = STREAM_TYPE_WS;

        /* Accept with 200 */
        nghttp3_nv nva[] = {
            { (uint8_t *)":status", (uint8_t *)"200", 7, 3, NGHTTP3_NV_FLAG_NONE },
        };
        int rv = nghttp3_conn_submit_response(sc->h3conn, stream_id,
                                              nva, 1, NULL);
        if (rv != 0) {
            fprintf(stderr, "[WS] submit_response error: %s\n",
                    nghttp3_strerror(rv));
            return NGHTTP3_ERR_CALLBACK_FAILURE;
        }
        fprintf(stderr, "[WS] WebSocket session established — echoing\n");
        return 0;
    }

    /* ── Regular HTTP/3 GET request ── */
    if (strcmp(s->method, "GET") == 0) {
        if (strcmp(s->path, "/.well-known/webtransport") == 0 ||
            strcmp(s->path, "/") == 0) {
            submit_response(sc, stream_id, 200, "text/plain", NULL, 0);
        } else {
            submit_response(sc, stream_id, 404, "text/plain", NULL, 0);
        }
        return 0;
    }

    /* Default: 405 */
    submit_response(sc, stream_id, 405, NULL, NULL, 0);
    return 0;
}

static int h3_end_stream(nghttp3_conn *conn, int64_t stream_id,
                         void *conn_user_data, void *stream_user_data) {
    server_conn *sc = (server_conn *)conn_user_data;
    stream_data *s = (stream_data *)stream_user_data;
    (void)conn;
    if (!s) s = find_stream(sc, stream_id);
    if (s) s->fin_received = 1;

    fprintf(stderr, "[H3] end_stream stream=%lld\n", (long long)stream_id);
    return 0;
}

static int h3_stop_sending(nghttp3_conn *conn, int64_t stream_id,
                           uint64_t app_error_code, void *conn_user_data,
                           void *stream_user_data) {
    server_conn *sc = (server_conn *)conn_user_data;
    (void)conn; (void)stream_user_data;
    ngtcp2_conn_shutdown_stream_read(sc->conn, 0, stream_id, app_error_code);
    return 0;
}

static int h3_reset_stream(nghttp3_conn *conn, int64_t stream_id,
                           uint64_t app_error_code, void *conn_user_data,
                           void *stream_user_data) {
    server_conn *sc = (server_conn *)conn_user_data;
    (void)conn; (void)stream_user_data;
    ngtcp2_conn_shutdown_stream_write(sc->conn, 0, stream_id, app_error_code);
    return 0;
}

static int h3_recv_settings(nghttp3_conn *conn,
                            const nghttp3_proto_settings *settings,
                            void *conn_user_data) {
    (void)conn; (void)conn_user_data;
    fprintf(stderr, "[H3] SETTINGS received: connect_protocol=%d h3_datagram=%d\n",
            settings->enable_connect_protocol, settings->h3_datagram);
    return 0;
}

/* ============================================================
 * ALPN select callback for wolfSSL
 * ============================================================ */

static int alpn_select_cb(WOLFSSL *ssl, const unsigned char **out,
                          unsigned char *outlen, const unsigned char *in,
                          unsigned int inlen, void *arg) {
    (void)ssl; (void)arg;

    /* Prefer h3 for WebTransport support */
    const unsigned char *p = in;
    while (p < in + inlen) {
        unsigned char len = *p++;
        if (len == 2 && memcmp(p, "h3", 2) == 0) {
            *out = p;
            *outlen = 2;
            fprintf(stderr, "[TLS] ALPN selected: h3\n");
            return 0;
        }
        p += len;
    }

    /* Fallback to echo */
    p = in;
    while (p < in + inlen) {
        unsigned char len = *p++;
        if (len == 4 && memcmp(p, "echo", 4) == 0) {
            *out = p;
            *outlen = 4;
            fprintf(stderr, "[TLS] ALPN selected: echo\n");
            return 0;
        }
        p += len;
    }

    fprintf(stderr, "[TLS] ALPN: no matching protocol found!\n");
    return 3; /* SSL_TLSEXT_ERR_ALERT_FATAL */
}

/* ============================================================
 * wolfSSL context setup
 * ============================================================ */

static int setup_ssl_ctx(void) {
    wolfSSL_Init();

    g_ssl_ctx = wolfSSL_CTX_new(wolfTLSv1_3_server_method());
    if (!g_ssl_ctx) {
        fprintf(stderr, "[TLS] wolfSSL_CTX_new failed\n");
        return -1;
    }

    int rv = ngtcp2_crypto_wolfssl_configure_server_context(g_ssl_ctx);
    if (rv != 0) {
        fprintf(stderr, "[TLS] configure_server_context failed: %d\n", rv);
        return -1;
    }

    fprintf(stderr, "[TLS] Loading cert (%d bytes) and key (%d bytes)...\n",
            cert_der_len, key_der_len);

    int cert_rv = wolfSSL_CTX_use_certificate_buffer(g_ssl_ctx, cert_der,
            cert_der_len, SSL_FILETYPE_ASN1);
    if (cert_rv != SSL_SUCCESS) {
        fprintf(stderr, "[TLS] use_certificate_buffer failed: %d\n", cert_rv);
        return -1;
    }
    fprintf(stderr, "[TLS] Certificate loaded OK\n");

    int key_rv = wolfSSL_CTX_use_PrivateKey_buffer(g_ssl_ctx, key_der,
            key_der_len, SSL_FILETYPE_ASN1);
    if (key_rv != SSL_SUCCESS) {
        fprintf(stderr, "[TLS] use_PrivateKey_buffer failed: %d\n", key_rv);
        return -1;
    }
    fprintf(stderr, "[TLS] Private key loaded OK\n");

    wolfSSL_CTX_set_alpn_select_cb(g_ssl_ctx, alpn_select_cb, NULL);
    fprintf(stderr, "[TLS] SSL context configured\n");
    return 0;
}

/* ============================================================
 * Setup nghttp3 HTTP/3 connection
 * ============================================================ */

static int setup_h3_connection(server_conn *sc) {
    if (sc->h3conn) return 0;

    /* Need at least 3 uni streams for H3 control + QPACK enc/dec */
    if (ngtcp2_conn_get_streams_uni_left(sc->conn) < 3) {
        fprintf(stderr, "[H3] Peer doesn't allow enough uni streams\n");
        return -1;
    }

    nghttp3_callbacks callbacks = {
        .acked_stream_data = h3_acked_stream_data,
        .recv_data         = h3_recv_data,
        .deferred_consume  = h3_deferred_consume,
        .begin_headers     = h3_begin_headers,
        .recv_header       = h3_recv_header,
        .end_headers       = h3_end_headers,
        .end_stream        = h3_end_stream,
        .stop_sending      = h3_stop_sending,
        .reset_stream      = h3_reset_stream,
        .recv_settings2    = h3_recv_settings,
    };

    nghttp3_settings settings;
    nghttp3_settings_default(&settings);
    settings.qpack_max_dtable_capacity = 4096;
    settings.qpack_blocked_streams = 100;

    /* Enable WebTransport + WebSocket over H3 */
    settings.enable_connect_protocol = 1;  /* RFC 9220 Extended CONNECT */
    settings.h3_datagram = 1;              /* RFC 9297 HTTP/3 Datagrams */

    nghttp3_mem *mem = nghttp3_mem_default();

    int rv = nghttp3_conn_server_new(&sc->h3conn, &callbacks, &settings,
                                     mem, sc);
    if (rv != 0) {
        fprintf(stderr, "[H3] conn_server_new failed: %s\n", nghttp3_strerror(rv));
        return -1;
    }

    const ngtcp2_transport_params *params =
        ngtcp2_conn_get_local_transport_params(sc->conn);
    nghttp3_conn_set_max_client_streams_bidi(sc->h3conn,
                                             params->initial_max_streams_bidi);

    /* Open control stream */
    int64_t ctrl_stream_id;
    rv = ngtcp2_conn_open_uni_stream(sc->conn, &ctrl_stream_id, NULL);
    if (rv != 0) {
        fprintf(stderr, "[H3] open control stream failed: %s\n", ngtcp2_strerror(rv));
        return -1;
    }
    rv = nghttp3_conn_bind_control_stream(sc->h3conn, ctrl_stream_id);
    if (rv != 0) {
        fprintf(stderr, "[H3] bind_control_stream failed: %s\n", nghttp3_strerror(rv));
        return -1;
    }
    fprintf(stderr, "[H3] Control stream: %lld\n", (long long)ctrl_stream_id);

    /* Open QPACK encoder/decoder streams */
    int64_t qenc_stream_id, qdec_stream_id;
    rv = ngtcp2_conn_open_uni_stream(sc->conn, &qenc_stream_id, NULL);
    if (rv != 0) return -1;
    rv = ngtcp2_conn_open_uni_stream(sc->conn, &qdec_stream_id, NULL);
    if (rv != 0) return -1;

    rv = nghttp3_conn_bind_qpack_streams(sc->h3conn,
                                         qenc_stream_id, qdec_stream_id);
    if (rv != 0) {
        fprintf(stderr, "[H3] bind_qpack_streams failed: %s\n", nghttp3_strerror(rv));
        return -1;
    }
    fprintf(stderr, "[H3] QPACK streams: enc=%lld dec=%lld\n",
            (long long)qenc_stream_id, (long long)qdec_stream_id);

    fprintf(stderr, "[H3] HTTP/3 connection established (WebTransport + RFC 9220 enabled)\n");
    return 0;
}

/* ============================================================
 * Connection write loop
 * ============================================================ */

static int write_streams(server_conn *sc) {
    uint8_t txbuf[MAX_UDP_PAYLOAD];
    ngtcp2_path_storage ps;
    ngtcp2_pkt_info pi;
    ngtcp2_tstamp ts = timestamp_ns();

    ngtcp2_path_storage_zero(&ps);

    for (;;) {
        int64_t stream_id = -1;
        ngtcp2_vec datav = {NULL, 0};
        size_t datavcnt = 0;
        uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
        int fin = 0;

        if (sc->proto == PROTO_H3 && sc->h3conn) {
            /* Let nghttp3 decide what to write */
            nghttp3_vec h3vec[16];
            nghttp3_ssize sveccnt = nghttp3_conn_writev_stream(
                sc->h3conn, &stream_id, &fin, h3vec, 16);
            if (sveccnt < 0) {
                fprintf(stderr, "[H3] writev_stream error: %s\n",
                        nghttp3_strerror((int)sveccnt));
                return -1;
            }
            if (sveccnt > 0) {
                /* Use first vec for simplicity (could coalesce) */
                datav.base = h3vec[0].base;
                datav.len = h3vec[0].len;
                datavcnt = 1;
            }
            if (fin) flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
        } else {
            /* Raw echo mode — find stream with pending data */
            for (stream_data *s = sc->streams; s; s = s->next) {
                if (s->sendoff < s->sendlen || s->fin_received) {
                    stream_id = s->stream_id;
                    if (s->sendoff < s->sendlen) {
                        datav.base = s->sendbuf + s->sendoff;
                        datav.len = s->sendlen - s->sendoff;
                        datavcnt = 1;
                    }
                    if (s->fin_received && s->sendoff >= s->sendlen) {
                        flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
                    }
                    break;
                }
            }
        }

        ngtcp2_ssize ndatalen = 0;
        ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(
            sc->conn, &ps.path, &pi,
            txbuf, sizeof(txbuf),
            &ndatalen, flags,
            stream_id,
            datavcnt > 0 ? &datav : NULL, datavcnt,
            ts);

        if (nwrite < 0) {
            if (nwrite == NGTCP2_ERR_WRITE_MORE) {
                if (sc->h3conn && ndatalen >= 0) {
                    nghttp3_conn_add_write_offset(sc->h3conn, stream_id,
                                                  (uint64_t)ndatalen);
                } else if (sc->proto == PROTO_ECHO) {
                    stream_data *s = find_stream(sc, stream_id);
                    if (s && ndatalen > 0) s->sendoff += (size_t)ndatalen;
                }
                continue;
            }
            if (nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
                if (sc->h3conn) {
                    nghttp3_conn_block_stream(sc->h3conn, stream_id);
                }
                continue;
            }
            if (nwrite == NGTCP2_ERR_STREAM_SHUT_WR) {
                if (sc->h3conn) {
                    nghttp3_conn_shutdown_stream_write(sc->h3conn, stream_id);
                }
                continue;
            }
            fprintf(stderr, "[QUIC] writev_stream error: %s\n",
                    ngtcp2_strerror((int)nwrite));
            return -1;
        }

        if (nwrite == 0) break;

        if (sc->h3conn && ndatalen >= 0) {
            nghttp3_conn_add_write_offset(sc->h3conn, stream_id,
                                          (uint64_t)ndatalen);
        } else if (sc->proto == PROTO_ECHO) {
            stream_data *s = find_stream(sc, stream_id);
            if (s && ndatalen > 0) s->sendoff += (size_t)ndatalen;
        }

        /* Send the UDP packet */
        ssize_t sent = sendto(sc->fd, txbuf, (size_t)nwrite, 0,
                              (struct sockaddr *)&sc->remote_addr,
                              sc->remote_addrlen);
        if (sent < 0) {
            fprintf(stderr, "[UDP] sendto error: %s\n", strerror(errno));
        }

        if (stream_id == -1) break;
    }

    ngtcp2_conn_update_pkt_tx_time(sc->conn, ts);
    return 0;
}

/* ============================================================
 * Create a new QUIC server connection
 * ============================================================ */

static server_conn *create_server_conn(int fd,
                                       const ngtcp2_pkt_hd *hd,
                                       const struct sockaddr *local_addr,
                                       socklen_t local_addrlen,
                                       const struct sockaddr *remote_addr,
                                       socklen_t remote_addrlen,
                                       const uint8_t *pkt, size_t pktlen) {
    server_conn *sc = calloc(1, sizeof(server_conn));
    if (!sc) return NULL;

    sc->fd = fd;
    sc->wt_session_stream = -1;
    memcpy(&sc->local_addr, local_addr, local_addrlen);
    sc->local_addrlen = local_addrlen;
    memcpy(&sc->remote_addr, remote_addr, remote_addrlen);
    sc->remote_addrlen = remote_addrlen;

    ngtcp2_ccerr_default(&sc->last_error);

    sc->conn_ref.get_conn = get_conn_cb;
    sc->conn_ref.user_data = sc;

    /* Generate server SCID */
    ngtcp2_cid scid;
    WC_RNG rng;
    wc_InitRng(&rng);
    wc_RNG_GenerateBlock(&rng, scid.data, SCID_LEN);
    scid.datalen = SCID_LEN;
    wc_FreeRng(&rng);

    /* Callbacks */
    ngtcp2_callbacks callbacks = {0};
    callbacks.recv_client_initial      = ngtcp2_crypto_recv_client_initial_cb;
    callbacks.recv_crypto_data         = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks.encrypt                  = ngtcp2_crypto_encrypt_cb;
    callbacks.decrypt                  = ngtcp2_crypto_decrypt_cb;
    callbacks.hp_mask                  = ngtcp2_crypto_hp_mask_cb;
    callbacks.update_key               = ngtcp2_crypto_update_key_cb;
    callbacks.delete_crypto_aead_ctx   = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    callbacks.get_path_challenge_data  = ngtcp2_crypto_get_path_challenge_data_cb;
    callbacks.version_negotiation      = ngtcp2_crypto_version_negotiation_cb;
    callbacks.handshake_completed      = handshake_completed_cb;
    callbacks.recv_stream_data         = recv_stream_data_cb;
    callbacks.stream_open              = stream_open_cb;
    callbacks.stream_close             = stream_close_cb;
    callbacks.stream_reset             = stream_reset_cb;
    callbacks.stream_stop_sending      = stream_stop_sending_cb;
    callbacks.acked_stream_data_offset = acked_stream_data_offset_cb;
    callbacks.recv_datagram            = recv_datagram_cb;
    callbacks.rand                     = rand_cb;
    callbacks.get_new_connection_id    = get_new_connection_id_cb;
    callbacks.remove_connection_id     = remove_connection_id_cb;
    callbacks.extend_max_remote_streams_bidi = extend_max_remote_streams_bidi_cb;

    /* Settings */
    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = timestamp_ns();
    settings.log_printf = NULL;

    /* Transport params */
    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local  = 256 * 1024;
    params.initial_max_stream_data_bidi_remote = 256 * 1024;
    params.initial_max_stream_data_uni         = 256 * 1024;
    params.initial_max_data                    = 1 * 1024 * 1024;
    params.initial_max_streams_bidi            = 100;
    params.initial_max_streams_uni             = 10;  /* need >=3 for H3 + extras for WT */
    params.max_idle_timeout                    = 30 * NGTCP2_SECONDS;
    params.active_connection_id_limit          = 7;

    /* Enable DATAGRAM frames for WebTransport */
    params.max_datagram_frame_size = 65535;

    params.original_dcid = hd->dcid;
    params.original_dcid_present = 1;

    params.stateless_reset_token_present = 1;
    ngtcp2_crypto_generate_stateless_reset_token(
        params.stateless_reset_token, static_secret, sizeof(static_secret), &scid);

    /* Path */
    ngtcp2_path path;
    ngtcp2_addr_init(&path.local, local_addr, local_addrlen);
    ngtcp2_addr_init(&path.remote, remote_addr, remote_addrlen);
    path.user_data = NULL;

    int rv = ngtcp2_conn_server_new(&sc->conn, &hd->scid, &scid, &path,
                                    hd->version, &callbacks, &settings,
                                    &params, NULL, sc);
    if (rv != 0) {
        fprintf(stderr, "[QUIC] conn_server_new failed: %s\n", ngtcp2_strerror(rv));
        free(sc);
        return NULL;
    }

    /* Create TLS session */
    sc->ssl = wolfSSL_new(g_ssl_ctx);
    if (!sc->ssl) {
        fprintf(stderr, "[TLS] wolfSSL_new failed\n");
        ngtcp2_conn_del(sc->conn);
        free(sc);
        return NULL;
    }
    wolfSSL_set_app_data(sc->ssl, &sc->conn_ref);
    wolfSSL_set_accept_state(sc->ssl);

    ngtcp2_conn_set_tls_native_handle(sc->conn, sc->ssl);

    /* Feed initial packet */
    ngtcp2_pkt_info pi = {0};
    rv = ngtcp2_conn_read_pkt(sc->conn, &path, &pi, pkt, pktlen, timestamp_ns());
    if (rv != 0) {
        fprintf(stderr, "[QUIC] Initial read_pkt failed: %s\n", ngtcp2_strerror(rv));
        wolfSSL_free(sc->ssl);
        ngtcp2_conn_del(sc->conn);
        free(sc);
        return NULL;
    }

    /* Determine protocol from ALPN */
    const uint8_t *alpn_data;
    unsigned int alpn_len;
    wolfSSL_ALPN_GetProtocol(sc->ssl, (char **)&alpn_data, (unsigned short *)&alpn_len);
    if (alpn_data && alpn_len == 2 && memcmp(alpn_data, "h3", 2) == 0) {
        sc->proto = PROTO_H3;
        fprintf(stderr, "[QUIC] Protocol: HTTP/3 (WebTransport + RFC 9220 enabled)\n");
    } else {
        sc->proto = PROTO_ECHO;
        fprintf(stderr, "[QUIC] Protocol: Raw echo\n");
    }

    /* Send handshake response */
    write_streams(sc);

    fprintf(stderr, "[QUIC] New connection created (scid=%02x%02x%02x%02x...)\n",
           scid.data[0], scid.data[1], scid.data[2], scid.data[3]);
    return sc;
}

/* ============================================================
 * Destroy a connection
 * ============================================================ */

static void destroy_server_conn(server_conn *sc) {
    if (!sc) return;

    stream_data *s = sc->streams;
    while (s) {
        stream_data *next = s->next;
        free(s);
        s = next;
    }

    if (sc->h3conn) nghttp3_conn_del(sc->h3conn);
    if (sc->ssl) wolfSSL_free(sc->ssl);
    if (sc->conn) ngtcp2_conn_del(sc->conn);
    free(sc);
}

/* ============================================================
 * Handle incoming UDP packet
 * ============================================================ */

static int handle_packet(int fd,
                         const struct sockaddr *local_addr, socklen_t local_addrlen,
                         const struct sockaddr *remote_addr, socklen_t remote_addrlen,
                         const uint8_t *pkt, size_t pktlen) {
    ngtcp2_version_cid vc;
    int rv = ngtcp2_pkt_decode_version_cid(&vc, pkt, pktlen, SCID_LEN);
    if (rv == NGTCP2_ERR_VERSION_NEGOTIATION) {
        fprintf(stderr, "[QUIC] Version negotiation needed (not implemented)\n");
        return 0;
    }
    if (rv < 0) {
        fprintf(stderr, "[QUIC] pkt_decode_version_cid: %s\n", ngtcp2_strerror(rv));
        return 0;
    }

    /* Check existing connection */
    if (g_sconn) {
        ngtcp2_cid scids[8];
        size_t nscids = ngtcp2_conn_get_scid(g_sconn->conn, scids);
        for (size_t i = 0; i < nscids; i++) {
            if (scids[i].datalen == vc.dcidlen &&
                memcmp(scids[i].data, vc.dcid, vc.dcidlen) == 0) {
                if (ngtcp2_conn_in_closing_period(g_sconn->conn) ||
                    ngtcp2_conn_in_draining_period(g_sconn->conn)) {
                    return 0;
                }

                ngtcp2_path path;
                ngtcp2_addr_init(&path.local, local_addr, local_addrlen);
                ngtcp2_addr_init(&path.remote, remote_addr, remote_addrlen);
                path.user_data = NULL;

                ngtcp2_pkt_info pi = {0};
                rv = ngtcp2_conn_read_pkt(g_sconn->conn, &path, &pi,
                                          pkt, pktlen, timestamp_ns());
                if (rv != 0) {
                    fprintf(stderr, "[QUIC] read_pkt error: %s\n", ngtcp2_strerror(rv));
                    if (rv == NGTCP2_ERR_DRAINING) return 0;
                    return -1;
                }

                /* Setup H3 layer after handshake (ALPN is known) */
                if (g_sconn->handshake_done && g_sconn->proto == PROTO_H3 &&
                    !g_sconn->h3conn) {
                    if (setup_h3_connection(g_sconn) != 0) {
                        fprintf(stderr, "[H3] Failed to setup HTTP/3 layer\n");
                    }
                }

                write_streams(g_sconn);
                return 0;
            }
        }
    }

    /* New connection */
    ngtcp2_pkt_hd hd;
    rv = ngtcp2_accept(&hd, pkt, pktlen);
    if (rv < 0) {
        fprintf(stderr, "[QUIC] Not a valid Initial packet, ignoring\n");
        return 0;
    }

    if (g_sconn) {
        fprintf(stderr, "[QUIC] Already have a connection, ignoring new Initial\n");
        return 0;
    }

    fprintf(stderr, "[QUIC] Accepting new connection from client\n");
    g_sconn = create_server_conn(fd, &hd,
                                  local_addr, local_addrlen,
                                  remote_addr, remote_addrlen,
                                  pkt, pktlen);
    if (!g_sconn) {
        fprintf(stderr, "[QUIC] Failed to create connection\n");
        return -1;
    }

    return 0;
}

/* ============================================================
 * Main event loop
 * ============================================================ */

int main(void) {
    fprintf(stderr, "=== QUIC Echo Server with WebTransport + RFC 9220 ===\n\n");

    /* Generate static secret */
    {
        WC_RNG rng;
        wc_InitRng(&rng);
        wc_RNG_GenerateBlock(&rng, static_secret, sizeof(static_secret));
        wc_FreeRng(&rng);
    }

    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    write(2, "Starting...\n", 12);

    fprintf(stderr, "[CERT] Using pre-generated certificate (%d bytes)\n", cert_der_len);
    fflush(stderr);
    fprintf(stderr, "[CERT] Key: %d bytes\n", key_der_len);
    fflush(stderr);

    if (setup_ssl_ctx() != 0) {
        fprintf(stderr, "FATAL: TLS context setup failed\n");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "FATAL: socket() failed: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(SERVER_PORT);
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        fprintf(stderr, "FATAL: bind() failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    fprintf(stderr, "[UDP] Listening on 0.0.0.0:%d\n", SERVER_PORT);
    fprintf(stderr, "[UDP] Supported protocols:\n");
    fprintf(stderr, "[UDP]   - ALPN 'echo': Raw QUIC echo\n");
    fprintf(stderr, "[UDP]   - ALPN 'h3': HTTP/3 + WebTransport + WebSocket (RFC 9220)\n");
    fprintf(stderr, "[UDP] Waiting for QUIC connections...\n\n");

    struct sockaddr_storage local_addr;
    socklen_t local_addrlen = sizeof(local_addr);
    getsockname(fd, (struct sockaddr *)&local_addr, &local_addrlen);

    uint8_t rxbuf[65536];
    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    for (;;) {
        int timeout_ms = 1000;
        if (g_sconn) {
            ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(g_sconn->conn);
            ngtcp2_tstamp now = timestamp_ns();
            if (expiry <= now) {
                timeout_ms = 0;
            } else if (expiry != UINT64_MAX) {
                uint64_t delta = (expiry - now) / 1000000ULL;
                if (delta > 1000) delta = 1000;
                timeout_ms = (int)delta;
            }
        }

        int nready = poll(&pfd, 1, timeout_ms);

        if (nready < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[POLL] error: %s\n", strerror(errno));
            break;
        }

        /* Handle timer expiry */
        if (g_sconn) {
            ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(g_sconn->conn);
            if (expiry <= timestamp_ns()) {
                int rv = ngtcp2_conn_handle_expiry(g_sconn->conn, timestamp_ns());
                if (rv == NGTCP2_ERR_IDLE_CLOSE) {
                    fprintf(stderr, "[QUIC] Idle timeout — closing connection\n");
                    destroy_server_conn(g_sconn);
                    g_sconn = NULL;
                } else if (rv != 0) {
                    fprintf(stderr, "[QUIC] handle_expiry error: %s\n", ngtcp2_strerror(rv));
                    destroy_server_conn(g_sconn);
                    g_sconn = NULL;
                } else {
                    write_streams(g_sconn);
                }
            }
        }

        if (nready == 0) continue;

        struct sockaddr_storage remote_addr;
        socklen_t remote_addrlen = sizeof(remote_addr);
        ssize_t nread = recvfrom(fd, rxbuf, sizeof(rxbuf), 0,
                                 (struct sockaddr *)&remote_addr, &remote_addrlen);
        if (nread < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            fprintf(stderr, "[UDP] recvfrom error: %s\n", strerror(errno));
            continue;
        }

        handle_packet(fd,
                      (struct sockaddr *)&local_addr, local_addrlen,
                      (struct sockaddr *)&remote_addr, remote_addrlen,
                      rxbuf, (size_t)nread);

        if (g_sconn && (ngtcp2_conn_in_closing_period(g_sconn->conn) ||
                        ngtcp2_conn_in_draining_period(g_sconn->conn))) {
            fprintf(stderr, "[QUIC] Connection closing/draining, cleaning up\n");
            destroy_server_conn(g_sconn);
            g_sconn = NULL;
        }
    }

    close(fd);
    wolfSSL_CTX_free(g_ssl_ctx);
    wolfSSL_Cleanup();
    return 0;
}
