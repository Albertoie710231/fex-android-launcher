/* v93: v91b + fake SHMemStream (creates shared memory + socket for steam IPC).
 *
 * v85: + shm_open redirect works (ValveIPC + chrome_shmem)
 * v86: + heartbeat (38 threads, process ALIVE)
 * v87-v88: + FD 11 "sdPC" handshake (2R+2W then STOPS)
 * v89: + shm_open O_EXCL strip → ALL shm_open FIXED
 * v90: + accept errno/accept4
 * v91b: + fake FD 11 ready signal (Chromium init never completes in
 *         single-process mode due to V8 proxy resolver → no ready signal
 *         → steam client times out after 60s)
 */
#define _GNU_SOURCE
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/resource.h>
#include <dlfcn.h>
#include <time.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <stdarg.h>
#include <pthread.h>
#include <errno.h>

typedef int (*real_sigaction_fn)(int, const struct sigaction *, struct sigaction *);
static real_sigaction_fn real_sigaction_ptr = NULL;

static void debug_msg(const char *msg) {
    int len = 0;
    while (msg[len]) len++;
    /* Use raw syscall to avoid recursion through our syscall() wrapper */
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret)
        : "0"((long)SYS_write), "D"(2L), "S"(msg), "d"((long)len)
        : "rcx", "r11", "memory");
}

static void debug_int(const char *prefix, long val) {
    char buf[80];
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
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret)
        : "0"((long)SYS_write), "D"(2L), "S"(buf), "d"((long)(p - buf))
        : "rcx", "r11", "memory");
}

static void debug_hex(const char *prefix, unsigned long val) {
    char buf[80];
    char *p = buf;
    const char *s = prefix;
    while (*s) *p++ = *s++;
    *p++ = '0'; *p++ = 'x';
    char digits[16];
    int n = 0;
    if (val == 0) { digits[n++] = '0'; }
    else { while (val > 0) { digits[n++] = "0123456789abcdef"[val & 0xf]; val >>= 4; } }
    for (int i = n - 1; i >= 0; i--) *p++ = digits[i];
    *p++ = '\n';
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret)
        : "0"((long)SYS_write), "D"(2L), "S"(buf), "d"((long)(p - buf))
        : "rcx", "r11", "memory");
}

static long get_pid(void) {
    long pid;
    __asm__ volatile ("syscall" : "=a"(pid)
        : "0"((long)SYS_getpid)
        : "rcx", "r11", "memory");
    return pid;
}

/* Hex dump up to 60 bytes of a buffer (multiple lines if needed) */
static void debug_hexdump(const char *prefix, const void *data, int len) {
    const unsigned char *d = (const unsigned char *)data;
    int show = len > 60 ? 60 : len;
    int off = 0;
    while (off < show) {
        char buf[200];
        char *p = buf;
        if (off == 0) {
            const char *s = prefix;
            while (*s && p < buf + 20) *p++ = *s++;
        } else {
            /* continuation indent */
            *p++ = ' '; *p++ = ' '; *p++ = ' '; *p++ = ' ';
            *p++ = ' '; *p++ = ' ';
        }
        int line_end = off + 20;
        if (line_end > show) line_end = show;
        for (int i = off; i < line_end; i++) {
            *p++ = "0123456789abcdef"[(d[i] >> 4) & 0xf];
            *p++ = "0123456789abcdef"[d[i] & 0xf];
            *p++ = ' ';
        }
        if (off == 0 && len > 60) { *p++ = '.'; *p++ = '.'; *p++ = '.'; }
        *p++ = '\n';
        long ret;
        __asm__ volatile ("syscall" : "=a"(ret)
            : "0"((long)SYS_write), "D"(2L), "S"(buf), "d"((long)(p - buf))
            : "rcx", "r11", "memory");
        off = line_end;
    }
}

/* Crash handler: skip int3 (SIGTRAP) and ud2 (SIGILL), park SIGSEGV/SIGBUS.
 * Chromium's FATAL/ImmediateCrash uses: int3 (1 byte) + ud2 (2 bytes).
 * We skip both so FATAL doesn't kill the main thread. */
static volatile int sigtrap_count = 0;
static volatile int sigill_count = 0;

static void crash_handler(int sig, siginfo_t *info, void *ucontext) {
    if (sig == SIGTRAP && ucontext) {
        int count = __sync_fetch_and_add(&sigtrap_count, 1);
        ucontext_t *ctx = (ucontext_t *)ucontext;
        if (count < 10) {
            debug_int("FIX: SIGTRAP skip #", count + 1);
            debug_hex("  RIP=", ctx->uc_mcontext.gregs[REG_RIP]);
        }
        ctx->uc_mcontext.gregs[REG_RIP] += 1;
        return;
    }
    if (sig == SIGILL && ucontext) {
        int count = __sync_fetch_and_add(&sigill_count, 1);
        ucontext_t *ctx = (ucontext_t *)ucontext;
        if (count < 10) {
            debug_int("FIX: SIGILL skip #", count + 1);
            debug_hex("  RIP=", ctx->uc_mcontext.gregs[REG_RIP]);
        }
        ctx->uc_mcontext.gregs[REG_RIP] += 2; /* ud2 = 0x0F 0x0B, 2 bytes */
        return;
    }
    debug_int("FIX: CRASH signal=", sig);
    struct timespec ts = { .tv_sec = 3600, .tv_nsec = 0 };
    while (1) {
        long ret;
        __asm__ volatile ("syscall" : "=a"(ret)
            : "0"((long)SYS_nanosleep), "D"(&ts), "S"((long)0)
            : "rcx", "r11", "memory");
    }
}

/* sigaction wrapper: block ALL fatal signal overrides (v56 style) */
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    if (!real_sigaction_ptr)
        real_sigaction_ptr = (real_sigaction_fn)dlsym(RTLD_NEXT, "sigaction");
    if (act != NULL && (signum == SIGTRAP || signum == SIGILL || signum == SIGABRT ||
                        signum == SIGSEGV || signum == SIGBUS || signum == SIGSYS)) {
        if (oldact) memset(oldact, 0, sizeof(*oldact));
        return 0;
    }
    return real_sigaction_ptr(signum, act, oldact);
}

