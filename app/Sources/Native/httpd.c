// httpd.c — minimal HTTP/1.1 server for the LAN debug endpoint.
#include "httpd.h"
#include "rl_log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static httpd_handler g_handler;
static int g_listen_fd = -1;
static uint64_t g_requests;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int fd;
    struct sockaddr_storage peer;
} conn_t;

static int v4_is_lan(uint32_t a) {
    return (a >> 24) == 10 || (a >> 24) == 127 || (a >> 20) == 0xAC1 ||
           (a >> 16) == 0xC0A8 || (a >> 16) == 0xA9FE || (a >> 10) == (0x64400000u >> 10);
}

static int peer_is_lan(const struct sockaddr_storage *ss) {
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)ss;
        return v4_is_lan(ntohl(in->sin_addr.s_addr));
    }
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)ss;
        const uint8_t *b = in6->sin6_addr.s6_addr;
        if (IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr)) return 1;
        if (IN6_IS_ADDR_LINKLOCAL(&in6->sin6_addr)) return 1;
        if ((b[0] & 0xFE) == 0xFC) return 1;  // fc00::/7 ULA
        if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
            uint32_t a = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) | ((uint32_t)b[14] << 8) | b[15];
            return v4_is_lan(a);
        }
    }
    return 0;
}

static const char *reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Status";
    }
}

void httpd_set_response(httpd_response *r, int status, const char *content_type,
                        const void *data, size_t len) {
    if (!r) return;
    if (r->body) { free(r->body); r->body = NULL; }
    r->status = status;
    snprintf(r->content_type, sizeof r->content_type, "%s",
             content_type ? content_type : "application/octet-stream");
    r->body_len = 0;
    if (len > 0 && data) {
        r->body = malloc(len);
        if (r->body) { memcpy(r->body, data, len); r->body_len = len; }
    }
}

static int send_all(int fd, const void *p, size_t n) {
    const char *c = (const char *)p;
    while (n > 0) {
        ssize_t w = send(fd, c, n, 0);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        if (w == 0) return -1;
        c += w;
        n -= (size_t)w;
    }
    return 0;
}

static void send_simple(int fd, int status, const char *text) {
    char hdr[512];
    size_t tl = text ? strlen(text) : 0;
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
                     status, reason(status), tl);
    if (send_all(fd, hdr, (size_t)n) == 0 && tl) send_all(fd, text, tl);
}

static void *conn_thread(void *arg) {
    conn_t *c = (conn_t *)arg;
    int fd = c->fd;
    struct sockaddr_storage peer = c->peer;
    free(c);

    struct timeval tv = {15, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    tv.tv_sec = 60;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    if (!peer_is_lan(&peer)) {
        send_simple(fd, 403, "LAN only\n");
        close(fd);
        return NULL;
    }

    const size_t cap = 64 * 1024;
    char *buf = malloc(cap + 1);
    char *body = NULL;
    if (!buf) { close(fd); return NULL; }
    size_t len = 0;
    char *hdr_end = NULL;
    for (;;) {
        ssize_t r = recv(fd, buf + len, cap - len, 0);
        if (r <= 0) goto done;
        len += (size_t)r;
        buf[len] = 0;
        hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) break;
        if (len >= cap) { send_simple(fd, 413, "headers too large\n"); goto done; }
    }
    size_t hdr_len = (size_t)(hdr_end - buf) + 4;

    char method[16] = {0};
    char *target = malloc(cap);
    if (!target) goto done;
    target[0] = 0;
    {
        // request line: METHOD SP TARGET SP VERSION
        const char *sp1 = strchr(buf, ' ');
        if (!sp1 || (size_t)(sp1 - buf) >= sizeof method) { send_simple(fd, 400, "bad request line\n"); free(target); goto done; }
        memcpy(method, buf, (size_t)(sp1 - buf));
        method[sp1 - buf] = 0;
        const char *sp2 = strchr(sp1 + 1, ' ');
        const char *eol = strstr(sp1 + 1, "\r\n");
        if (!sp2 || !eol || sp2 > eol) { send_simple(fd, 400, "bad request line\n"); free(target); goto done; }
        size_t tl = (size_t)(sp2 - (sp1 + 1));
        memcpy(target, sp1 + 1, tl);
        target[tl] = 0;
    }

    size_t clen = 0;
    {
        const char *p = buf;
        while ((p = strcasestr(p, "\r\ncontent-length:")) != NULL && p < hdr_end) {
            clen = strtoul(p + 17, NULL, 10);
            break;
        }
    }
    if (clen > 128u * 1024u * 1024u) { send_simple(fd, 413, "body too large\n"); free(target); goto done; }

    body = malloc(clen + 1);
    if (!body) { send_simple(fd, 500, "oom\n"); free(target); goto done; }
    size_t have = len - hdr_len;
    if (have > clen) have = clen;
    memcpy(body, buf + hdr_len, have);
    while (have < clen) {
        ssize_t r = recv(fd, body + have, clen - have, 0);
        if (r <= 0) { free(target); goto done; }
        have += (size_t)r;
    }
    body[clen] = 0;

    char *query = strchr(target, '?');
    if (query) { *query = 0; query++; } else query = target + strlen(target);  // ""

    httpd_response resp;
    memset(&resp, 0, sizeof resp);
    resp.status = 404;
    snprintf(resp.content_type, sizeof resp.content_type, "text/plain; charset=utf-8");
    if (g_handler) g_handler(method, target, query, body, clen, &resp);

    pthread_mutex_lock(&g_mu);
    g_requests++;
    pthread_mutex_unlock(&g_mu);

    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\nCache-Control: no-store\r\n"
                     "Access-Control-Allow-Origin: *\r\n\r\n",
                     resp.status, reason(resp.status), resp.content_type, resp.body_len);
    if (send_all(fd, hdr, (size_t)n) == 0 && resp.body_len) send_all(fd, resp.body, resp.body_len);
    free(resp.body);
    free(target);

