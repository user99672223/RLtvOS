// rlkernel.h — C API of the fake Linux kernel (Phase C), used by the Swift
// app. Guest processes are thread groups inside this process; syscalls are
// served by the kernel through FEXCore's SyscallHandler interface.
#ifndef RLKERNEL_H
#define RLKERNEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Log sink for kernel messages and the guest syscall log (strace format,
// one line per call). Defaults to stderr.
typedef void (*rlk_log_fn)(const char *line);
void rlk_set_log(rlk_log_fn fn);

// The read-only guest filesystem: the rlvfs::Vfs* the app mounted
// (rlcore_vfs_handle()). Must be set before rlk_exec.
void rlk_set_vfs(void *rlvfs_vfs);

// Start a guest process (pid 1 = init when none exists; later calls make
// children of pid 1) from argv[0] resolved in the guest namespace, with the
// given environment (envc == 0 → a default PATH/HOME/USER/LANG/TERM/TMPDIR
// set) and cwd. dry_run != 0 loads the ELF (and its interpreter), builds
// the stack and reports the layout without executing anything; dry_run == 0
// additionally needs FEX (JIT pool ready) and runs the guest on a new host
// thread. Writes {"ok":bool,"pid":N,"layout":{..},"error":..}. Returns the
// pid (> 0) or -errno (Linux numbering).
int rlk_exec(const char *const *argv, int argc, const char *const *envp, int envc, const char *cwd, int dry_run,
             char *out, size_t cap);

// {"processes":[{"pid","ppid","exe","state","exit_code","threads","vmas",
//  "mapped_bytes","syscalls"}],"syscalls_total":N,"fex":bool}
void rlk_status_json(char *out, size_t cap);

// Last 64 KB of what guest pid wrote to fd 1/2, raw bytes, NUL-terminated
// (the tail that fits). Returns bytes copied (excluding the NUL).
size_t rlk_process_output(int pid, char *out, size_t cap);

// Ask every running guest thread to stop (best effort). Returns count.
int rlk_kill_all(void);

#ifdef __cplusplus
}
#endif
#endif