/* signal() wrapper (v56 had this) */
typedef void (*sighandler_t)(int);
sighandler_t signal(int signum, sighandler_t handler) {
    if (signum == SIGTRAP || signum == SIGILL || signum == SIGABRT ||
        signum == SIGSEGV || signum == SIGBUS || signum == SIGSYS) {
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

/* _exit/_Exit wrappers (v56 had these) */
void _exit(int status) {
    debug_int("FIX: _exit status=", status);
    __asm__ volatile ("syscall" : :
        "a"((long)SYS_exit_group), "D"((long)status)
        : "rcx", "r11", "memory");
    __builtin_unreachable();
}

void _Exit(int status) {
    debug_int("FIX: _Exit status=", status);
    __asm__ volatile ("syscall" : :
        "a"((long)SYS_exit_group), "D"((long)status)
        : "rcx", "r11", "memory");
    __builtin_unreachable();
}

/* ============================================================
 * ROBUST_LIST FIX: FEX's get_robust_list returns -1.
 * Valve's steamwebhelper checks this in child processes and
 * calls Fatal() if it fails → children die → no renderer.
 *
 * Strategy:
 *   1. Scan glibc's TCB for the real robust_list_head
 *   2. Track set_robust_list calls in __thread storage
 *   3. Use pthread_atfork to re-register in forked children
 *   4. Return saved/scanned pointer from get_robust_list wrapper
 * ============================================================ */

typedef long (*real_syscall_fn)(long, ...);
static real_syscall_fn real_syscall_ptr = NULL;

static __thread void *robust_list_saved = NULL;
static __thread size_t robust_list_saved_len = 0;

/* Find the robust_list_head in glibc's TCB by scanning for the
 * distinctive self-referential pattern (list pointer == &list). */
static void *find_tcb_robust_head(void) {
    char *self = (char *)pthread_self();
    void *best = NULL;

    for (int offset = 0; offset < 2048; offset += sizeof(void *)) {
        void **ptr = (void **)(self + offset);
        if (*ptr == (void *)ptr) {
            long *fields = (long *)ptr;
            /* Best match: futex_offset non-zero, list_op_pending == 0 */
            if (!best && fields[1] != 0 && fields[2] == 0) {
                best = ptr;
            }
        }
    }

    /* Fallback: first self-ref pointer */
    if (!best) {
        for (int offset = 0; offset < 2048; offset += sizeof(void *)) {
            void **ptr = (void **)(self + offset);
            if (*ptr == (void *)ptr) {
                best = ptr;
                break;
            }
        }
    }

    return best;
}

/* After fork(), kernel clears the child's robust_list.
 * Re-scan TCB and re-register so Valve's check passes. */
static void child_fork_handler(void) {
    debug_msg("FIX: child_fork_handler (post-fork)\n");

    void *tcb_head = find_tcb_robust_head();
    if (tcb_head) {
        robust_list_saved = tcb_head;
        robust_list_saved_len = 24;
        if (real_syscall_ptr) {
            long ret = real_syscall_ptr(SYS_set_robust_list, (long)tcb_head, 24L);
            debug_int("FIX:   child set_robust_list ret=", ret);
        }
    } else {
        debug_msg("FIX:   WARNING: no robust_list_head in child TCB\n");
    }
}

/* syscall() wrapper: intercept get/set_robust_list */
long syscall(long number, ...) {
    if (!real_syscall_ptr)
        real_syscall_ptr = (real_syscall_fn)dlsym(RTLD_NEXT, "syscall");

    va_list ap;
    va_start(ap, number);

    if (number == SYS_set_robust_list) {
        void *head = va_arg(ap, void *);
        size_t len = va_arg(ap, size_t);
        va_end(ap);
        robust_list_saved = head;
        robust_list_saved_len = len;
        return real_syscall_ptr(number, (long)head, (long)len);
    }

    if (number == SYS_get_robust_list) {
        int pid = va_arg(ap, int);
        void **head_ptr = va_arg(ap, void **);
        size_t *len_ptr = va_arg(ap, size_t *);
        va_end(ap);

        if (pid == 0) {
            if (robust_list_saved) {
                if (head_ptr) *head_ptr = robust_list_saved;
                if (len_ptr) *len_ptr = robust_list_saved_len;
                return 0;
            }
            void *tcb_head = find_tcb_robust_head();
            if (tcb_head) {
                robust_list_saved = tcb_head;
                robust_list_saved_len = 24;
                if (head_ptr) *head_ptr = tcb_head;
                if (len_ptr) *len_ptr = 24;
                return 0;
            }
        }

        /* For other pids or if self-lookup failed, try real then fake */
        long ret = real_syscall_ptr(number, (long)pid, (long)head_ptr, (long)len_ptr);
        if (ret != 0 && pid != 0) {
            void *tcb_head = find_tcb_robust_head();
            if (head_ptr) *head_ptr = tcb_head ? tcb_head : NULL;
            if (len_ptr) *len_ptr = tcb_head ? 24 : 0;
            return 0;
        }
        return ret;
    }

    /* Forward all other syscalls */
    long a1 = va_arg(ap, long);
    long a2 = va_arg(ap, long);
    long a3 = va_arg(ap, long);
    long a4 = va_arg(ap, long);
    long a5 = va_arg(ap, long);
    long a6 = va_arg(ap, long);
    va_end(ap);
    return real_syscall_ptr(number, a1, a2, a3, a4, a5, a6);
}

/* fopen/open wrappers: redirect /proc/bus/pci and /sys/bus/pci → /dev/null.
 * libpci uses fopen("/proc/bus/pci/devices") which bypasses open() wrappers
 * (glibc's fopen calls openat syscall directly). Must intercept fopen too.
 * Without this, --in-process-gpu crashes from PCI enumeration SIGSEGV. */
#include <stdio.h>

typedef FILE *(*real_fopen_fn)(const char *, const char *);
static real_fopen_fn real_fopen_ptr = NULL;

static int is_pci_path(const char *path) {
    if (!path) return 0;
    return (strncmp(path, "/proc/bus/pci", 13) == 0 ||
            strncmp(path, "/sys/bus/pci", 12) == 0 ||
            strncmp(path, "/sys/devices/pci", 16) == 0);
}

FILE *fopen(const char *pathname, const char *mode) {
    if (!real_fopen_ptr)
        real_fopen_ptr = (real_fopen_fn)dlsym(RTLD_NEXT, "fopen");
    if (is_pci_path(pathname)) {
        debug_msg("FIX: fopen PCI redirect → /dev/null: ");
        debug_msg(pathname);
        debug_msg("\n");
        return real_fopen_ptr("/dev/null", mode);
    }
    return real_fopen_ptr(pathname, mode);
}

FILE *fopen64(const char *pathname, const char *mode) {
    return fopen(pathname, mode);
}

typedef int (*real_open_fn)(const char *, int, ...);
static real_open_fn real_open_ptr = NULL;

int open(const char *pathname, int flags, ...) {
    if (!real_open_ptr)
        real_open_ptr = (real_open_fn)dlsym(RTLD_NEXT, "open");
    if (is_pci_path(pathname)) {
        debug_msg("FIX: open PCI redirect → /dev/null\n");
        return real_open_ptr("/dev/null", O_RDONLY);
    }
    va_list ap;
    va_start(ap, flags);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE))
        mode = va_arg(ap, mode_t);
    va_end(ap);
    return real_open_ptr(pathname, flags, mode);
}

