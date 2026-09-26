// rlcore.h — C API of the CMake-built core (C++ inside), consumed by the
// Swift app through the bridging header.
#ifndef RLCORE_H
#define RLCORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// "0.1 (clang X, libc++ Y, built <date>)"
const char *rlcore_version(void);

// Exercises the C++ runtime inside the app process (threads, atomics,
// exceptions, allocation, chrono, thread_local) and writes a JSON object
// into out/cap. Returns 1 on success.
int rlcore_selftest(char *out, size_t cap);

// Log sink installed by the app (rl_log_str); the core calls it for its
// own lines. Optional; defaults to stderr.
typedef void (*rlcore_log_fn)(const char *line);
void rlcore_set_log(rlcore_log_fn fn);

#ifdef __cplusplus
}
#endif
#endif
