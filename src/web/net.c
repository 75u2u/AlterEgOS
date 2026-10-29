/* The HTTP client: one request per connection (Connection: close), HTTP/1.1
 * with chunked and Content-Length bodies, up to 5 redirects, HTTPS through
 * mbedTLS when built with it (HAVE_TLS). Bodies are read whole; the
 * browser shows pages, not streams. */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "web.h"

#ifdef HAVE_TLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>
#endif

#define CONNECT_TIMEOUT_MS 10000
#define READ_TIMEOUT_S     20
#define MAX_REDIRECTS      5
#define UA "AlterEgOS-web/0.1 (text browser; +https://github.com/75u2u/AlterEgOS)"

#ifndef CA_BUNDLE
#define CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"
#endif

int net_has_tls(void)
{
#ifdef HAVE_TLS
    return 1;
#else
    return 0;
#endif
}

void resp_free(struct resp *r)
{
    free(r->body);
    r->body = NULL;
    r->len = 0;
}

/* ---- the connection: a socket, optionally wrapped in TLS */

struct conn {
    int fd;
#ifdef HAVE_TLS
    int tls;
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
#endif
};

static int connect_to(const char *host, int port, char *err, size_t ecap)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res = NULL, *ai;
    char service[16];
    snprintf(service, sizeof service, "%d", port);
    int rc = getaddrinfo(host, service, &hints, &res);
    if (rc != 0) { snprintf(err, ecap, "%s が見つかりません (DNS: %s)", host, gai_strerror(rc)); return -1; }
    int fd = -1;
    const char *why = "接続できません";
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;
        int fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            struct pollfd p = { .fd = fd, .events = POLLOUT };
            rc = poll(&p, 1, CONNECT_TIMEOUT_MS);
            if (rc == 0) { why = "接続がタイムアウトしました"; rc = -1; }
            else if (rc > 0) {
                int soerr = 0;
                socklen_t sl = sizeof soerr;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                if (soerr) { errno = soerr; rc = -1; why = strerror(soerr); }
                else rc = 0;
            }
        } else if (rc < 0) why = strerror(errno);
        if (rc == 0) {
            fcntl(fd, F_SETFL, fl);
            struct timeval tv = { .tv_sec = READ_TIMEOUT_S };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) snprintf(err, ecap, "%s:%d に%s", host, port, why);
    return fd;
}

static void conn_close(struct conn *c)
{
#ifdef HAVE_TLS
    if (c->tls) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_x509_crt_free(&c->ca);
        mbedtls_ctr_drbg_free(&c->drbg);
        mbedtls_entropy_free(&c->entropy);
    }
#endif
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

#ifdef HAVE_TLS
static void tls_err(int rc, const char *what, char *err, size_t ecap)
{
    char buf[128];
    mbedtls_strerror(rc, buf, sizeof buf);
    snprintf(err, ecap, "%s: %s", what, buf);
}

/* Wrap c->fd in TLS; verifies the certificate against the CA bundle when
 * one can be read (ALTEREGO_CA or the system bundle), else only warns. */
static int tls_start(struct conn *c, const char *host, int *insecure, char *err, size_t ecap)
{
    int rc;
    c->tls = 1;
    mbedtls_net_init(&c->net);
    c->net.fd = c->fd;
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->ca);
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->drbg);
#if MBEDTLS_VERSION_MAJOR >= 3 && defined(MBEDTLS_USE_PSA_CRYPTO)
    psa_crypto_init();
