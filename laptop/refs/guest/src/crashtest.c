// crashtest.c — deliberate faults for the fake kernel's fault paths:
//   crashtest null-write     store to address 0        → SIGSEGV (SEGV_MAPERR)
//   crashtest ro-write       store to a PROT_READ page → SIGSEGV (SEGV_ACCERR)
//   crashtest handler        installs a SIGSEGV handler that fixes the page
//                            (mprotect) and returns; prints "recovered"
//   crashtest abort          abort() → SIGABRT
//   crashtest loop           busy loop that never syscalls (killall/kick test)
//   crashtest fork-untouched a forked child writes to pages its parent mapped
//                            but never touched (heap and stack); the parent
//                            must see them unchanged afterwards
// Exit codes: the signal's default action (128+sig from the shell) or 0.
#define _GNU_SOURCE  // REG_ERR / REG_RIP in <ucontext.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

static volatile int* g_page;
static volatile int g_handled;

static void on_segv(int sig, siginfo_t* si, void* uc_) {
    ucontext_t* uc = uc_;
    g_handled++;
    unsigned long err = (unsigned long)uc->uc_mcontext.gregs[REG_ERR];
    char buf[160];
    int n = snprintf(buf, sizeof buf, "handler: sig=%d code=%d addr=%p rip=%#llx err=%#lx (%s)\n", sig, si->si_code, si->si_addr,
                     (unsigned long long)uc->uc_mcontext.gregs[REG_RIP], err, (err & 2) ? "write" : "read");
    if (write(1, buf, (size_t)n) < 0) _exit(98);
    if (si->si_addr == (void*)g_page) {
        mprotect((void*)g_page, 4096, PROT_READ | PROT_WRITE);  // fix it: the store is retried
    } else {
        _exit(99);
    }
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "null-write";
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!strcmp(mode, "null-write")) {
        printf("writing to NULL\n");
        *(volatile int*)0 = 1;
        printf("not reached\n");
    } else if (!strcmp(mode, "ro-write")) {
        g_page = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        printf("writing to a read-only page %p\n", (void*)g_page);
        *g_page = 1;
        printf("not reached\n");
    } else if (!strcmp(mode, "handler")) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_segv;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        g_page = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        printf("writing to a read-only page %p with a handler\n", (void*)g_page);
        *g_page = 42;
        printf("recovered: value=%d handled=%d\n", *g_page, g_handled);
        return g_handled == 1 && *g_page == 42 ? 0 : 3;
    } else if (!strcmp(mode, "abort")) {
        printf("abort()\n");
        abort();
    } else if (!strcmp(mode, "loop")) {
        printf("looping\n");
        for (volatile unsigned long i = 0;; i++) {
        }
    } else if (!strcmp(mode, "fork-untouched")) {
        // The child's first write to a page the parent never touched has no
        // translation entry yet: the copy-on-write fault arrives as a
        // translation fault, not a permission fault (result 006, C2).
        const size_t len = 4u << 20;
        unsigned char* m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) return 4;
        printf("fork with %zu KB untouched\n", len >> 10);
        pid_t pid = fork();
        if (pid < 0) return 5;
        if (pid == 0) {
            m[0] = 1;
            m[len - 1] = 2;                  // untouched heap pages
            volatile char deep[1 << 20];     // untouched stack pages
            memset((char*)deep, 3, sizeof deep);
            _exit(m[0] + m[len - 1] + deep[12345] == 6 ? 0 : 7);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        printf("child: %s %d; parent sees m[0]=%d m[last]=%d (expect 0 0)\n", WIFEXITED(st) ? "exit" : "signal",
               WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st), m[0], m[len - 1]);
        return WIFEXITED(st) && WEXITSTATUS(st) == 0 && m[0] == 0 && m[len - 1] == 0 ? 0 : 6;
    } else {
        fprintf(stderr, "usage: crashtest null-write|ro-write|handler|abort|loop|fork-untouched\n");
        return 2;
    }
    return 0;
}
