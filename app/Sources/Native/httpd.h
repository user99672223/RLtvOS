// httpd.h — tiny blocking HTTP/1.1 server (one thread per connection),
// LAN-only peers, used for the in-app debug endpoint on port 7777.
#ifndef RL_HTTPD_H
#define RL_HTTPD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct httpd_response {
    int status;              // HTTP status code (default 404)
    char content_type[96];   // default "text/plain; charset=utf-8"
    void *body;              // malloc'd; freed by the server after sending
    size_t body_len;
} httpd_response;

// Handler: fill *resp. body/query/path are NUL-terminated; body_len is the
// exact request body length. Runs on the connection's thread.
typedef void (*httpd_handler)(const char *method, const char *path, const char *query,
                              const char *body, size_t body_len, httpd_response *resp);

// Copies `data` (len bytes) into a fresh malloc'd body and sets status/type.
void httpd_set_response(httpd_response *r, int status, const char *content_type,
                        const void *data, size_t len);

// Bind 0.0.0.0:port, start the accept thread. Returns 0 or -errno.
int httpd_start(uint16_t port, httpd_handler handler);

// Number of requests served so far.
uint64_t httpd_request_count(void);

#ifdef __cplusplus
}
#endif
#endif