#endif
    const char *pers = "alterego-web";
    if ((rc = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy, (const unsigned char *)pers, strlen(pers))) != 0) {
        tls_err(rc, "乱数の初期化に失敗", err, ecap);
        return -1;
    }
    if ((rc = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        tls_err(rc, "TLS の設定に失敗", err, ecap);
        return -1;
    }
    const char *bundle = getenv("ALTEREGO_CA");
    if (!bundle) bundle = CA_BUNDLE;
    if (mbedtls_x509_crt_parse_file(&c->ca, bundle) >= 0 && c->ca.version != 0) {
        mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
        *insecure = 1;
    }
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    if ((rc = mbedtls_ssl_setup(&c->ssl, &c->conf)) != 0) { tls_err(rc, "TLS の準備に失敗", err, ecap); return -1; }
    if ((rc = mbedtls_ssl_set_hostname(&c->ssl, host)) != 0) { tls_err(rc, "ホスト名の設定に失敗", err, ecap); return -1; }
    mbedtls_ssl_set_bio(&c->ssl, &c->net, mbedtls_net_send, mbedtls_net_recv, NULL);
    while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            char vrfy[256];
            mbedtls_x509_crt_verify_info(vrfy, sizeof vrfy, "", mbedtls_ssl_get_verify_result(&c->ssl));
            vrfy[strcspn(vrfy, "\n")] = '\0';
            snprintf(err, ecap, "%s の証明書を信頼できません (%s)", host, vrfy);
        } else tls_err(rc, "TLS の接続に失敗", err, ecap);
        return -1;
    }
    return 0;
}
#endif

static ssize_t conn_read(struct conn *c, void *buf, size_t n)
{
#ifdef HAVE_TLS
    if (c->tls) {
        for (;;) {
            int rc = mbedtls_ssl_read(&c->ssl, buf, n);
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_NET_CONN_RESET) return 0;
            return rc;
        }
    }
#endif
    return read(c->fd, buf, n);
}

static int conn_write_all(struct conn *c, const char *buf, size_t n)
{
    while (n > 0) {
        ssize_t k;
#ifdef HAVE_TLS
        if (c->tls) {
            int rc = mbedtls_ssl_write(&c->ssl, (const unsigned char *)buf, n);
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            k = rc;
        } else
#endif
        k = write(c->fd, buf, n);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        buf += k;
        n -= (size_t)k;
    }
    return 0;
}

/* ---- the response */

struct buf { char *p; size_t len, cap; };

static int buf_add(struct buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 16384;
        while (cap < b->len + n + 1) cap *= 2;
        if (cap > NET_MAX_BODY + 65536) return -1;
        char *np = realloc(b->p, cap);
        if (!np) return -1;
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 0;
}

static const char *header(const char *hdr, const char *name)
{
    size_t nl = strlen(name);
    for (const char *p = hdr; (p = strcasestr(p, name)) != NULL; p += nl) {
        if ((p == hdr || p[-1] == '\n') && p[nl] == ':') {
            p += nl + 1;
            while (*p == ' ' || *p == '\t') p++;
            return p;
        }
    }
    return NULL;
}

static void header_value(const char *hdr, const char *name, char *out, size_t cap)
{
    const char *v = header(hdr, name);
    if (!v) { out[0] = '\0'; return; }
    snprintf(out, cap, "%.*s", (int)strcspn(v, "\r\n"), v);
}

/* Chunked transfer coding decoded in place. Returns the body length, or the
 * best effort when the stream ended early. */
static size_t dechunk(char *body, size_t n)
{
    size_t in = 0, out = 0;
    while (in < n) {
        char *end;
        unsigned long sz = strtoul(body + in, &end, 16);
        if (end == body + in) break;
        char *crlf = strstr(end, "\r\n");
        if (!crlf) break;
        in = (size_t)(crlf - body) + 2;
        if (sz == 0) break;
        if (in + sz > n) sz = n - in;
        memmove(body + out, body + in, sz);
        out += sz;
        in += sz;
        if (in + 2 <= n && body[in] == '\r') in += 2;
    }
    body[out] = '\0';
    return out;
}