int open64(const char *pathname, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE))
        mode = va_arg(ap, mode_t);
    va_end(ap);
    return open(pathname, flags, mode);
}

/* bind/connect wrappers: filesystem AF_UNIX → abstract socket.
 * Only convert paths under the Android cache dir (Chromium internal sockets).
 * Do NOT convert /tmp/ paths — the steam client uses those for IPC and
 * doesn't have our wrapper, so both ends must agree on filesystem paths. */
static socklen_t make_abstract(const struct sockaddr *addr, socklen_t addrlen,
                               struct sockaddr_un *out) {
    if (!addr || addr->sa_family != AF_UNIX || addrlen <= sizeof(sa_family_t))
        return 0;
    const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
    if (un->sun_path[0] == '\0')
        return 0;

    /* Convert ALL filesystem Unix sockets to abstract.
     * FEX's overlay doesn't support filesystem socket creation.
     * Both steamwebhelper sides (server+client) use our wrapper in single-process. */

    memset(out, 0, sizeof(*out));
    out->sun_family = AF_UNIX;
    out->sun_path[0] = '\0';

    const char *src = un->sun_path;
    int len = 0;
    while (src[len] && len < 105) len++;
    memcpy(&out->sun_path[1], src, len);

    debug_msg("FIX: socket -> abstract: ");
    debug_msg(un->sun_path);
    debug_msg("\n");

    return offsetof(struct sockaddr_un, sun_path) + 1 + len;
}

typedef int (*real_bind_fn)(int, const struct sockaddr *, socklen_t);
static real_bind_fn real_bind_ptr = NULL;

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (!real_bind_ptr)
        real_bind_ptr = (real_bind_fn)dlsym(RTLD_NEXT, "bind");
    struct sockaddr_un abstract_addr;
    socklen_t abs_len = make_abstract(addr, addrlen, &abstract_addr);
    if (abs_len > 0) {
        int ret = real_bind_ptr(sockfd, (struct sockaddr *)&abstract_addr, abs_len);
        int bind_errno = errno;
        debug_int("FIX: bind abstract ret=", ret);
        if (ret < 0) {
            debug_int("FIX: bind errno=", bind_errno);
        }
        if (ret == 0) {
            /* Create dummy file at original path so stat/chmod/access checks
             * by steamwebhelper's post-bind verification find something.
             * Abstract sockets don't create filesystem entries. */
            const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
            long fd;
            register long r10 __asm__("r10") = 0666;
            __asm__ volatile ("syscall" : "=a"(fd)
                : "0"((long)SYS_openat), "D"(-100L), "S"(un->sun_path),
                  "d"((long)(O_WRONLY | O_CREAT | O_TRUNC)), "r"(r10)
                : "rcx", "r11", "memory");
            if (fd >= 0) {
                long dummy;
                __asm__ volatile ("syscall" : "=a"(dummy)
                    : "0"((long)SYS_close), "D"(fd)
                    : "rcx", "r11", "memory");
                debug_msg("FIX: bind dummy file OK: ");
                debug_msg(un->sun_path);
                debug_msg("\n");
            } else {
                debug_int("FIX: bind dummy file FAIL errno=", -fd);
            }
        }
        return ret;
    }
    return real_bind_ptr(sockfd, addr, addrlen);
}

typedef int (*real_connect_fn)(int, const struct sockaddr *, socklen_t);
static real_connect_fn real_connect_ptr = NULL;

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (!real_connect_ptr)
        real_connect_ptr = (real_connect_fn)dlsym(RTLD_NEXT, "connect");
    struct sockaddr_un abstract_addr;
    socklen_t abs_len = make_abstract(addr, addrlen, &abstract_addr);
    if (abs_len > 0)
        return real_connect_ptr(sockfd, (struct sockaddr *)&abstract_addr, abs_len);
    return real_connect_ptr(sockfd, addr, addrlen);
}

/* listen() wrapper: log when steamwebhelper starts listening on a socket */
typedef int (*real_listen_fn)(int, int);
static real_listen_fn real_listen_ptr = NULL;

int listen(int sockfd, int backlog) {
    if (!real_listen_ptr)
        real_listen_ptr = (real_listen_fn)dlsym(RTLD_NEXT, "listen");
    int ret = real_listen_ptr(sockfd, backlog);
    /* Try to identify which socket this is */
    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);
    if (getsockname(sockfd, (struct sockaddr *)&addr, &addrlen) == 0 &&
        addr.sun_family == AF_UNIX) {
        if (addr.sun_path[0] == '\0' && addrlen > sizeof(sa_family_t) + 1) {
            debug_msg("FIX: listen on abstract: ");
            debug_msg(&addr.sun_path[1]);
        } else if (addr.sun_path[0] != '\0') {
            debug_msg("FIX: listen on: ");
            debug_msg(addr.sun_path);
        } else {
            debug_msg("FIX: listen on unnamed unix");
        }
        debug_int(" ret=", ret);
    }
    return ret;
}

/* chmod wrapper: log + succeed on socket paths where abstract sockets
 * don't have real filesystem entries. Without this, chmod after bind
 * fails with ENOENT and steamwebhelper reports "Failed to bind shmem socket" */
typedef int (*real_chmod_fn)(const char *, mode_t);
static real_chmod_fn real_chmod_ptr = NULL;

int chmod(const char *pathname, mode_t mode) {
    if (!real_chmod_ptr)
        real_chmod_ptr = (real_chmod_fn)dlsym(RTLD_NEXT, "chmod");
    int ret = real_chmod_ptr(pathname, mode);
    if (ret != 0 && strstr(pathname, "steam_chrome_shmem")) {
        /* chmod on our dummy file might fail — force success.
         * The real permission control is on the abstract socket. */
        debug_msg("FIX: chmod on shmem path forced OK: ");
        debug_msg(pathname);
        debug_msg("\n");
        return 0;
    }
    return ret;
}

/* ============================================================
 * FD 11 IPC MONITORING: Steam client uses FD 11 to communicate
 * with steamwebhelper. We need to see if steamwebhelper ever
 * writes to FD 11 (the "ready" signal) and if it reads from it.
 * ============================================================ */
#include <sys/uio.h>

static volatile int fd11_write_count = 0;
static volatile int fd11_read_count = 0;

/* write() wrapper — monitor FD 11 writes */
typedef ssize_t (*real_write_fn)(int, const void *, size_t);
static real_write_fn real_write_ptr = NULL;

