/* Test G: ONLY SIGSYS handler */
#define _GNU_SOURCE
#include <signal.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef int (*real_sigaction_fn)(int, const struct sigaction *, struct sigaction *);

static void sigsys_handler(int sig, siginfo_t *info, void *ucontext) { }

__attribute__((constructor))
static void init(void) {
    real_sigaction_fn real_sa = (real_sigaction_fn)dlsym(RTLD_NEXT, "sigaction");
    if (!real_sa) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigsys_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    real_sa(SIGSYS, &sa, NULL);
    syscall(SYS_write, 2, "SIG: SIGSYS only\n", 18);
}
