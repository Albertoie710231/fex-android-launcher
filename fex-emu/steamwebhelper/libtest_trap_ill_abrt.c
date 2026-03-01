/* Test H: SIGTRAP + SIGILL + SIGABRT (no SIGSYS) */
#define _GNU_SOURCE
#include <signal.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>

typedef int (*real_sigaction_fn)(int, const struct sigaction *, struct sigaction *);

static void crash_handler(int sig, siginfo_t *info, void *ucontext) {
    struct timespec ts = { .tv_sec = 3600, .tv_nsec = 0 };
    while (1) { syscall(SYS_nanosleep, &ts, NULL); }
}

__attribute__((constructor))
static void init(void) {
    real_sigaction_fn real_sa = (real_sigaction_fn)dlsym(RTLD_NEXT, "sigaction");
    if (!real_sa) return;
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    real_sa(SIGTRAP, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    real_sa(SIGILL, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    real_sa(SIGABRT, &sa, NULL);

    syscall(SYS_write, 2, "SIG: TRAP+ILL+ABRT\n", 20);
}
