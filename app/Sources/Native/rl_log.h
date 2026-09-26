// rl_log.h — in-process line log shared by host code and (later) the fake
// kernel's strace-format syscall log. One monotonically increasing sequence
// number for both, so /log?since=N is a single stream.
#ifndef RL_LOG_H
#define RL_LOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Append one line (a trailing newline is stripped). Thread-safe.
void rl_log_str(const char *line);

// printf-style variant for C callers (Swift uses rl_log_str).
void rl_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Sequence number the next appended line will get.
uint64_t rl_log_next_seq(void);

// JSON document {"next":N,"dropped":D,"lines":[{"seq":..,"t":ms,"s":".."},..]}
// with all retained lines whose seq >= since (at most max_lines). The
// returned buffer is malloc'd; caller frees. Never returns NULL.
char *rl_log_json_since(uint64_t since, int max_lines);

// Plain text: one line per entry "<seq> <t_ms> <line>\n"; malloc'd.
char *rl_log_text_since(uint64_t since, int max_lines, uint64_t *next_out);

// Escape s as a JSON string body (no surrounding quotes) into out; returns
// bytes written (excluding NUL). Valid UTF-8 passes through, other bytes
// >= 0x80 become \u00XX.
size_t rl_json_escape(const char *s, size_t n, char *out, size_t cap);

// Milliseconds since process start (monotonic).
double rl_uptime_ms(void);

#ifdef __cplusplus
}
#endif
#endif