ssize_t write(int fd, const void *buf, size_t count) {
    if (!real_write_ptr)
        real_write_ptr = (real_write_fn)dlsym(RTLD_NEXT, "write");

    /* Intercept the 1-byte '0' status write on FD 11 → change to '1' (ready) */
    if (fd == 11 && count == 1 && buf && *(const unsigned char *)buf == '0') {
        char one = '1';
        ssize_t ret = real_write_ptr(fd, &one, 1);
        int n = __sync_fetch_and_add(&fd11_write_count, 1);
        if (n < 50) {
            debug_msg("FIX: WRITE fd=11 INTERCEPTED: '0' → '1' (fake ready)\n");
            debug_int("  ret=", (long)ret);
        }
        return ret;
    }

    ssize_t ret = real_write_ptr(fd, buf, count);
    if (fd == 11) {
        int n = __sync_fetch_and_add(&fd11_write_count, 1);
        if (n < 50) {
            debug_int("FIX: WRITE fd=11 pid=", get_pid());
            debug_int("  count=", (long)count);
            debug_int("  ret=", (long)ret);
            if (ret > 0 && buf) debug_hexdump("  data: ", buf, (int)ret);
        }
    }
    return ret;
}

/* writev() wrapper — monitor FD 11 vector writes */
typedef ssize_t (*real_writev_fn)(int, const struct iovec *, int);
static real_writev_fn real_writev_ptr = NULL;

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    if (!real_writev_ptr)
        real_writev_ptr = (real_writev_fn)dlsym(RTLD_NEXT, "writev");
    ssize_t ret = real_writev_ptr(fd, iov, iovcnt);
    if (fd == 11) {
        int n = __sync_fetch_and_add(&fd11_write_count, 1);
        if (n < 50) {
            debug_int("FIX: WRITEV fd=11 pid=", get_pid());
            debug_int("  iovcnt=", (long)iovcnt);
            debug_int("  ret=", (long)ret);
            if (ret > 0 && iov && iovcnt > 0 && iov[0].iov_base)
                debug_hexdump("  iov0: ", iov[0].iov_base, (int)(iov[0].iov_len > 16 ? 16 : iov[0].iov_len));
        }
    }
    return ret;
}

/* send() wrapper */
typedef ssize_t (*real_send_fn)(int, const void *, size_t, int);
static real_send_fn real_send_ptr = NULL;

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    if (!real_send_ptr)
        real_send_ptr = (real_send_fn)dlsym(RTLD_NEXT, "send");
    ssize_t ret = real_send_ptr(sockfd, buf, len, flags);
    if (sockfd == 11) {
        int n = __sync_fetch_and_add(&fd11_write_count, 1);
        if (n < 50) {
            debug_int("FIX: SEND fd=11 pid=", get_pid());
            debug_int("  len=", (long)len);
            debug_int("  ret=", (long)ret);
            if (ret > 0 && buf) debug_hexdump("  data: ", buf, (int)ret);
        }
    }
    return ret;
}

/* sendmsg() wrapper */
typedef ssize_t (*real_sendmsg_fn)(int, const struct msghdr *, int);
static real_sendmsg_fn real_sendmsg_ptr = NULL;

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    if (!real_sendmsg_ptr)
        real_sendmsg_ptr = (real_sendmsg_fn)dlsym(RTLD_NEXT, "sendmsg");
    ssize_t ret = real_sendmsg_ptr(sockfd, msg, flags);
    if (sockfd == 11) {
        int n = __sync_fetch_and_add(&fd11_write_count, 1);
        if (n < 50) {
            debug_int("FIX: SENDMSG fd=11 pid=", get_pid());
            debug_int("  iovlen=", (long)(msg ? msg->msg_iovlen : -1));
            debug_int("  ret=", (long)ret);
        }
    }
    return ret;
}

/* read() wrapper — monitor FD 11 reads */
typedef ssize_t (*real_read_fn)(int, void *, size_t);
static real_read_fn real_read_ptr = NULL;

ssize_t read(int fd, void *buf, size_t count) {
    if (!real_read_ptr)
        real_read_ptr = (real_read_fn)dlsym(RTLD_NEXT, "read");
    ssize_t ret = real_read_ptr(fd, buf, count);
    if (fd == 11) {
        int n = __sync_fetch_and_add(&fd11_read_count, 1);
        if (n < 50) {
            debug_int("FIX: READ fd=11 pid=", get_pid());
            debug_int("  count=", (long)count);
            debug_int("  ret=", (long)ret);
            if (ret > 0 && buf) debug_hexdump("  data: ", buf, (int)ret);
        }
    }
    return ret;
}

/* recv() wrapper */
typedef ssize_t (*real_recv_fn)(int, void *, size_t, int);
static real_recv_fn real_recv_ptr = NULL;

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    if (!real_recv_ptr)
        real_recv_ptr = (real_recv_fn)dlsym(RTLD_NEXT, "recv");
    ssize_t ret = real_recv_ptr(sockfd, buf, len, flags);
    if (sockfd == 11) {
        int n = __sync_fetch_and_add(&fd11_read_count, 1);
        if (n < 50) {
            debug_int("FIX: RECV fd=11 pid=", get_pid());
            debug_int("  len=", (long)len);
            debug_int("  ret=", (long)ret);
            if (ret > 0 && buf) debug_hexdump("  data: ", buf, (int)ret);
        }
    }
    return ret;
}

/* recvmsg() wrapper */
typedef ssize_t (*real_recvmsg_fn)(int, struct msghdr *, int);
static real_recvmsg_fn real_recvmsg_ptr = NULL;

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
    if (!real_recvmsg_ptr)
        real_recvmsg_ptr = (real_recvmsg_fn)dlsym(RTLD_NEXT, "recvmsg");
    ssize_t ret = real_recvmsg_ptr(sockfd, msg, flags);
    if (sockfd == 11) {
        int n = __sync_fetch_and_add(&fd11_read_count, 1);
        if (n < 50) {
            debug_int("FIX: RECVMSG fd=11 pid=", get_pid());
            debug_int("  ret=", (long)ret);
        }
    }
    return ret;
}

/* accept/accept4 wrapper — detect if anyone connects to the shmem socket */
typedef int (*real_accept_fn)(int, struct sockaddr *, socklen_t *);
static real_accept_fn real_accept_ptr = NULL;
typedef int (*real_accept4_fn)(int, struct sockaddr *, socklen_t *, int);
static real_accept4_fn real_accept4_ptr = NULL;
static volatile int accept_count = 0;
static volatile int accept_success_count = 0;

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    if (!real_accept_ptr)
        real_accept_ptr = (real_accept_fn)dlsym(RTLD_NEXT, "accept");
    int ret = real_accept_ptr(sockfd, addr, addrlen);
    int saved_errno = errno;
    int n = __sync_fetch_and_add(&accept_count, 1);
    if (ret >= 0) {
        int sn = __sync_fetch_and_add(&accept_success_count, 1);
        if (sn < 10) {
            debug_int("FIX: accept() SUCCESS sockfd=", sockfd);
            debug_int("  new_fd=", ret);
        }
    } else if (n < 5) {
        debug_int("FIX: accept() sockfd=", sockfd);
        debug_int("  ret=", ret);
        debug_int("  errno=", saved_errno);
    }
    errno = saved_errno;
    return ret;
}

