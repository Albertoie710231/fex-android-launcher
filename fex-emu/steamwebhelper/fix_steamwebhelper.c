/* v109: Re-enable CMsgBrowserReady patching on Ring A (server→client).
 *       Shm_99462695 IS the webhelper→client channel. v108's assumption that
 *       Ring A was client→server was WRONG. The "Invalid command" was pre-existing.
 *       Also: deploy steamwebhelper.sh to correct filename.
 *
 *       v108: removed --single-process from steamwebhelper.sh, stopped Ring A patching.
 *       v107: persistent SHM patcher thread.
 *       v106: X11 abstract socket rewrite in 32-bit shim.
 *       v103: Auto-detect IPC fd from -child-update-ui-socket cmdline arg.
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
#include <sys/mman.h>

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

/* v103: Auto-detect IPC fd from Steam's -child-update-ui-socket cmdline arg.
 * Steam client passes the IPC socket fd number, which may be 11, 13, or other.
 * -1 means no IPC fd found (process is not the webhelper child). */
static volatile int ipc_fd = -1;

static void detect_ipc_fd(void) {
    /* Read /proc/self/cmdline (NUL-separated args) */
    char cmdline[4096];
    long fd;
    __asm__ volatile ("syscall" : "=a"(fd)
        : "0"((long)SYS_openat), "D"(-100L),
          "S"("/proc/self/cmdline"), "d"((long)O_RDONLY)
        : "rcx", "r11", "memory");
    if (fd < 0) return;
    long n;
    __asm__ volatile ("syscall" : "=a"(n)
        : "0"((long)SYS_read), "D"(fd), "S"(cmdline), "d"(4095L)
        : "rcx", "r11", "memory");
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret)
        : "0"((long)SYS_close), "D"(fd)
        : "rcx", "r11", "memory");
    if (n <= 0) return;
    cmdline[n] = '\0';

    /* Scan NUL-separated args for "-child-update-ui-socket" */
    const char *needle = "-child-update-ui-socket";
    int needle_len = 23;
    int pos = 0;
    while (pos < n) {
        /* Find end of current arg */
        int arg_start = pos;
        while (pos < n && cmdline[pos] != '\0') pos++;
        int arg_len = pos - arg_start;
        pos++; /* skip NUL */

        /* Check if this arg matches */
        if (arg_len == needle_len) {
            int match = 1;
            for (int i = 0; i < needle_len; i++) {
                if (cmdline[arg_start + i] != needle[i]) { match = 0; break; }
            }
            if (match && pos < n) {
                /* Next arg is the fd number */
                long val = 0;
                int dpos = pos;
                while (dpos < n && cmdline[dpos] >= '0' && cmdline[dpos] <= '9') {
                    val = val * 10 + (cmdline[dpos] - '0');
                    dpos++;
                }
                if (val > 0 && val < 65536) {
                    ipc_fd = (int)val;
                    debug_int("FIX: detected IPC fd from cmdline: ", val);
                    return;
                }
            }
        }
    }

    /* Fallback: scan fds 3-20 for open sockets (skip stdin/out/err) */
    for (int sfd = 3; sfd <= 20; sfd++) {
        struct stat st;
        if (fstat(sfd, &st) == 0 && S_ISSOCK(st.st_mode)) {
            /* First socket found — might be it. Don't use this as
             * primary detection, just log for debugging. */
            debug_int("FIX: fallback: socket fd=", sfd);
        }
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
 * IPC FD MONITORING: Steam client passes an IPC socket to
 * steamwebhelper via -child-update-ui-socket <N>.
 * v103: auto-detect fd number (was hardcoded 11, but can be 13+).
 * ============================================================ */
#include <sys/uio.h>

static volatile int ipc_write_count = 0;
static volatile int ipc_read_count = 0;

/* write() wrapper — monitor IPC fd writes */
typedef ssize_t (*real_write_fn)(int, const void *, size_t);
static real_write_fn real_write_ptr = NULL;

ssize_t write(int fd, const void *buf, size_t count) {
    if (!real_write_ptr)
        real_write_ptr = (real_write_fn)dlsym(RTLD_NEXT, "write");

    /* Intercept the 1-byte '0' status write on IPC fd → change to '1' (ready) */
    if (ipc_fd >= 0 && fd == ipc_fd && count == 1 && buf && *(const unsigned char *)buf == '0') {
        char one = '1';
        ssize_t ret = real_write_ptr(fd, &one, 1);
        int n = __sync_fetch_and_add(&ipc_write_count, 1);
        if (n < 50) {
            debug_int("FIX: WRITE ipc_fd INTERCEPTED '0'→'1' fd=", fd);
            debug_int("  ret=", (long)ret);
        }
        return ret;
    }

    ssize_t ret = real_write_ptr(fd, buf, count);
    if (ipc_fd >= 0 && fd == ipc_fd) {
        int n = __sync_fetch_and_add(&ipc_write_count, 1);
        if (n < 50) {
            debug_int("FIX: WRITE ipc_fd=", fd);
            debug_int("  count=", (long)count);
            debug_int("  ret=", (long)ret);
            if (ret > 0 && buf) debug_hexdump("  data: ", buf, (int)ret);
        }
    }
    return ret;
}

/* writev() wrapper — monitor IPC fd vector writes */
typedef ssize_t (*real_writev_fn)(int, const struct iovec *, int);
static real_writev_fn real_writev_ptr = NULL;

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    if (!real_writev_ptr)
        real_writev_ptr = (real_writev_fn)dlsym(RTLD_NEXT, "writev");
    ssize_t ret = real_writev_ptr(fd, iov, iovcnt);
    if (ipc_fd >= 0 && fd == ipc_fd) {
        int n = __sync_fetch_and_add(&ipc_write_count, 1);
        if (n < 50) {
            debug_int("FIX: WRITEV ipc_fd=", fd);
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
    if (ipc_fd >= 0 && sockfd == ipc_fd) {
        int n = __sync_fetch_and_add(&ipc_write_count, 1);
        if (n < 50) {
            debug_int("FIX: SEND ipc_fd=", sockfd);
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
    if (ipc_fd >= 0 && sockfd == ipc_fd) {
        int n = __sync_fetch_and_add(&ipc_write_count, 1);
        if (n < 50) {
            debug_int("FIX: SENDMSG ipc_fd=", sockfd);
            debug_int("  iovlen=", (long)(msg ? msg->msg_iovlen : -1));
            debug_int("  ret=", (long)ret);
        }
    }
    return ret;
}

/* read() wrapper — monitor IPC fd reads */
typedef ssize_t (*real_read_fn)(int, void *, size_t);
static real_read_fn real_read_ptr = NULL;

ssize_t read(int fd, void *buf, size_t count) {
    if (!real_read_ptr)
        real_read_ptr = (real_read_fn)dlsym(RTLD_NEXT, "read");
    ssize_t ret = real_read_ptr(fd, buf, count);
    if (ipc_fd >= 0 && fd == ipc_fd) {
        int n = __sync_fetch_and_add(&ipc_read_count, 1);
        if (n < 50) {
            debug_int("FIX: READ ipc_fd=", fd);
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
    if (ipc_fd >= 0 && sockfd == ipc_fd) {
        int n = __sync_fetch_and_add(&ipc_read_count, 1);
        if (n < 50) {
            debug_int("FIX: RECV ipc_fd=", sockfd);
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
    if (ipc_fd >= 0 && sockfd == ipc_fd) {
        int n = __sync_fetch_and_add(&ipc_read_count, 1);
        if (n < 50) {
            debug_int("FIX: RECVMSG ipc_fd=", sockfd);
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

/* Forward declaration — defined in SHM BRIDGE section below */
static void track_shm_file(const char *name, const char *path, int fd);

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
    /* Track Shm_ files created by webhelper for bridge to 32-bit side */
    if (fd >= 0 && (oflag & O_CREAT) && strstr(name, "Shm_")) {
        track_shm_file(name, path, (int)fd);
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
        /* Report IPC fd I/O activity and accept count */
        debug_int("  ipc_fd=", (long)ipc_fd);
        debug_int("  ipc_wr=", (long)ipc_write_count);
        debug_int("  ipc_rd=", (long)ipc_read_count);
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

/* IPC "fake ready" thread: the webhelper never signals "ready" because
 * Chromium's init never completes in single-process mode (V8 proxy resolver).
 * The steam client waits ~60s on the IPC fd for a ready message then gives up.
 *
 * v103: uses auto-detected ipc_fd instead of hardcoded 11. */
static volatile int ipc_ready_sent = 0;

static void *ipc_ready_func(void *arg) {
    (void)arg;
    int my_ipc_fd = ipc_fd;
    if (my_ipc_fd < 0) {
        debug_msg("FIX: ipc_ready: no IPC fd, aborting\n");
        return NULL;
    }
    debug_int("FIX: ipc_ready: waiting 20s, ipc_fd=", my_ipc_fd);
    struct timespec ts = { .tv_sec = 20, .tv_nsec = 0 };
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret)
        : "0"((long)SYS_nanosleep), "D"(&ts), "S"((long)0)
        : "rcx", "r11", "memory");

    /* Check that IPC fd is still open */
    struct stat st;
    if (fstat(my_ipc_fd, &st) != 0) {
        debug_int("FIX: ipc_ready: fd closed, aborting fd=", my_ipc_fd);
        return NULL;
    }

    debug_int("FIX: ipc_ready: sending fake ready signal to fd=", my_ipc_fd);

    if (!real_write_ptr)
        real_write_ptr = (real_write_fn)dlsym(RTLD_NEXT, "write");

    /* First: drain pending data from the IPC fd */
    unsigned char rdbuf[256];
    for (int i = 0; i < 5; i++) {
        int flags = fcntl(my_ipc_fd, F_GETFL);
        fcntl(my_ipc_fd, F_SETFL, flags | O_NONBLOCK);
        ssize_t r = read(my_ipc_fd, rdbuf, sizeof(rdbuf));
        fcntl(my_ipc_fd, F_SETFL, flags);
        if (r > 0) {
            debug_int("FIX: ipc_ready: drained pending data, len=", (long)r);
            debug_hexdump("  data: ", rdbuf, r > 60 ? 60 : (int)r);
        } else {
            break;
        }
    }

    /* Write sdPC echo */
    unsigned char ready_msg[56];
    memset(ready_msg, 0, sizeof(ready_msg));
    ready_msg[0] = 's'; ready_msg[1] = 'd'; ready_msg[2] = 'P'; ready_msg[3] = 'C';
    ready_msg[4] = 0x02; /* version = 2 */
    ready_msg[8] = 0x01; /* type = 1 */

    ssize_t n = real_write_ptr(my_ipc_fd, ready_msg, 56);
    debug_int("FIX: ipc_ready: wrote sdPC type=1, ret=", (long)n);

    if (n == 56) {
        char one = '1';
        n = real_write_ptr(my_ipc_fd, &one, 1);
        debug_int("FIX: ipc_ready: wrote '1' status, ret=", (long)n);
    }

    ipc_ready_sent = 1;
    return NULL;
}

/* ============================================================
 * SHM BRIDGE: Forward Shm_ file data from 64-bit overlay to
 * 32-bit overlay via abstract socket.
 *
 * Problem: FEX overlay isolation — each FEX process (32-bit
 * steam client vs 64-bit webhelper) has its own overlay.
 * Files created in one overlay are invisible to the other.
 * Abstract sockets ARE shared (kernel namespace).
 *
 * Flow:
 * 1. Webhelper's chrome_ipc_server.cpp creates Shm_ file + socket
 *    (in 64-bit overlay — invisible to 32-bit)
 * 2. This thread monitors Shm_ files created via our shm_open wrapper
 * 3. Reads file content and sends over abstract socket "shm_bridge"
 * 4. 32-bit shim receives data, writes to its own Shm_ file
 * 5. Steam client reads the 32-bit file → sees real CMsgBrowserReady
 * ============================================================ */

static void int_to_str(char *buf, long val) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char digits[20];
    int n = 0;
    while (val > 0) { digits[n++] = '0' + (val % 10); val /= 10; }
    for (int i = 0; i < n; i++) buf[i] = digits[n - 1 - i];
    buf[n] = '\0';
}

/* Track Shm_ files created by the webhelper */
#define MAX_SHM_TRACKED 8
static struct {
    char name[64];   /* shm_open name (e.g. "/u1000-Shm_abc123") */
    char path[256];  /* full redirect path */
    int shm_fd;      /* fd from shm_open (for fd-sharing across threads) */
    int active;
} shm_tracked[MAX_SHM_TRACKED];
static volatile int shm_tracked_count = 0;

/* Called from shm_open when O_CREAT + name contains "Shm_".
 * Now also saves the fd for fd-sharing with the 32-bit side. */
static void track_shm_file(const char *name, const char *path, int fd) {
    int idx = __sync_fetch_and_add(&shm_tracked_count, 1);
    if (idx >= MAX_SHM_TRACKED) return;
    int i = 0;
    while (name[i] && i < 63) { shm_tracked[idx].name[i] = name[i]; i++; }
    shm_tracked[idx].name[i] = '\0';
    i = 0;
    while (path[i] && i < 255) { shm_tracked[idx].path[i] = path[i]; i++; }
    shm_tracked[idx].path[i] = '\0';
    shm_tracked[idx].shm_fd = fd;
    shm_tracked[idx].active = 1;
    debug_msg("FIX: BRIDGE: tracking Shm_ file: ");
    debug_msg(name);
    debug_int("  fd=", fd);
}

/* Bridge protocol v98 — SCM_RIGHTS fd passing:
 * FEX guests have separate fd tables (dup fails with EBADF).
 * Use sendmsg(SCM_RIGHTS) to pass the REAL fd across the abstract socket.
 * The 32-bit side receives a new fd pointing to the same open file —
 * live MAP_SHARED mapping, real-time content updates.
 *
 * For each tracked file, we send:
 *   data: name_len(2) + name (as iov)
 *   cmsg: SCM_RIGHTS with the fd
 * Terminator: name_len=0 (no cmsg) */
#define BRIDGE_SOCK_NAME "\0shm_bridge_64to32"
#define BRIDGE_SOCK_NAME_LEN 19

/* Helper: send one fd with SCM_RIGHTS along with name data */
static int send_fd_with_name(int sock, const char *name, int name_len, int fd_to_send) {
    unsigned char nbuf[66];
    nbuf[0] = (unsigned char)(name_len & 0xff);
    nbuf[1] = (unsigned char)((name_len >> 8) & 0xff);
    memcpy(nbuf + 2, name, name_len);

    struct iovec iov;
    iov.iov_base = nbuf;
    iov.iov_len = 2 + name_len;

    /* Build cmsg for SCM_RIGHTS */
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg_buf;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf.buf;
    msg.msg_controllen = sizeof(cmsg_buf.buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

    return sendmsg(sock, &msg, 0);
}

static void *shm_bridge_func(void *arg) {
    (void)arg;
    debug_msg("FIX: BRIDGE: starting server (SCM_RIGHTS mode)\n");

    int listenfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenfd < 0) {
        debug_msg("FIX: BRIDGE: socket() failed\n");
        return NULL;
    }

    struct sockaddr_un baddr;
    memset(&baddr, 0, sizeof(baddr));
    baddr.sun_family = AF_UNIX;
    memcpy(baddr.sun_path, BRIDGE_SOCK_NAME, BRIDGE_SOCK_NAME_LEN);

    if (!real_bind_ptr)
        real_bind_ptr = (real_bind_fn)dlsym(RTLD_NEXT, "bind");

    /* v103: Retry bind on EADDRINUSE (old process may still hold socket).
     * Try connect first — if old server responds, skip (it's still alive). */
    int bret = -1;
    for (int retry = 0; retry < 10; retry++) {
        bret = real_bind_ptr(listenfd, (struct sockaddr *)&baddr,
                             offsetof(struct sockaddr_un, sun_path) + BRIDGE_SOCK_NAME_LEN);
        if (bret == 0) break;
        if (errno != EADDRINUSE) break;

        /* EADDRINUSE: check if old server is alive by trying to connect */
        int probe = socket(AF_UNIX, SOCK_STREAM, 0);
        if (probe >= 0) {
            if (!real_connect_ptr)
                real_connect_ptr = (real_connect_fn)dlsym(RTLD_NEXT, "connect");
            int cret = real_connect_ptr(probe, (struct sockaddr *)&baddr,
                offsetof(struct sockaddr_un, sun_path) + BRIDGE_SOCK_NAME_LEN);
            close(probe);
            if (cret == 0) {
                /* Old server is alive and accepting — we're a duplicate, exit */
                debug_int("FIX: BRIDGE: old server alive, skipping (retry=", retry);
                close(listenfd);
                return NULL;
            }
        }
        /* Old server not responding but socket stuck — wait and retry */
        debug_int("FIX: BRIDGE: EADDRINUSE, retry #", retry + 1);
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        /* Re-create socket for retry */
        close(listenfd);
        listenfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listenfd < 0) return NULL;
    }
    if (bret < 0) {
        debug_int("FIX: BRIDGE: bind failed after retries, errno=", errno);
        close(listenfd);
        return NULL;
    }

    if (!real_listen_ptr)
        real_listen_ptr = (real_listen_fn)dlsym(RTLD_NEXT, "listen");
    real_listen_ptr(listenfd, 10);
    debug_msg("FIX: BRIDGE: server listening\n");

    int served = 0;

    while (1) {
        if (!real_accept_ptr)
            real_accept_ptr = (real_accept_fn)dlsym(RTLD_NEXT, "accept");
        int cfd = real_accept_ptr(listenfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            debug_int("FIX: BRIDGE: accept error errno=", errno);
            break;
        }

        /* Wait for tracked files if none yet */
        int count = shm_tracked_count;
        if (count == 0) {
            debug_msg("FIX: BRIDGE: waiting for tracked files...\n");
            struct timespec ts = {0, 200000000L};
            for (int w = 0; w < 75 && shm_tracked_count == 0; w++)
                nanosleep(&ts, NULL);
            count = shm_tracked_count;
            debug_int("FIX: BRIDGE: tracked_count=", count);
        }
        if (count > MAX_SHM_TRACKED) count = MAX_SHM_TRACKED;

        int sent = 0;
        for (int i = 0; i < count; i++) {
            if (!shm_tracked[i].active) continue;
            int sfd = shm_tracked[i].shm_fd;
            if (sfd < 0) continue;

            int name_len = 0;
            while (shm_tracked[i].name[name_len]) name_len++;

            /* v107: CMsgBrowserReady patching moved to shm_patcher_func thread.
             * The one-shot patch here was unreliable — webhelper's own init
             * would overwrite it. The patcher thread re-applies every 200ms. */

            int ret = send_fd_with_name(cfd, shm_tracked[i].name, name_len, sfd);
            sent++;

            if (sent <= 10) {
                debug_msg("FIX: BRIDGE: sent SCM_RIGHTS for ");
                debug_msg(shm_tracked[i].name);
                debug_int("  fd=", sfd);
                debug_int("  sendmsg ret=", ret);
            }
        }

        /* Terminator: name_len=0, no cmsg */
        {
            unsigned char zero[2] = {0, 0};
            if (!real_write_ptr)
                real_write_ptr = (real_write_fn)dlsym(RTLD_NEXT, "write");
            real_write_ptr(cfd, zero, 2);
        }

        close(cfd);
        served++;
        if (served <= 20) {
            debug_int("FIX: BRIDGE: served #", served);
            debug_int("  sent=", sent);
        }
    }

    close(listenfd);
    debug_int("FIX: BRIDGE: exiting, total=", served);
    return NULL;
}

/* ============================================================
 * SHM PATCHER v107b: Persistent thread that keeps CMsgBrowserReady
 * in Shm_ ring buffers until the 32-bit client consumes them.
 *
 * v107: Discovered the Shm_ file has TWO ring buffers:
 *   Ring A (offset 16): client→server (commands) — webhelper READS
 *   Ring B (offset 16+cap+16): server→client (responses) — client READS
 * v107 wrote to Ring A → webhelper read it as "Invalid command"!
 * v107b writes to Ring B (server→client direction).
 *
 * File layout (probed from file size):
 *   [Header A: 16 bytes][Ring A: cap bytes][Header B: 16 bytes][Ring B: cap bytes]
 *   Header: [get:4][put:4][cap:4][pending:4]
 *   Total expected: 16 + cap + 16 + cap = 32 + 2*cap
 * ============================================================ */
static void *shm_patcher_func(void *arg) {
    (void)arg;
    debug_msg("FIX: SHM_PATCHER v109: starting\n");

    /* Wait for at least one tracked Shm_ file */
    for (int w = 0; w < 300 && shm_tracked_count == 0; w++) {
        struct timespec ts = {0, 200000000L}; /* 200ms */
        nanosleep(&ts, NULL);
    }
    int count = shm_tracked_count;
    if (count <= 0) {
        debug_msg("FIX: SHM_PATCHER: no Shm_ files tracked, exiting\n");
        return NULL;
    }
    if (count > MAX_SHM_TRACKED) count = MAX_SHM_TRACKED;
    debug_int("FIX: SHM_PATCHER: tracking count=", count);

    /* mmap each tracked Shm_ file (MAP_SHARED for live updates) */
    struct {
        void *map;
        long size;
        int done;    /* 1 = consumed or failed, stop patching */
        int patches; /* number of times we patched this file */
        long ring_b_hdr_off; /* offset to Ring B header (server→client) */
        long ring_b_data_off; /* offset to Ring B data */
    } maps[MAX_SHM_TRACKED];
    memset(maps, 0, sizeof(maps));

    for (int i = 0; i < count; i++) {
        if (!shm_tracked[i].active) { maps[i].done = 1; continue; }
        int sfd = shm_tracked[i].shm_fd;
        if (sfd < 0) { maps[i].done = 1; continue; }

        struct stat bst;
        if (fstat(sfd, &bst) != 0 || bst.st_size < 16) {
            maps[i].map = NULL;
            continue;
        }
        maps[i].size = bst.st_size;
        debug_int("FIX: SHM_PATCHER: file size idx=", i);
        debug_int("  size=", bst.st_size);
        maps[i].map = mmap(NULL, bst.st_size,
            PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
        if (maps[i].map == MAP_FAILED) {
            maps[i].map = NULL;
            maps[i].done = 1;
            debug_int("FIX: SHM_PATCHER: mmap failed for idx=", i);
            continue;
        }

        /* Probe file layout: dump first 48 bytes of header */
        {
            unsigned char *raw = (unsigned char *)maps[i].map;
            int dump_len = bst.st_size < 48 ? (int)bst.st_size : 48;
            debug_msg("FIX: SHM_PATCHER: header dump: ");
            debug_hexdump("  ", raw, dump_len);
        }
    }

    /* Poll loop: wait for capacity, find Ring B, patch every 200ms */
    int all_done = 0;
    for (int cycle = 0; cycle < 600 && !all_done; cycle++) { /* 600*200ms = 120s */
        struct timespec ts = {0, 200000000L};
        nanosleep(&ts, NULL);

        all_done = 1;
        for (int i = 0; i < count; i++) {
            if (maps[i].done) continue;

            /* Lazy mmap if not done yet (file may have grown) */
            if (!maps[i].map) {
                int sfd = shm_tracked[i].shm_fd;
                struct stat bst;
                if (sfd < 0 || fstat(sfd, &bst) != 0 || bst.st_size < 16)
                    { all_done = 0; continue; }
                /* Re-mmap if file grew */
                if (bst.st_size > maps[i].size) {
                    maps[i].size = bst.st_size;
                    debug_int("FIX: SHM_PATCHER: file grew, new size=", bst.st_size);
                }
                maps[i].map = mmap(NULL, maps[i].size,
                    PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
                if (maps[i].map == MAP_FAILED) {
                    maps[i].map = NULL;
                    maps[i].done = 1;
                    continue;
                }
            }

            /* Also check for file growth (webhelper may ftruncate later) */
            {
                int sfd = shm_tracked[i].shm_fd;
                struct stat bst;
                if (sfd >= 0 && fstat(sfd, &bst) == 0 && bst.st_size > maps[i].size) {
                    munmap(maps[i].map, maps[i].size);
                    maps[i].size = bst.st_size;
                    maps[i].map = mmap(NULL, bst.st_size,
                        PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
                    if (maps[i].map == MAP_FAILED) {
                        maps[i].map = NULL;
                        maps[i].done = 1;
                        continue;
                    }
                    debug_int("FIX: SHM_PATCHER: remapped, new size=", bst.st_size);
                }
            }

            unsigned int *hdr_a = (unsigned int *)maps[i].map;
            unsigned int cap_a = hdr_a[2];

            /* Find Ring B header offset (if not yet found) */
            if (maps[i].ring_b_hdr_off == 0 && cap_a > 0) {
                /* Expected layout: [HdrA:16][RingA:cap][HdrB:16][RingB:cap] */
                long ring_b_off = 16 + (long)cap_a;
                long min_size = ring_b_off + 16; /* need at least HdrB */
                if (maps[i].size >= min_size) {
                    maps[i].ring_b_hdr_off = ring_b_off;
                    maps[i].ring_b_data_off = ring_b_off + 16;
                    debug_int("FIX: SHM_PATCHER: Ring B found at offset=", ring_b_off);
                    debug_int("  file_size=", maps[i].size);
                    /* Dump Ring B header */
                    unsigned int *hdr_b = (unsigned int *)((char *)maps[i].map + ring_b_off);
                    debug_int("  B.get=", hdr_b[0]);
                    debug_int("  B.put=", hdr_b[1]);
                    debug_int("  B.cap=", hdr_b[2]);
                    debug_int("  B.pend=", hdr_b[3]);
                } else {
                    /* File not big enough for two ring buffers — try single */
                    debug_int("FIX: SHM_PATCHER: single ring? size=", maps[i].size);
                    debug_int("  need=", min_size);
                }
            }

            /* Log header state periodically (every 5s = 25 cycles) */
            if (cycle % 25 == 0) {
                debug_int("FIX: SHM_PATCHER: [", i);
                debug_int("] A.get=", hdr_a[0]);
                debug_int("  A.put=", hdr_a[1]);
                debug_int("  A.cap=", cap_a);
                debug_int("  A.pend=", hdr_a[3]);
                if (maps[i].ring_b_hdr_off > 0) {
                    unsigned int *hdr_b = (unsigned int *)((char *)maps[i].map + maps[i].ring_b_hdr_off);
                    debug_int("  B.get=", hdr_b[0]);
                    debug_int("  B.put=", hdr_b[1]);
                    debug_int("  B.cap=", hdr_b[2]);
                    debug_int("  B.pend=", hdr_b[3]);
                }
                debug_int("  patches=", maps[i].patches);
            }

            /* Try patching Ring B (server→client) if available */
            if (maps[i].ring_b_hdr_off > 0) {
                unsigned int *hdr_b = (unsigned int *)((char *)maps[i].map + maps[i].ring_b_hdr_off);
                unsigned int b_get = hdr_b[0];
                unsigned int b_put = hdr_b[1];
                unsigned int b_cap = hdr_b[2];
                unsigned int b_pend = hdr_b[3];

                /* Client consumed message: get moved past 0 */
                if (b_get > 0) {
                    debug_int("FIX: SHM_PATCHER: CONSUMED! idx=", i);
                    debug_int("  B.get=", b_get);
                    debug_int("  after patches=", maps[i].patches);
                    maps[i].done = 1;
                    continue;
                }

                /* Ring B ready (cap set) and empty: patch it */
                if (b_cap > 0 && b_put == 0 && b_pend == 0) {
                    unsigned char *ring = (unsigned char *)maps[i].map + maps[i].ring_b_data_off;
                    /* CMsgBrowserReady: type=0, size=2, payload=08 01 */
                    unsigned int msg_type = 0;
                    unsigned int msg_size = 2;
                    memcpy(ring, &msg_type, 4);
                    memcpy(ring + 4, &msg_size, 4);
                    ring[8] = 0x08;  /* protobuf field 1, varint */
                    ring[9] = 0x01;  /* browser_handle = 1 */
                    __sync_synchronize();
                    hdr_b[1] = 10;   /* put = 10 */
                    hdr_b[3] = 10;   /* pending = 10 */
                    __sync_synchronize();
                    msync((char *)maps[i].map + maps[i].ring_b_hdr_off, 32, MS_SYNC);
                    maps[i].patches++;
                    if (maps[i].patches <= 10 || (maps[i].patches % 50 == 0)) {
                        debug_int("FIX: SHM_PATCHER: patched Ring B idx=", i);
                        debug_int("  patch#=", maps[i].patches);
                        debug_int("  cycle=", cycle);
                    }
                }
            } else if (cap_a == 0) {
                /* Ring A not initialized yet — keep waiting */
            } else {
                /* v109: Ring A IS the server→client channel for Shm_99462695.
                 * The webhelper creates this file. The 32-bit client reads it.
                 * Wait 15s for Chromium to write CMsgBrowserReady natively,
                 * then inject it ourselves if the ring is still empty. */
                unsigned int a_get = hdr_a[0];
                unsigned int a_put = hdr_a[1];
                unsigned int a_pend = hdr_a[3];

                /* Client consumed: get moved past 0 */
                if (a_get > 0) {
                    debug_int("FIX: SHM_PATCHER: Ring A CONSUMED! idx=", i);
                    debug_int("  A.get=", a_get);
                    debug_int("  patches=", maps[i].patches);
                    maps[i].done = 1;
                    continue;
                }

                /* Wait 75 cycles (15s) for Chromium to write natively */
                if (cycle < 75) {
                    if (cycle % 25 == 0) {
                        debug_int("FIX: SHM_PATCHER: waiting for native write, cycle=", cycle);
                    }
                } else if (a_put == 0 && a_pend == 0 && cap_a > 0) {
                    /* Ring A still empty after 15s — inject CMsgBrowserReady */
                    unsigned char *ring = (unsigned char *)maps[i].map + 16;
                    unsigned int msg_type = 0;
                    unsigned int msg_size = 2;
                    memcpy(ring, &msg_type, 4);
                    memcpy(ring + 4, &msg_size, 4);
                    ring[8] = 0x08;  /* protobuf field 1, varint */
                    ring[9] = 0x01;  /* browser_handle = 1 */
                    __sync_synchronize();
                    hdr_a[1] = 10;   /* put = 10 */
                    hdr_a[3] = 10;   /* pending = 10 */
                    __sync_synchronize();
                    msync(maps[i].map, 32, MS_SYNC);
                    maps[i].patches++;
                    if (maps[i].patches <= 5 || (maps[i].patches % 50 == 0)) {
                        debug_int("FIX: SHM_PATCHER: patched Ring A idx=", i);
                        debug_int("  patch#=", maps[i].patches);
                        debug_int("  cycle=", cycle);
                    }
                }
            }

            all_done = 0;
        }
    }

    /* Cleanup mmaps */
    for (int i = 0; i < count; i++) {
        if (maps[i].map && maps[i].map != MAP_FAILED) {
            unsigned int *hdr = (unsigned int *)maps[i].map;
            debug_int("FIX: SHM_PATCHER: final idx=", i);
            debug_int("  A.get=", hdr[0]);
            debug_int("  A.put=", hdr[1]);
            if (maps[i].ring_b_hdr_off > 0) {
                unsigned int *hdr_b = (unsigned int *)((char *)maps[i].map + maps[i].ring_b_hdr_off);
                debug_int("  B.get=", hdr_b[0]);
                debug_int("  B.put=", hdr_b[1]);
            }
            debug_int("  total_patches=", maps[i].patches);
            munmap(maps[i].map, maps[i].size);
        }
    }

    debug_msg("FIX: SHM_PATCHER: exiting\n");
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

    /* v103: Auto-detect IPC fd from cmdline */
    detect_ipc_fd();

    /* Log IPC fd status and all open sockets/pipes */
    {
        struct stat st;
        if (ipc_fd >= 0 && fstat(ipc_fd, &st) == 0) {
            debug_int("FIX: IPC fd OPEN: ", ipc_fd);
            if (S_ISSOCK(st.st_mode)) debug_msg("  type=socket\n");
            else if (S_ISFIFO(st.st_mode)) debug_msg("  type=pipe\n");
            else debug_int("  type=", st.st_mode & S_IFMT);
        } else if (ipc_fd >= 0) {
            debug_int("FIX: IPC fd CLOSED! fd=", ipc_fd);
        } else {
            debug_msg("FIX: no IPC fd detected (not webhelper child)\n");
        }
        /* Also check FDs 3-20 for any open sockets/pipes */
        for (int fd = 3; fd <= 20; fd++) {
            if (fstat(fd, &st) == 0 && (S_ISSOCK(st.st_mode) || S_ISFIFO(st.st_mode))) {
                debug_int("FIX: FD open: fd=", fd);
            }
        }
    }

    /* Start heartbeat thread */
    {
        pthread_t hb_thread;
        pthread_create(&hb_thread, NULL, heartbeat_func, NULL);
        pthread_detach(hb_thread);
    }

    /* Start IPC fake ready thread — only if IPC fd is valid and open */
    if (ipc_fd >= 0) {
        struct stat st;
        if (fstat(ipc_fd, &st) == 0 && S_ISSOCK(st.st_mode)) {
            pthread_t ready_thread;
            pthread_create(&ready_thread, NULL, ipc_ready_func, NULL);
            pthread_detach(ready_thread);
            debug_int("FIX: started ipc_ready thread, fd=", ipc_fd);
        }
    }

    /* Start SHM bridge thread */
    {
        pthread_t bridge_thread;
        pthread_create(&bridge_thread, NULL, shm_bridge_func, NULL);
        pthread_detach(bridge_thread);
        debug_msg("FIX: started shm_bridge thread\n");
    }

    /* v107: Start persistent SHM patcher thread */
    {
        pthread_t patcher_thread;
        pthread_create(&patcher_thread, NULL, shm_patcher_func, NULL);
        pthread_detach(patcher_thread);
        debug_msg("FIX: started shm_patcher thread\n");
    }

    debug_msg("FIX-v109: + Ring A CMsgBrowserReady patcher\n");
}