static int fetch_once(const struct url *u, struct resp *r, char *location, size_t lcap)
{
    struct conn c;
    memset(&c, 0, sizeof c);
    int https = strcmp(u->scheme, "https") == 0;
    if (!https && strcmp(u->scheme, "http") != 0) { snprintf(r->err, sizeof r->err, "%s: は扱えません", u->scheme); return -1; }
    if (https && !net_has_tls()) { snprintf(r->err, sizeof r->err, "この alterego-web は TLS なしでビルドされています (https は開けません)"); return -1; }
    int port = u->port ? u->port : (https ? 443 : 80);
    c.fd = connect_to(u->host, port, r->err, sizeof r->err);
    if (c.fd < 0) return -1;
#ifdef HAVE_TLS
    if (https && tls_start(&c, u->host, &r->insecure, r->err, sizeof r->err) < 0) { conn_close(&c); return -1; }
#endif
    char req[8192];
    int n;
    if (u->port) n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: " UA "\r\n"
                     "Accept: text/html, text/plain;q=0.9, */*;q=0.5\r\nAccept-Language: ja, en;q=0.5\r\n"
                     "Accept-Encoding: identity\r\nConnection: close\r\n\r\n", u->path, u->host, u->port);
    else n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " UA "\r\n"
                     "Accept: text/html, text/plain;q=0.9, */*;q=0.5\r\nAccept-Language: ja, en;q=0.5\r\n"
                     "Accept-Encoding: identity\r\nConnection: close\r\n\r\n", u->path, u->host);
    if (n < 0 || n >= (int)sizeof req || conn_write_all(&c, req, (size_t)n) < 0) {
        snprintf(r->err, sizeof r->err, "要求を送れません: %s", strerror(errno));
        conn_close(&c);
        return -1;
    }
    struct buf b = { 0 };
    char chunk[16384];
    size_t hdr_end = 0;
    long want = -1;                                   /* Content-Length once the header is in */
    for (;;) {
        ssize_t k = conn_read(&c, chunk, sizeof chunk);
        if (k < 0) {
            if (errno == EINTR) continue;
            if (b.len == 0) { snprintf(r->err, sizeof r->err, "応答を受け取れません: %s", strerror(errno)); conn_close(&c); free(b.p); return -1; }
            break;                                    /* a timeout after some data: show what came */
        }
        if (k == 0) break;
        if (buf_add(&b, chunk, (size_t)k) < 0) { snprintf(r->err, sizeof r->err, "ページが大きすぎます"); conn_close(&c); free(b.p); return -1; }
        if (!hdr_end) {
            char *e = strstr(b.p, "\r\n\r\n");
            if (e) {
                hdr_end = (size_t)(e - b.p) + 4;
                const char *cl = header(b.p, "Content-Length");
                if (cl) want = atol(cl);
            }
        }
        if (hdr_end && want >= 0 && b.len - hdr_end >= (size_t)want) break;
    }
    conn_close(&c);
    if (!b.p || strncmp(b.p, "HTTP/1.", 7) != 0 || !hdr_end) {
        snprintf(r->err, sizeof r->err, "HTTP の応答ではありません");
        free(b.p);
        return -1;
    }
    r->status = atoi(b.p + 9);
    b.p[hdr_end - 2] = '\0';                          /* headers as a NUL-terminated string */
    header_value(b.p, "Content-Type", r->ctype, sizeof r->ctype);
    for (char *p = r->ctype; *p; p++) *p = (char)tolower((unsigned char)*p);
    header_value(b.p, "Location", location, lcap);
    const char *te = header(b.p, "Transfer-Encoding");
    size_t len = b.len - hdr_end;
    char *body = b.p + hdr_end;
    if (te && strncasecmp(te, "chunked", 7) == 0) len = dechunk(body, len);
    else if (want >= 0 && (size_t)want < len) len = (size_t)want;
    r->body = malloc(len + 1);
    if (!r->body) { free(b.p); snprintf(r->err, sizeof r->err, "メモリ不足"); return -1; }
    memcpy(r->body, body, len);
    r->body[len] = '\0';
    r->len = len;
    free(b.p);
    return 0;
}

int net_fetch(const char *url, struct resp *r)
{
    memset(r, 0, sizeof *r);
    snprintf(r->url, sizeof r->url, "%s", url);
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        struct url u;
        if (url_parse(r->url, &u) < 0) { snprintf(r->err, sizeof r->err, "URL がおかしいです: %s", r->url); return -1; }
        char location[4096] = "";
        if (fetch_once(&u, r, location, sizeof location) < 0) return -1;
        int redirect = r->status == 301 || r->status == 302 || r->status == 303 || r->status == 307 || r->status == 308;
        if (!redirect || !location[0]) return 0;
        char next[4096];
        url_resolve(r->url, location, next, sizeof next);
        resp_free(r);
        snprintf(r->url, sizeof r->url, "%s", next);
    }
    snprintf(r->err, sizeof r->err, "転送が多すぎます");
    return -1;
}
