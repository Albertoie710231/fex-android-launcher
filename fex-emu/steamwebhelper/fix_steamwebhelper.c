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
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>

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

/* NO SIGSYS handler — FEX handles syscall interception via host-level SIGSYS.
 * Installing a guest SIGSYS handler breaks FEX's syscall translation entirely.
 * FEX should handle clone3 → ENOSYS natively at the host level. */

/* Crash handler: park crashing thread in nanosleep */
static void crash_handler(int sig, siginfo_t *info, void *ucontext) {
    trap_count++;
    if (trap_count > 100) {
        syscall(SYS_exit_group, 0);
        return;
    }

    struct timespec ts = { .tv_sec = 3600, .tv_nsec = 0 };
    while (1) { syscall(SYS_nanosleep, &ts, NULL); }
}

/* sigaction wrapper — block app overrides of our crash handlers
 * CRITICAL: Do NOT block SIGSYS — FEX needs it for syscall interception.
 * We only block SIGTRAP/SIGILL/SIGABRT/SIGSEGV/SIGBUS. */
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    if (!real_sigaction_ptr)
        real_sigaction_ptr = (real_sigaction_fn)dlsym(RTLD_NEXT, "sigaction");
    if (act != NULL && (signum == SIGTRAP || signum == SIGILL ||
                        signum == SIGABRT ||
                        signum == SIGSEGV || signum == SIGBUS)) {
        if (oldact) memset(oldact, 0, sizeof(*oldact));
        return 0;
    }
    /* Also protect our chained SIGSYS handler from being overridden by the app */
    if (act != NULL && signum == SIGSYS) {
        if (oldact) memset(oldact, 0, sizeof(*oldact));
        return 0;
    }
    return real_sigaction_ptr(signum, act, oldact);
}

/* bind() wrapper — convert filesystem AF_UNIX sockets to abstract sockets.
 * FEX translates paths for mkdir/stat but NOT for bind(), so the kernel
 * can't find directories created through FEX's overlay. Abstract sockets
 * bypass the filesystem entirely. */
typedef int (*real_bind_fn)(int, const struct sockaddr *, socklen_t);
static real_bind_fn real_bind_ptr = NULL;

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (!real_bind_ptr)
        real_bind_ptr = (real_bind_fn)dlsym(RTLD_NEXT, "bind");

    if (addr && addr->sa_family == AF_UNIX && addrlen > sizeof(sa_family_t)) {
        const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
        /* If it's a filesystem path (not already abstract), convert to abstract */
        if (un->sun_path[0] != '\0') {
            struct sockaddr_un abstract_addr;
            memset(&abstract_addr, 0, sizeof(abstract_addr));
            abstract_addr.sun_family = AF_UNIX;
            /* Abstract socket: sun_path[0] = '\0', rest is the name */
            /* Use a hash of the path to keep it short */
            abstract_addr.sun_path[0] = '\0';
            /* Copy as much of the path as fits (skip leading dirs for uniqueness) */
            const char *name = un->sun_path;
            /* Find last meaningful part of path */
            const char *p = name;
            while (*p) p++;
            /* Walk back to get ~90 chars max */
            while (p > name && (p - name) > 90) p++;
            int len = 0;
            while (*p && len < 105) {
                abstract_addr.sun_path[1 + len] = *p;
                len++;
                p++;
            }
            socklen_t abs_len = offsetof(struct sockaddr_un, sun_path) + 1 + len;

            debug_msg("FIX: bind() → abstract socket: ");
            debug_msg(un->sun_path);
            debug_msg("\n");

            return real_bind_ptr(sockfd, (struct sockaddr *)&abstract_addr, abs_len);
        }
    }

    return real_bind_ptr(sockfd, addr, addrlen);
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

    /* NO SIGSYS handler — FEX needs SIGSYS for syscall interception */

    /* Install crash handlers for other signals (these are safe) */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    real_sigaction_ptr(SIGTRAP, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    real_sigaction_ptr(SIGILL, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    real_sigaction_ptr(SIGSEGV, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    real_sigaction_ptr(SIGBUS, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    real_sigaction_ptr(SIGABRT, &sa, NULL);

    mkdir("/home/user/.steam/debian-installation/config/htmlcache", 0755);

    debug_msg("FIX-v50: exec+bind wrapper\n");
}
