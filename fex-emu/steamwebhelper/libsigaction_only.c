/* Shim with ONLY sigaction wrapper — tests if overriding sigaction breaks FEX */
#define _GNU_SOURCE
#include <signal.h>
#include <string.h>
#include <dlfcn.h>

typedef int (*real_sigaction_fn)(int, const struct sigaction *, struct sigaction *);
static real_sigaction_fn real_sigaction_ptr = NULL;

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    if (!real_sigaction_ptr)
        real_sigaction_ptr = (real_sigaction_fn)dlsym(RTLD_NEXT, "sigaction");
    if (act != NULL && (signum == SIGTRAP || signum == SIGILL ||
                        signum == SIGABRT || signum == SIGSYS ||
                        signum == SIGSEGV || signum == SIGBUS)) {
        if (oldact) memset(oldact, 0, sizeof(*oldact));
        return 0;
    }
    return real_sigaction_ptr(signum, act, oldact);
}
