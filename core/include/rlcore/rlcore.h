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

// ---- guest VFS (served by laptop/assets_server.py) -------------------------
// All write a JSON object into out/cap and return 1 on success, 0 on error.
// base_url: "http://192.168.1.133:8090"; cache_dir: block cache directory.
int rlcore_vfs_mount(const char *base_url, const char *cache_dir, char *out, size_t cap);
int rlcore_vfs_stat(const char *guest_path, int follow, char *out, size_t cap);
int rlcore_vfs_ls(const char *guest_path, char *out, size_t cap);
// Reads up to len (<= 65536) bytes at off; JSON has n, hex32 and escaped text.
int rlcore_vfs_read(const char *guest_path, uint64_t off, uint32_t len, char *out, size_t cap);
void rlcore_vfs_stats(char *out, size_t cap);
// The mounted rlvfs::Vfs* (opaque here) for the fake kernel (rlk_set_vfs),
// NULL when nothing is mounted. Valid until the next rlcore_vfs_mount, which
// must not happen while guest processes run.
void *rlcore_vfs_handle(void);

#ifdef __cplusplus
}
#endif
#endif