int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
    if (!real_accept4_ptr)
        real_accept4_ptr = (real_accept4_fn)dlsym(RTLD_NEXT, "accept4");
    int ret = real_accept4_ptr(sockfd, addr, addrlen, flags);
    int saved_errno = errno;
    int n = __sync_fetch_and_add(&accept_count, 1);
    if (ret >= 0) {
        int sn = __sync_fetch_and_add(&accept_success_count, 1);
        if (sn < 10) {
            debug_int("FIX: accept4() SUCCESS sockfd=", sockfd);
            debug_int("  new_fd=", ret);
            debug_int("  flags=", flags);
        }
    } else if (n < 5) {
        debug_int("FIX: accept4() sockfd=", sockfd);
        debug_int("  ret=", ret);
        debug_int("  errno=", saved_errno);
        debug_int("  flags=", flags);
    }
    errno = saved_errno;
    return ret;
}

/* Save the ValveIPCSharedObj fd so heartbeat can monitor it */
static volatile int valve_ipc_fd = -1;

/* shm_open/shm_unlink wrappers: redirect /dev/shm to a real Android path.
 * Under FEX on Android, /dev/shm doesn't exist or isn't a tmpfs.
 * Steam's shmemdrop.h uses shm_open for SteamController_*_mem and chrome IPC.
 * Redirect to /data/data/com.mediatek.steamlauncher/cache/s/shm/ which is
 * a real Android directory where file creation and mmap work. */

#define SHM_REDIR_DIR "/data/data/com.mediatek.steamlauncher/cache/s/shm"

static void build_shm_path(char *out, int outsz, const char *name) {
    int n = 0;
    const char *d = SHM_REDIR_DIR;
    while (*d && n < outsz - 2) out[n++] = *d++;
    /* name typically starts with '/' per POSIX, but handle both */
    if (name[0] != '/') out[n++] = '/';
    const char *s = name;
    while (*s && n < outsz - 1) out[n++] = *s++;
    out[n] = '\0';
}

static volatile int shm_open_count = 0;

int shm_open(const char *name, int oflag, mode_t mode) {
    char path[256];
    build_shm_path(path, sizeof(path), name);
    int real_flags;
    if (oflag & O_CREAT) {
        /* Caller wants to create: strip O_EXCL, force O_RDWR */
        real_flags = (oflag & ~O_EXCL) | O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW;
    } else {
        /* Caller is polling/reading (O_RDONLY): preserve original flags.
         * Steam client polls shm_open("Shm_<hash>", O_RDONLY) to detect
         * when the webhelper has created the SHMemStream. If we force O_CREAT,
         * we create an empty file that the client reads as corrupt. */
        real_flags = oflag | O_CLOEXEC | O_NOFOLLOW;
    }
    if (!mode) mode = 0666;
    /* Use raw openat syscall to avoid our open() wrapper */
    long fd;
    register long r10_mode __asm__("r10") = (long)mode;
    __asm__ volatile ("syscall" : "=a"(fd)
        : "0"((long)SYS_openat), "D"(-100L), "S"(path),
          "d"((long)real_flags), "r"(r10_mode)
        : "rcx", "r11", "memory");
    /* Only log first 30 calls */
    int count = __sync_fetch_and_add(&shm_open_count, 1);
    if (count < 30) {
        debug_msg("FIX: shm_open(");
        debug_msg(name);
        debug_int(") flags=", oflag);
        debug_int("  real_flags=", real_flags);
        debug_int("  mode=", (long)mode);
        debug_msg("  path=");
        debug_msg(path);
        debug_int("\n  fd=", fd);
    }
    /* Fix return: shm_open returns -1 on failure, not raw -errno */
    if (fd < 0) {
        errno = (int)(-fd);
        return -1;
    }
    /* Save the first ValveIPCSharedObj fd for heartbeat monitoring */
    if (fd >= 0 && strstr(name, "ValveIPCSharedObj") && valve_ipc_fd < 0) {
        valve_ipc_fd = (int)fd;
        debug_int("FIX: saved ValveIPC fd=", fd);
    }
    return (int)fd;
}

int shm_unlink(const char *name) {
    char path[256];
    build_shm_path(path, sizeof(path), name);
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret)
        : "0"((long)SYS_unlinkat), "D"(-100L), "S"(path), "d"(0L)
        : "rcx", "r11", "memory");
    debug_msg("FIX: shm_unlink(");
    debug_msg(name);
    debug_int(") ret=", ret);
    return (ret < 0) ? -1 : 0;
}

/* Heartbeat thread: logs every 15 seconds to confirm the process is alive.
 * Reports thread count from /proc/self/stat to detect hangs. */
static void *heartbeat_func(void *arg) {
    (void)arg;
    int beat = 0;
    while (1) {
        struct timespec ts = { .tv_sec = 15, .tv_nsec = 0 };
        long ret;
        __asm__ volatile ("syscall" : "=a"(ret)
            : "0"((long)SYS_nanosleep), "D"(&ts), "S"((long)0)
            : "rcx", "r11", "memory");
        beat++;
        debug_int("FIX: heartbeat #", beat);
        /* Read thread count from /proc/self/stat (field 20) */
        char buf[512];
        long fd;
        __asm__ volatile ("syscall" : "=a"(fd)
            : "0"((long)SYS_openat), "D"(-100L),
              "S"("/proc/self/stat"), "d"((long)O_RDONLY)
            : "rcx", "r11", "memory");
        if (fd >= 0) {
            long n;
            __asm__ volatile ("syscall" : "=a"(n)
                : "0"((long)SYS_read), "D"(fd), "S"(buf), "d"(511L)
                : "rcx", "r11", "memory");
            __asm__ volatile ("syscall" : "=a"(ret)
                : "0"((long)SYS_close), "D"(fd)
                : "rcx", "r11", "memory");
            if (n > 0) {
                buf[n] = '\0';
                /* Find field 20 (num_threads): skip past ')' then count spaces */
                char *p = buf;
                while (*p && *p != ')') p++;
                if (*p) p++;
                int field = 2; /* we're at field 2 after ')' */
                while (*p && field < 20) {
                    if (*p == ' ') field++;
                    p++;
                }
                /* p now points to the thread count */
                long threads = 0;
                while (*p >= '0' && *p <= '9') {
                    threads = threads * 10 + (*p - '0');
                    p++;
                }
                debug_int("  threads=", threads);
            }
        }
        /* Report FD 11 I/O activity and accept count */
        debug_int("  fd11_wr=", (long)fd11_write_count);
        debug_int("  fd11_rd=", (long)fd11_read_count);
        debug_int("  accepts=", (long)accept_count);
        debug_int("  accept_ok=", (long)accept_success_count);
        /* Read ValveIPCSharedObj at multiple offsets to find ready flag */
        int vipc = valve_ipc_fd;
        if (vipc >= 0 && beat <= 3) {
            char shmbuf[32];
            long offsets[] = { 0, 256, 512, 1024 };
            for (int oi = 0; oi < 4; oi++) {
                long nr;
                register long r10_off __asm__("r10") = offsets[oi];
                __asm__ volatile ("syscall" : "=a"(nr)
                    : "0"((long)SYS_pread64), "D"((long)vipc), "S"(shmbuf),
                      "d"(32L), "r"(r10_off)
                    : "rcx", "r11", "memory");
                if (nr > 0) {
                    /* Check if any bytes are non-zero */
                    int has_data = 0;
                    for (int j = 0; j < nr; j++) if (shmbuf[j]) has_data = 1;
                    if (has_data) {
                        debug_int("  vipc@", offsets[oi]);
                        debug_hexdump("    ", shmbuf, (int)nr);
                    }
                }
            }
        }
    }
    return NULL;
}

