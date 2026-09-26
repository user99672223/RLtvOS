// threads_test.c — pthreads + futex + signals smoke test (used from C3 on).
// clone(CLONE_VM|CLONE_FS|...|CLONE_SETTLS|CLONE_CHILD_CLEARTID), futex
// wait/wake, rt_sigaction + tgkill delivery into a running thread, nanosleep.
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int ready;
static atomic_long counter;
static volatile sig_atomic_t got_usr1;

static void on_usr1(int sig) { (void)sig; got_usr1 = 1; }

static void *worker(void *arg) {
    long id = (long)arg;
    pthread_mutex_lock(&mu);
    while (!ready) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < 100000; i++) atomic_fetch_add(&counter, 1);
    return (void *)(id * 10);
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);

    pthread_t th[4];
    for (long i = 0; i < 4; i++)
        if (pthread_create(&th[i], NULL, worker, (void *)i) != 0) { perror("pthread_create"); return 1; }
    struct timespec ts = {0, 20 * 1000 * 1000};
    nanosleep(&ts, NULL);
    pthread_mutex_lock(&mu);
    ready = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);

    // Signal ourselves while threads run (async signal into JIT'd code).
    syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), SIGUSR1);

    long sum = 0;
    for (int i = 0; i < 4; i++) { void *r; pthread_join(th[i], &r); sum += (long)r; }
    printf("threads: counter=%ld (expect 400000) joined=%ld (expect 60) usr1=%d tid=%ld\n",
           (long)atomic_load(&counter), sum, (int)got_usr1, (long)syscall(SYS_gettid));
    return (atomic_load(&counter) == 400000 && sum == 60 && got_usr1) ? 0 : 2;
}
