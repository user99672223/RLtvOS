// hello_dyn.c — checkpoint C2: dynamically linked glibc hello.
// Exercises the ld.so path (openat/read/mmap/mprotect/arch_prctl/brk/
// set_tid_address/rseq/clock_gettime) plus a few libc calls the fake kernel
// must answer: getpid, uname, clock_gettime, getcwd, readlink /proc/self/exe.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv) {
    printf("hello from x86-64 glibc (argc=%d)\n", argc);
    struct utsname u;
    if (uname(&u) == 0) printf("uname: %s %s %s %s\n", u.sysname, u.release, u.version, u.machine);
    else printf("uname failed: %s\n", strerror(errno));
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("monotonic: %ld.%09ld\n", (long)ts.tv_sec, ts.tv_nsec);
    char cwd[512];
    printf("pid=%d ppid=%d uid=%d cwd=%s\n", getpid(), getppid(), getuid(), getcwd(cwd, sizeof cwd) ? cwd : "?");
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) { exe[n] = 0; printf("exe=%s\n", exe); } else printf("readlink /proc/self/exe: %s\n", strerror(errno));
    const char *home = getenv("HOME");
    printf("HOME=%s\n", home ? home : "(unset)");
    double *big = malloc(64 << 20);          // 64 MB: forces mmap, touches a few pages
    if (big) { for (int i = 0; i < 64; i++) big[i * 4096] = i; printf("malloc 64MB ok %g\n", big[63 * 4096]); free(big); }
    fflush(stdout);
    return 0;
}