/* FD 11 "fake ready" thread: the webhelper never signals "ready" because
 * Chromium's init never completes in single-process mode (V8 proxy resolver).
 * The steam client waits ~60s on FD 11 for a ready message then gives up.
 *
 * We monitor FD 11 writes: after the handshake (2 writes: 56-byte echo + '0'),
 * we wait 5 seconds then try writing additional messages to FD 11 to fake
 * the ready signal. We try multiple formats since we don't know the protocol. */
static volatile int fd11_ready_sent = 0;

static void *fd11_ready_func(void *arg) {
    (void)arg;
    /* The handshake happens in a different process image (before exec),
     * so fd11_write_count is always 0 here. Just wait a fixed 20s for
     * Chromium to init, then send the ready signal. */
    debug_msg("FIX: fd11_ready: waiting 20s for Chromium init...\n");
    struct timespec ts = { .tv_sec = 20, .tv_nsec = 0 };
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret)
        : "0"((long)SYS_nanosleep), "D"(&ts), "S"((long)0)
        : "rcx", "r11", "memory");

    /* Check that FD 11 is still open */
    struct stat st;
    if (fstat(11, &st) != 0) {
        debug_msg("FIX: fd11_ready: FD 11 closed, aborting\n");
        return NULL;
    }

    debug_msg("FIX: fd11_ready: sending fake ready signal to FD 11\n");

    if (!real_write_ptr)
        real_write_ptr = (real_write_fn)dlsym(RTLD_NEXT, "write");

    /* First: read any pending data on FD 11 (the handshake messages from steam client) */
    unsigned char rdbuf[256];
    for (int i = 0; i < 5; i++) {
        /* Non-blocking read to drain pending data */
        int flags = fcntl(11, F_GETFL);
        fcntl(11, F_SETFL, flags | O_NONBLOCK);
        ssize_t r = read(11, rdbuf, sizeof(rdbuf));
        fcntl(11, F_SETFL, flags); /* restore */
        if (r > 0) {
            debug_int("FIX: fd11_ready: drained pending data, len=", (long)r);
            debug_hexdump("  data: ", rdbuf, r > 60 ? 60 : (int)r);
        } else {
            break;
        }
    }

    /* Write sdPC echo (same as the handshake reply the wrapper already sent) */
    unsigned char ready_msg[56];
    memset(ready_msg, 0, sizeof(ready_msg));
    ready_msg[0] = 's'; ready_msg[1] = 'd'; ready_msg[2] = 'P'; ready_msg[3] = 'C';
    ready_msg[4] = 0x02; /* version = 2 */
    ready_msg[8] = 0x01; /* type = 1 (same as handshake) */

    ssize_t n = real_write_ptr(11, ready_msg, 56);
    debug_int("FIX: fd11_ready: wrote sdPC type=1, ret=", (long)n);

    if (n == 56) {
        /* Write '1' as status (handshake sent '0', try '1' for ready) */
        char one = '1';
        n = real_write_ptr(11, &one, 1);
        debug_int("FIX: fd11_ready: wrote '1' status, ret=", (long)n);
    }

    fd11_ready_sent = 1;
    return NULL;
}

/* ============================================================
 * FAKE SHMemStream: Creates the shared memory + socket that the
 * steam client polls for. Chromium never fully initializes in
 * single-process mode, so CMsgBrowserReady is never sent and the
 * SHMemStream is never created. We fake it.
 *
 * Flow:
 * 1. 32-bit shim detects Shm_ polling failures, writes coordination file
 * 2. This thread reads the coordination file to get the Shm_ name
 * 3. Creates the shared memory file with valid SHMemStream header
 * 4. Creates + binds + listens on the shmem Unix socket
 * 5. Steam client's next poll succeeds → connects to socket
 * ============================================================ */

static void int_to_str(char *buf, long val) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char digits[20];
    int n = 0;
    while (val > 0) { digits[n++] = '0' + (val % 10); val /= 10; }
    for (int i = 0; i < n; i++) buf[i] = digits[n - 1 - i];
    buf[n] = '\0';
}

