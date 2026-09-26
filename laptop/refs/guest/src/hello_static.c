// hello_static.c — checkpoint C1: no libc, only write(2) and exit_group(2).
// Build: gcc -static -nostdlib -nostartfiles -O2 -o hello-static hello_static.c
// Expected syscalls: write(1, "hello from x86-64 static\n", 25) = 25; exit_group(0)
typedef unsigned long u64;

static long sys3(long n, long a, long b, long c) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return ret;
}

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }

void _start(void) {
    const char *msg = "hello from x86-64 static\n";
    sys3(1, 1, (long)msg, (long)slen(msg));          // write
    // A second line with a computed value so the JIT's ALU path is exercised.
    char buf[32];
    u64 v = 0;
    for (u64 i = 1; i <= 1000; i++) v += i * i;      // 333833500
    int p = 31; buf[p] = '\n';
    do { buf[--p] = (char)('0' + v % 10); v /= 10; } while (v);
    sys3(1, 1, (long)(buf + p), (long)(32 - p));
    sys3(231, 0, 0, 0);                              // exit_group(0)
    for (;;) {}
}