done:
    free(body);
    free(buf);
    shutdown(fd, SHUT_RDWR);
    close(fd);
    return NULL;
}

static uint16_t g_port = 0;

// A listening TCP socket bound to every interface. Returns the fd or -errno.
static int open_listener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -errno;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a;
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) { int e = errno; close(fd); return -e; }
    if (listen(fd, 32) != 0) { int e = errno; close(fd); return -e; }
    return fd;
}

static void *accept_thread(void *arg) {
    (void)arg;
    pthread_setname_np("rl.httpd.accept");
    unsigned backoff_us = 200000;
    for (;;) {
        conn_t *c = calloc(1, sizeof *c);
        if (!c) { usleep(100000); continue; }
        socklen_t sl = sizeof c->peer;
        c->fd = accept(g_listen_fd, (struct sockaddr *)&c->peer, &sl);
        if (c->fd < 0) {
            int e = errno;
            free(c);
            if (e == EINTR || e == ECONNABORTED) continue;
            if (e == EBADF || e == EINVAL || e == ENOTSOCK) {
                // tvOS reclaims a suspended app's listening socket (result 003:
                // accept = EBADF forever after a trip to the background).
                // Re-create it; while still suspended bind may fail — back off.
                close(g_listen_fd);
                int fd = open_listener(g_port);
                if (fd >= 0) {
                    g_listen_fd = fd;
                    backoff_us = 200000;
                    rl_logf("httpd: listener lost (errno=%d); re-created on port %u", e, (unsigned)g_port);
                    continue;
                }
                rl_logf("httpd: listener lost (errno=%d); re-create failed errno=%d, retry in %u ms", e, -fd, backoff_us / 1000);
            } else {
                rl_logf("httpd: accept failed errno=%d", e);
            }
            usleep(backoff_us);
            if (backoff_us < 5000000) backoff_us *= 2;
            continue;
        }
        backoff_us = 200000;
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 512 * 1024);
        if (pthread_create(&t, &attr, conn_thread, c) != 0) {
            close(c->fd);
            free(c);
        }
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

int httpd_start(uint16_t port, httpd_handler handler) {
    g_handler = handler;
    g_port = port;
    signal(SIGPIPE, SIG_IGN);
    int fd = open_listener(port);
    if (fd < 0) return fd;
    g_listen_fd = fd;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &attr, accept_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) { close(fd); g_listen_fd = -1; return -rc; }
    return 0;
}

uint64_t httpd_request_count(void) {
    pthread_mutex_lock(&g_mu);
    uint64_t n = g_requests;
    pthread_mutex_unlock(&g_mu);
    return n;
}