static void *fake_shmem_func(void *arg) {
    (void)arg;
    debug_msg("FIX: fake_shmem: watching for poll target...\n");

    char coord_path[256];
    build_shm_path(coord_path, sizeof(coord_path), "/_shm_poll_target");

    /* Poll for coordination file from 32-bit shim (every 200ms, up to 120s) */
    char shm_name[64];
    memset(shm_name, 0, sizeof(shm_name));

    for (int i = 0; i < 600; i++) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000000L };
        long ret;
        __asm__ volatile ("syscall" : "=a"(ret)
            : "0"((long)SYS_nanosleep), "D"(&ts), "S"((long)0)
            : "rcx", "r11", "memory");

        long cfd;
        __asm__ volatile ("syscall" : "=a"(cfd)
            : "0"((long)SYS_openat), "D"(-100L), "S"(coord_path),
              "d"((long)O_RDONLY)
            : "rcx", "r11", "memory");

        if (cfd >= 0) {
            long n;
            __asm__ volatile ("syscall" : "=a"(n)
                : "0"((long)SYS_read), "D"(cfd), "S"(shm_name), "d"(63L)
                : "rcx", "r11", "memory");
            __asm__ volatile ("syscall" : "=a"(ret)
                : "0"((long)SYS_close), "D"(cfd)
                : "rcx", "r11", "memory");
            if (n > 0) {
                shm_name[n] = '\0';
                debug_msg("FIX: fake_shmem: got poll target: ");
                debug_msg(shm_name);
                debug_msg("\n");
                break;
            }
        }
    }

    if (!shm_name[0]) {
        debug_msg("FIX: fake_shmem: no poll target found after 120s, giving up\n");
        return NULL;
    }

    /* Parse steam PID from /proc/self/cmdline (-steampid=N) */
    long spid = 0;
    {
        char cmdline[4096];
        long cmdfd;
        __asm__ volatile ("syscall" : "=a"(cmdfd)
            : "0"((long)SYS_openat), "D"(-100L),
              "S"("/proc/self/cmdline"), "d"((long)O_RDONLY)
            : "rcx", "r11", "memory");
        if (cmdfd >= 0) {
            long n;
            __asm__ volatile ("syscall" : "=a"(n)
                : "0"((long)SYS_read), "D"(cmdfd), "S"(cmdline), "d"(4095L)
                : "rcx", "r11", "memory");
            long dummy;
            __asm__ volatile ("syscall" : "=a"(dummy)
                : "0"((long)SYS_close), "D"(cmdfd)
                : "rcx", "r11", "memory");
            if (n > 0) {
                cmdline[n] = '\0';
                /* Search for -steampid= in null-separated args */
                for (long j = 0; j < n - 10; j++) {
                    if (cmdline[j] == '-' && strncmp(&cmdline[j], "-steampid=", 10) == 0) {
                        for (long k = j + 10; k < n && cmdline[k] >= '0' && cmdline[k] <= '9'; k++)
                            spid = spid * 10 + (cmdline[k] - '0');
                        break;
                    }
                }
            }
        }
    }
    if (spid == 0) {
        /* Fallback: parent PID */
        __asm__ volatile ("syscall" : "=a"(spid)
            : "0"((long)SYS_getppid)
            : "rcx", "r11", "memory");
    }
    debug_int("FIX: fake_shmem: steam PID=", spid);

    long my_pid = get_pid();
    long my_uid;
    __asm__ volatile ("syscall" : "=a"(my_uid)
        : "0"((long)SYS_getuid)
        : "rcx", "r11", "memory");
    debug_int("FIX: fake_shmem: uid=", my_uid);

    /* Build stream name: SteamChrome_MasterStream_<spid>_<mypid> */
    char stream_name[64];
    {
        char *p = stream_name;
        const char *pfx = "SteamChrome_MasterStream_";
        while (*pfx) *p++ = *pfx++;
        char tmp[20];
        int_to_str(tmp, spid);
        for (int i = 0; tmp[i]; i++) *p++ = tmp[i];
        *p++ = '_';
        int_to_str(tmp, my_pid);
        for (int i = 0; tmp[i]; i++) *p++ = tmp[i];
        *p = '\0';
    }
    debug_msg("FIX: fake_shmem: stream=");
    debug_msg(stream_name);
    debug_msg("\n");

    /* Step 1: Create Shm_ file with SHMemStream header.
     * Use our own shm_open() wrapper — it's proven to work for other Shm_ files
     * (build_shm_path + SYS_openat with the right overlay flags). */
    debug_msg("FIX: fake_shmem: creating via shm_open: ");
    debug_msg(shm_name);
    debug_msg("\n");

    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    debug_int("FIX: fake_shmem: shm_open fd=", (long)shm_fd);
    if (shm_fd < 0) {
        debug_int("FIX: fake_shmem: create Shm_ failed, errno=", (long)errno);
        return NULL;
    }

    /* SHMemStream header (76 bytes):
     *  0: header_size  (uint32 LE) = 0x4c = 76
     *  4: data_offset  (uint32 LE) = 0x4c = 76
     *  8: buffer_size  (uint32 LE) = 0x2000 = 8192
     * 12: (padding)    = 0
     * 16: version      (uint32 LE) = 1
     * 20: count        (uint32 LE) = 1
     * 24: steam_pid    (uint32 LE)
     * 28: stream_name  (48 bytes, null-terminated) */
    unsigned char hdr[76];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 0x4c;                                    /* header_size */
    hdr[4] = 0x4c;                                    /* data_offset */
    hdr[8] = 0x00; hdr[9] = 0x20;                     /* buffer_size = 8192 */
    hdr[16] = 0x01;                                    /* version */
    hdr[20] = 0x01;                                    /* count */
    hdr[24] = (unsigned char)(spid & 0xff);            /* steam PID LE */
    hdr[25] = (unsigned char)((spid >> 8) & 0xff);
    hdr[26] = (unsigned char)((spid >> 16) & 0xff);
    hdr[27] = (unsigned char)((spid >> 24) & 0xff);
    { /* stream name at offset 28, max 47 chars + NUL */
        const char *s = stream_name;
        for (int i = 0; *s && i < 47; i++) hdr[28 + i] = (unsigned char)*s++;
    }

    long wr;
    __asm__ volatile ("syscall" : "=a"(wr)
        : "0"((long)SYS_write), "D"((long)shm_fd), "S"(hdr), "d"(76L)
        : "rcx", "r11", "memory");
    debug_int("FIX: fake_shmem: header write ret=", wr);

    /* ftruncate to header + ring buffer (76 + 8192 = 8268) */
    {
        long total = 76 + 8192;
        __asm__ volatile ("syscall" : "=a"(wr)
            : "0"((long)SYS_ftruncate), "D"((long)shm_fd), "S"(total)
            : "rcx", "r11", "memory");
    }
    debug_int("FIX: fake_shmem: ftruncate ret=", wr);

    close(shm_fd);

    /* Verify: try to open it back with O_RDONLY (same as steam client's poll) */
    int verify_fd = shm_open(shm_name, O_RDONLY, 0);
    debug_int("FIX: fake_shmem: VERIFY shm_open O_RDONLY fd=", (long)verify_fd);
    if (verify_fd >= 0) {
        unsigned char vbuf[8];
        if (!real_read_ptr)
            real_read_ptr = (real_read_fn)dlsym(RTLD_NEXT, "read");
        ssize_t vr = real_read_ptr(verify_fd, vbuf, 8);
        debug_int("FIX: fake_shmem: VERIFY read ret=", (long)vr);
        if (vr > 0)
            debug_hexdump("  verify hdr: ", vbuf, (int)vr);
        close(verify_fd);
    } else {
        debug_int("FIX: fake_shmem: VERIFY FAILED errno=", (long)errno);
    }

    /* Step 2: Create shmem socket at /tmp/steam_chrome_shmem_uid<uid>_spid<spid> */
    char sock_path[128];
    {
        char *p = sock_path;
        const char *s = "/tmp/steam_chrome_shmem_uid";
        while (*s) *p++ = *s++;
        char tmp[20];
        int_to_str(tmp, my_uid);
        for (int i = 0; tmp[i]; i++) *p++ = tmp[i];
        s = "_spid";
        while (*s) *p++ = *s++;
        int_to_str(tmp, spid);
        for (int i = 0; tmp[i]; i++) *p++ = tmp[i];
        *p = '\0';
    }
    debug_msg("FIX: fake_shmem: socket path: ");
    debug_msg(sock_path);
    debug_msg("\n");

    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) {
        debug_msg("FIX: fake_shmem: socket() failed\n");
        return NULL;
    }
    debug_int("FIX: fake_shmem: socket fd=", sockfd);

    /* bind() goes through our wrapper → converts to abstract socket */
    struct sockaddr_un saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sun_family = AF_UNIX;
    {
        const char *s = sock_path;
        int i = 0;
        while (*s && i < (int)sizeof(saddr.sun_path) - 1) saddr.sun_path[i++] = *s++;
        saddr.sun_path[i] = '\0';
    }

    int bret = bind(sockfd, (struct sockaddr *)&saddr,
                    offsetof(struct sockaddr_un, sun_path) + strlen(sock_path) + 1);
    debug_int("FIX: fake_shmem: bind ret=", bret);
    if (bret < 0) {
        debug_int("FIX: fake_shmem: bind errno=", errno);
        close(sockfd);
        return NULL;
    }

    int lret = listen(sockfd, 5);
    debug_int("FIX: fake_shmem: listen ret=", lret);

    debug_msg("FIX: fake_shmem: READY — waiting for steam client connection\n");

    /* Accept loop — handle connections from steam client */
    for (int ai = 0; ai < 10; ai++) {
        int cfd = accept(sockfd, NULL, NULL);
        if (cfd >= 0) {
            debug_int("FIX: fake_shmem: CONNECTED! client_fd=", cfd);
            /* Read whatever the client sends, log it for protocol analysis */
            for (int ri = 0; ri < 100; ri++) {
                unsigned char rbuf[1024];
                if (!real_read_ptr)
                    real_read_ptr = (real_read_fn)dlsym(RTLD_NEXT, "read");
                ssize_t r = real_read_ptr(cfd, rbuf, sizeof(rbuf));
                if (r <= 0) {
                    debug_int("FIX: fake_shmem: client disconnected, r=", (long)r);
                    break;
                }
                debug_int("FIX: fake_shmem: recv len=", (long)r);
                debug_hexdump("  data: ", rbuf, r > 60 ? 60 : (int)r);
            }
            close(cfd);
        } else {
            debug_int("FIX: fake_shmem: accept failed, errno=", errno);
            /* EAGAIN/EINTR: retry; other: break */
            if (errno != 4 && errno != 11) break;
        }
    }

    close(sockfd);
    debug_msg("FIX: fake_shmem: thread exiting\n");
    return NULL;
}

