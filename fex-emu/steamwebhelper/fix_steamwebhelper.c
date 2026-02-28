#define _GNU_SOURCE
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/resource.h>
#include <dlfcn.h>
#include <time.h>

#ifndef SYS_clone3
#define SYS_clone3 435
#endif

static volatile int trap_count = 0;

typedef int (*real_sigaction_fn)(int, const struct sigaction *, struct sigaction *);
static real_sigaction_fn real_sigaction_ptr = NULL;

static void debug_msg(const char *msg) {
    int len = 0;
    while (msg[len]) len++;
    syscall(SYS_write, 2, msg, len);
}

static void debug_int(const char *prefix, long val) {
    char buf[64];
    char *p = buf;
    const char *s = prefix;
    while (*s) *p++ = *s++;
    if (val < 0) { *p++ = '-'; val = -val; }
    char digits[20];
    int n = 0;
    if (val == 0) { digits[n++] = '0'; }
    else { while (val > 0) { digits[n++] = '0' + (val % 10); val /= 10; } }
    for (int i = n - 1; i >= 0; i--) *p++ = digits[i];
    *p++ = '\n';
    syscall(SYS_write, 2, buf, p - buf);
}

static void debug_hex(const char *prefix, unsigned long val) {
    char buf[80];
    char *p = buf;
    const char *s = prefix;
    while (*s) *p++ = *s++;
    *p++ = '0'; *p++ = 'x';
    char hex[17];
    int n = 0;
    if (val == 0) { hex[n++] = '0'; }
    else { unsigned long v = val; while (v > 0) { hex[n++] = "0123456789abcdef"[v & 0xf]; v >>= 4; } }
    for (int i = n - 1; i >= 0; i--) *p++ = hex[i];
    *p++ = '\n';
    syscall(SYS_write, 2, buf, p - buf);
}

/* SIGSYS handler for clone3 */
static void sigsys_handler(int sig, siginfo_t *info, void *ucontext) {
    ucontext_t *ctx = (ucontext_t *)ucontext;
    if (info->si_syscall == SYS_clone3) {
        ctx->uc_mcontext.gregs[REG_RAX] = (unsigned long)(-38);
    }
}

/*
 * Crash handler strategy: PARK the crashing thread.
 *
 * Handles SIGTRAP (int3), SIGILL (ud2), SIGSEGV, and SIGBUS.
 * Parks the thread in infinite nanosleep — keeps process alive,
 * other threads continue normally.
 */
static void crash_handler(int sig, siginfo_t *info, void *ucontext) {
    trap_count++;

    if (trap_count > 100) {
        debug_msg("FIX: >100 crashes total, exiting\n");
        syscall(SYS_exit_group, 0);
        return;
    }

    ucontext_t *ctx = (ucontext_t *)ucontext;
    unsigned long rip = ctx->uc_mcontext.gregs[REG_RIP];

    const char *signame = "UNKNOWN";
    if (sig == SIGTRAP) signame = "SIGTRAP";
    else if (sig == SIGILL) signame = "SIGILL";
    else if (sig == SIGSEGV) signame = "SIGSEGV";
    else if (sig == SIGBUS) signame = "SIGBUS";

    debug_msg("FIX: ");
    debug_msg(signame);
    debug_int(" — parking thread (crash #", trap_count);
    debug_hex("  RIP=", rip);
    if (sig == SIGSEGV || sig == SIGBUS) {
        debug_hex("  fault_addr=", (unsigned long)info->si_addr);
    }

    /* Park this thread forever — sleep in 1-hour intervals */
    struct timespec ts;
    ts.tv_sec = 3600;
    ts.tv_nsec = 0;
    while (1) {
        syscall(SYS_nanosleep, &ts, NULL);
    }
}

/* sigaction wrapper — block overrides of our crash handlers */
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    if (!real_sigaction_ptr) {
        real_sigaction_ptr = (real_sigaction_fn)dlsym(RTLD_NEXT, "sigaction");
    }
    if (act != NULL && (signum == SIGTRAP || signum == SIGILL ||
                        signum == SIGABRT || signum == SIGSYS ||
                        signum == SIGSEGV || signum == SIGBUS)) {
        if (oldact) memset(oldact, 0, sizeof(*oldact));
        return 0;
    }
    return real_sigaction_ptr(signum, act, oldact);
}

typedef void (*sighandler_t)(int);
sighandler_t signal(int signum, sighandler_t handler) {
    if (signum == SIGTRAP || signum == SIGILL ||
        signum == SIGABRT || signum == SIGSYS ||
        signum == SIGSEGV || signum == SIGBUS) {
        return SIG_DFL;
    }
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    if (real_sigaction_ptr) {
        real_sigaction_ptr(signum, &sa, &old_sa);
        return old_sa.sa_handler;
    }
    return SIG_DFL;
}

/* PLT close wrapper */
int close(int fd) {
    syscall(SYS_close, fd);
    errno = 0;
    return 0;
}

/* PLT flock wrapper */
int flock(int fd, int op) {
    syscall(SYS_flock, fd, op);
    errno = 0;
    return 0;
}

__attribute__((constructor))
static void install_fixes(void) {
    real_sigaction_ptr = (real_sigaction_fn)dlsym(RTLD_NEXT, "sigaction");
    if (!real_sigaction_ptr) {
        debug_msg("FIX: WARNING - could not resolve real sigaction!\n");
        return;
    }

    struct rlimit rl;
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    setrlimit(RLIMIT_AS, &rl);
    rl.rlim_cur = 65536;
    rl.rlim_max = 65536;
    setrlimit(RLIMIT_NOFILE, &rl);

    struct sigaction sa;

    /* SIGSYS for clone3 */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigsys_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    real_sigaction_ptr(SIGSYS, &sa, NULL);

    /* SIGTRAP — park crashing thread */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    real_sigaction_ptr(SIGTRAP, &sa, NULL);

    /* SIGILL — park crashing thread */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    real_sigaction_ptr(SIGILL, &sa, NULL);

    /* SIGSEGV — park crashing thread */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    real_sigaction_ptr(SIGSEGV, &sa, NULL);

    /* SIGBUS — park crashing thread */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    real_sigaction_ptr(SIGBUS, &sa, NULL);

    /* SIGABRT — ignore */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    real_sigaction_ptr(SIGABRT, &sa, NULL);

    mkdir("/home/user/.steam/debian-installation/config/htmlcache", 0755);
    debug_msg("FIX-v17: park ALL crashes (TRAP+ILL+SEGV+BUS) + sigaction guard + clone3\n");
}
