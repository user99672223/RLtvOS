// rl_log.c — fixed-size ring of log lines with a global sequence number.
#include "rl_log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RL_LOG_LINES 8192
#define RL_LOG_LINE_MAX 448

typedef struct {
    uint64_t seq;
    double t_ms;
    uint16_t len;
    char text[RL_LOG_LINE_MAX];
} rl_line;

static rl_line g_lines[RL_LOG_LINES];
static uint64_t g_next_seq = 1;        // seq 0 is never used
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct timespec g_start;
static pthread_once_t g_start_once = PTHREAD_ONCE_INIT;

static void start_clock(void) {
    clock_gettime(CLOCK_MONOTONIC, &g_start);
}

// Lock-free (safe to call with g_mu held).
double rl_uptime_ms(void) {
    struct timespec now;
    pthread_once(&g_start_once, start_clock);
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - g_start.tv_sec) * 1000.0 +
           (double)(now.tv_nsec - g_start.tv_nsec) / 1e6;
}

static void ensure_started(void) {
    pthread_once(&g_start_once, start_clock);
}

void rl_log_str(const char *line) {
    if (!line) return;
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    if (n >= RL_LOG_LINE_MAX) n = RL_LOG_LINE_MAX - 1;
    double t = rl_uptime_ms();

    pthread_mutex_lock(&g_mu);
    uint64_t seq = g_next_seq++;
    rl_line *l = &g_lines[seq % RL_LOG_LINES];
    l->seq = seq;
    l->t_ms = t;
    l->len = (uint16_t)n;
    memcpy(l->text, line, n);
    l->text[n] = 0;
    pthread_mutex_unlock(&g_mu);

    // Mirror to stderr so lldb / Console see it too.
    fprintf(stderr, "[%8.3f] %.*s\n", t / 1000.0, (int)n, line);
}

void rl_logf(const char *fmt, ...) {
    char buf[RL_LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rl_log_str(buf);
}

uint64_t rl_log_next_seq(void) {
    pthread_mutex_lock(&g_mu);
    uint64_t s = g_next_seq;
    pthread_mutex_unlock(&g_mu);
    return s;
}

static int utf8_seq_len(const unsigned char *p, size_t n) {
    // Returns the length of a valid UTF-8 sequence starting at p, else 0.
    if (n == 0) return 0;
    unsigned char c = p[0];
    int len;
    if (c < 0x80) return 1;
    else if ((c & 0xE0) == 0xC0 && c >= 0xC2) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0 && c <= 0xF4) len = 4;
    else return 0;
    if ((size_t)len > n) return 0;
    for (int i = 1; i < len; i++)
        if ((p[i] & 0xC0) != 0x80) return 0;
    return len;
}

size_t rl_json_escape(const char *s, size_t n, char *out, size_t cap) {
    static const char hex[] = "0123456789abcdef";
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n;) {
        unsigned char c = p[i];
        char esc[8];
        size_t el = 0;
        int ul = 0;
        if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = (char)c; el = 2; i++; }
        else if (c == '\n') { esc[0] = '\\'; esc[1] = 'n'; el = 2; i++; }
        else if (c == '\r') { esc[0] = '\\'; esc[1] = 'r'; el = 2; i++; }
        else if (c == '\t') { esc[0] = '\\'; esc[1] = 't'; el = 2; i++; }
        else if (c < 0x20 || c == 0x7f) {
            esc[0] = '\\'; esc[1] = 'u'; esc[2] = '0'; esc[3] = '0';
            esc[4] = hex[c >> 4]; esc[5] = hex[c & 15]; el = 6; i++;
        } else if (c < 0x80) { esc[0] = (char)c; el = 1; i++; }
        else if ((ul = utf8_seq_len(p + i, n - i)) > 0) {
            if (o + (size_t)ul >= cap) break;
            memcpy(out + o, p + i, (size_t)ul);
            o += (size_t)ul; i += (size_t)ul;
            continue;
        } else {
            esc[0] = '\\'; esc[1] = 'u'; esc[2] = '0'; esc[3] = '0';
            esc[4] = hex[c >> 4]; esc[5] = hex[c & 15]; el = 6; i++;
        }
        if (o + el >= cap) break;
        memcpy(out + o, esc, el);
        o += el;
    }
    if (cap) out[o < cap ? o : cap - 1] = 0;
    return o;
}

char *rl_log_json_since(uint64_t since, int max_lines) {
    if (max_lines <= 0 || max_lines > RL_LOG_LINES) max_lines = RL_LOG_LINES;
    pthread_mutex_lock(&g_mu);
    ensure_started();
    uint64_t next = g_next_seq;
    uint64_t oldest = next > RL_LOG_LINES ? next - RL_LOG_LINES : 1;
    if (since < 1) since = 1;
    uint64_t dropped = since < oldest ? oldest - since : 0;
    uint64_t from = since < oldest ? oldest : since;
    uint64_t count = next > from ? next - from : 0;
    if (count > (uint64_t)max_lines) count = (uint64_t)max_lines;
    uint64_t to = from + count;  // exclusive

    size_t cap = 128 + count * (RL_LOG_LINE_MAX * 6 + 64);
    char *out = malloc(cap);
    if (!out) { pthread_mutex_unlock(&g_mu); return strdup("{\"next\":0,\"dropped\":0,\"lines\":[]}"); }
    size_t o = (size_t)snprintf(out, cap, "{\"next\":%llu,\"dropped\":%llu,\"lines\":[",
                                (unsigned long long)to, (unsigned long long)dropped);
    for (uint64_t s = from; s < to; s++) {
        const rl_line *l = &g_lines[s % RL_LOG_LINES];
        if (l->seq != s) continue;  // overwritten mid-way; skip
        o += (size_t)snprintf(out + o, cap - o, "%s{\"seq\":%llu,\"t\":%.1f,\"s\":\"",
                              s == from ? "" : ",", (unsigned long long)l->seq, l->t_ms);
        o += rl_json_escape(l->text, l->len, out + o, cap - o);
        o += (size_t)snprintf(out + o, cap - o, "\"}");
    }
    snprintf(out + o, cap - o, "]}");
    pthread_mutex_unlock(&g_mu);
    return out;
}

char *rl_log_text_since(uint64_t since, int max_lines, uint64_t *next_out) {
    if (max_lines <= 0 || max_lines > RL_LOG_LINES) max_lines = RL_LOG_LINES;
    pthread_mutex_lock(&g_mu);
    ensure_started();
    uint64_t next = g_next_seq;
    uint64_t oldest = next > RL_LOG_LINES ? next - RL_LOG_LINES : 1;
    if (since < 1) since = 1;
    uint64_t from = since < oldest ? oldest : since;
    uint64_t count = next > from ? next - from : 0;
    if (count > (uint64_t)max_lines) count = (uint64_t)max_lines;
    uint64_t to = from + count;
    size_t cap = 64 + count * (RL_LOG_LINE_MAX + 48);
    char *out = malloc(cap);
    size_t o = 0;
    if (out) {
        for (uint64_t s = from; s < to; s++) {
            const rl_line *l = &g_lines[s % RL_LOG_LINES];
            if (l->seq != s) continue;
            o += (size_t)snprintf(out + o, cap - o, "%llu %.1f %s\n",
                                  (unsigned long long)l->seq, l->t_ms, l->text);
        }
        out[o] = 0;
    }
    if (next_out) *next_out = to;
    pthread_mutex_unlock(&g_mu);
    return out ? out : strdup("");
}