__attribute__((constructor))
static void install_fixes(void) {
    real_sigaction_ptr = (real_sigaction_fn)dlsym(RTLD_NEXT, "sigaction");
    real_syscall_ptr = (real_syscall_fn)dlsym(RTLD_NEXT, "syscall");
    if (!real_sigaction_ptr) {
        debug_msg("FIX: WARNING - could not resolve real sigaction!\n");
        return;
    }

    struct rlimit rl;
    rl.rlim_cur = (unsigned long)-1;
    rl.rlim_max = (unsigned long)-1;
    setrlimit(RLIMIT_AS, &rl);
    rl.rlim_cur = 65536;
    rl.rlim_max = 65536;
    setrlimit(RLIMIT_NOFILE, &rl);

    /* crash_handler for SIGTRAP/SIGILL/SIGSEGV/SIGBUS — parks in nanosleep */
    struct sigaction sa;
    int crash_sigs[] = { SIGTRAP, SIGILL, SIGSEGV, SIGBUS };
    for (int i = 0; i < 4; i++) {
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = crash_handler;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        real_sigaction_ptr(crash_sigs[i], &sa, NULL);
    }

    /* SIGABRT: ignore */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    real_sigaction_ptr(SIGABRT, &sa, NULL);

    /* NO SIGSYS handler — FEX-2601 handles SIGSYS internally.
     * v62-v78: setcontext handler → POISONED process (1 thread)
     * v80c: simple RIP+=2 handler → FEX ignores RAX → garbage fd → SIGSEGV
     * Conclusion: ANY SIGSYS handler conflicts with FEX's signal dispatch. */

    mkdir("/home/user/.steam/debian-installation/config/htmlcache", 0755);
    mkdir(SHM_REDIR_DIR, 0777);

    /* Initialize robust_list from TCB scan */
    void *tcb_head = find_tcb_robust_head();
    if (tcb_head) {
        robust_list_saved = tcb_head;
        robust_list_saved_len = 24;
        if (real_syscall_ptr) {
            long ret = real_syscall_ptr(SYS_set_robust_list, (long)tcb_head, 24L);
            debug_int("FIX:   set_robust_list ret=", ret);
        }
        debug_msg("FIX:   robust_list initialized OK\n");
    } else {
        debug_msg("FIX:   WARNING: no robust_list_head in TCB\n");
    }

    /* Re-register robust_list in forked children */
    pthread_atfork(NULL, NULL, child_fork_handler);

    /* Check if FD 11 (parent IPC socket) is open.
     * Steam passes -child-update-ui-socket 11 to steamwebhelper.
     * If FD 11 is closed, the main IPC channel to steam is broken. */
    {
        struct stat st;
        if (fstat(11, &st) == 0) {
            debug_msg("FIX: FD 11 (parent IPC socket) OPEN, type=");
            if (S_ISSOCK(st.st_mode)) debug_msg("socket\n");
            else if (S_ISFIFO(st.st_mode)) debug_msg("pipe\n");
            else debug_int("other mode=", st.st_mode & S_IFMT);
        } else {
            debug_msg("FIX: FD 11 (parent IPC socket) CLOSED!\n");
        }
        /* Also check FDs 3-15 for any open sockets/pipes */
        for (int fd = 3; fd <= 15; fd++) {
            if (fstat(fd, &st) == 0 && (S_ISSOCK(st.st_mode) || S_ISFIFO(st.st_mode))) {
                debug_int("FIX: FD open: fd=", fd);
            }
        }
    }

    /* Start heartbeat thread — logs every 15s to confirm process is alive. */
    {
        pthread_t hb_thread;
        pthread_create(&hb_thread, NULL, heartbeat_func, NULL);
        pthread_detach(hb_thread);
    }

    /* Start FD 11 fake ready thread — only in the process where FD 11 is open */
    {
        struct stat st;
        if (fstat(11, &st) == 0 && S_ISSOCK(st.st_mode)) {
            pthread_t ready_thread;
            pthread_create(&ready_thread, NULL, fd11_ready_func, NULL);
            pthread_detach(ready_thread);
            debug_msg("FIX: started fd11_ready thread\n");
        }
    }

    /* Start fake SHMemStream thread — creates shared memory + socket
     * that the steam client polls for, since Chromium never inits fully */
    {
        pthread_t shmem_thread;
        pthread_create(&shmem_thread, NULL, fake_shmem_func, NULL);
        pthread_detach(shmem_thread);
        debug_msg("FIX: started fake_shmem thread\n");
    }

    debug_msg("FIX-v93: + fake SHMemStream\n");
}
