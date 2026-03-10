/* v94: + /dev/shm/ open() redirect + sem_open/sem_unlink wrappers.
 *       steamclient.so IPC needs cross-process shared memory (SysMgrMutex).
 *       FEX overlay isolation breaks this — redirect all /dev/shm/ paths
 *       and semaphores to SHM_REDIR_DIR (shared Android directory).
 *
 * v93: v91b + fake SHMemStream (creates shared memory + socket for steam IPC).
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
    /* Strip O_TRUNC for MasterStream — S32 creates the file,
     * WH re-opens it and O_TRUNC would zero the header. */
    if (pathname && strstr(pathname, "MasterStream"))
        flags &= ~O_TRUNC;
    /* v94: Redirect /dev/shm/ opens to shared Android path.
     * steamclient.so (loaded via dlopen) uses open() for IPC shared memory
     * and mutexes (SysMgrMutex). FEX overlay isolation means each process
     * has its own /dev/shm/. Redirect so both S32 and WH see same files. */
    if (pathname && strncmp(pathname, "/dev/shm/", 9) == 0) {
        char redir[256];
        const char *base = "/data/data/com.mediatek.steamlauncher/cache/s/shm/";
        const char *name = pathname + 9; /* skip "/dev/shm/" */
        int n = 0;
        const char *s = base;
        while (*s && n < 254) redir[n++] = *s++;
        s = name;
        while (*s && n < 254) redir[n++] = *s++;
        redir[n] = '\0';
        static volatile int devshm_open_count = 0;
        int cnt = __sync_fetch_and_add(&devshm_open_count, 1);
        if (cnt < 30) {
            debug_msg("FIX: open /dev/shm redirect: ");
            debug_msg(pathname);
            debug_msg(" → ");
            debug_msg(redir);
            debug_int("\n  flags=", flags);
        }
        if (flags & O_CREAT) {
            if (!mode) mode = 0666;
        }
        return real_open_ptr(redir, flags, mode);
    }
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

/* dlmopen wrapper: redirect dlmopen(LM_ID_NEWLM, ...) to dlopen(...).
 * steamclient.so is loaded via dlmopen which creates a new linker namespace.
 * In that namespace, our LD_PRELOAD overrides (connect, bind, shm_open, etc.)
 * do NOT apply. steamclient.so then fails to connect to S32 via Unix sockets
 * because the abstract→filesystem redirect is missing.
 * Fix: intercept dlmopen and use dlopen instead, keeping everything in the
 * same namespace so our socket redirects work. */
void *dlmopen(long lmid, const char *filename, int flags) {
    debug_msg("FIX: dlmopen intercepted: ");
    if (filename) debug_msg(filename);
    debug_msg("\n");
    debug_int("FIX: dlmopen lmid=", lmid);
    debug_int("FIX: dlmopen flags=", flags);

    /* Use dlopen instead — same namespace, our overrides apply */
    void *handle = dlopen(filename, flags);
    if (handle) {
        debug_msg("FIX: dlmopen→dlopen OK: ");
        if (filename) debug_msg(filename);
        debug_msg("\n");
    } else {
        debug_msg("FIX: dlmopen→dlopen FAILED: ");
        const char *err = dlerror();
        if (err) debug_msg(err);
        debug_msg("\n");
    }
    return handle;
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
static int ipc_socket_fd = -1; /* v135: detected from -child-update-ui-socket cmdline arg */

/* write() wrapper — monitor IPC FD writes */
typedef ssize_t (*real_write_fn)(int, const void *, size_t);
static real_write_fn real_write_ptr = NULL;

ssize_t write(int fd, const void *buf, size_t count) {
    if (!real_write_ptr)
        real_write_ptr = (real_write_fn)dlsym(RTLD_NEXT, "write");

    /* v135: Intercept the 1-byte '0' status write on IPC FD → change to '1' (ready) */
    if (ipc_socket_fd > 0 && fd == ipc_socket_fd && count == 1 && buf && *(const unsigned char *)buf == '0') {
        char one = '1';
        ssize_t ret = real_write_ptr(fd, &one, 1);
        int n = __sync_fetch_and_add(&fd11_write_count, 1);
        if (n < 50) {
            debug_int("FIX: WRITE IPC INTERCEPTED '0'→'1' fd=", fd);
            debug_int("  ret=", (long)ret);
        }
        return ret;
    }

    ssize_t ret = real_write_ptr(fd, buf, count);
    if (ipc_socket_fd > 0 && fd == ipc_socket_fd) {
        int n = __sync_fetch_and_add(&fd11_write_count, 1);
        if (n < 50) {
            debug_int("FIX: WRITE ipc fd=", fd);
            debug_int("  count=", (long)count);
            debug_int("  ret=", (long)ret);
            if (ret > 0 && buf) debug_hexdump("  data: ", buf, (int)ret);
        }
    }
    return ret;
}

/* writev() wrapper — monitor IPC FD vector writes */
typedef ssize_t (*real_writev_fn)(int, const struct iovec *, int);
static real_writev_fn real_writev_ptr = NULL;

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    if (!real_writev_ptr)
        real_writev_ptr = (real_writev_fn)dlsym(RTLD_NEXT, "writev");
    ssize_t ret = real_writev_ptr(fd, iov, iovcnt);
    if (ipc_socket_fd > 0 && fd == ipc_socket_fd) {
        int n = __sync_fetch_and_add(&fd11_write_count, 1);
        if (n < 50) {
            debug_int("FIX: WRITEV ipc fd=", fd);
            debug_int("  iovcnt=", (long)iovcnt);
            debug_int("  ret=", (long)ret);
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
    if (ipc_socket_fd > 0 && sockfd == ipc_socket_fd) {
        int n = __sync_fetch_and_add(&fd11_write_count, 1);
        if (n < 50) {
            debug_int("FIX: SEND ipc fd=", sockfd);
            debug_int("  len=", (long)len);
            debug_int("  ret=", (long)ret);
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
    if (ipc_socket_fd > 0 && sockfd == ipc_socket_fd) {
        int n = __sync_fetch_and_add(&fd11_write_count, 1);
        if (n < 50) {
            debug_int("FIX: SENDMSG ipc fd=", sockfd);
            debug_int("  iovlen=", (long)(msg ? msg->msg_iovlen : -1));
            debug_int("  ret=", (long)ret);
        }
    }
    return ret;
}

/* read() wrapper — monitor IPC FD reads */
typedef ssize_t (*real_read_fn)(int, void *, size_t);
static real_read_fn real_read_ptr = NULL;

ssize_t read(int fd, void *buf, size_t count) {
    if (!real_read_ptr)
        real_read_ptr = (real_read_fn)dlsym(RTLD_NEXT, "read");
    ssize_t ret = real_read_ptr(fd, buf, count);
    if (ipc_socket_fd > 0 && fd == ipc_socket_fd) {
        int n = __sync_fetch_and_add(&fd11_read_count, 1);
        if (n < 50) {
            debug_int("FIX: READ ipc fd=", fd);
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
    if (ipc_socket_fd > 0 && sockfd == ipc_socket_fd) {
        int n = __sync_fetch_and_add(&fd11_read_count, 1);
        if (n < 50) {
            debug_int("FIX: RECV ipc fd=", sockfd);
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
    if (ipc_socket_fd > 0 && sockfd == ipc_socket_fd) {
        int n = __sync_fetch_and_add(&fd11_read_count, 1);
        if (n < 50) {
            debug_int("FIX: RECVMSG ipc fd=", sockfd);
            debug_int("  ret=", (long)ret);
        }
    }
    return ret;
}

/* v136: CEF CreateBrowser interceptor — async→sync fallback for single-process mode.
 * In single-process mode, cef_browser_host_create_browser (async) fails for the
 * second browser window. Try cef_browser_host_create_browser_sync as fallback. */
typedef struct _cef_window_info_t cef_window_info_t;
typedef struct _cef_client_t cef_client_t;
typedef struct _cef_string_t cef_string_t;
typedef struct _cef_browser_settings_t cef_browser_settings_t;
typedef struct _cef_dictionary_value_t cef_dictionary_value_t;
typedef struct _cef_request_context_t cef_request_context_t;
typedef struct _cef_browser_t cef_browser_t;

typedef int (*real_cef_create_browser_fn)(const cef_window_info_t*,
    cef_client_t*, const cef_string_t*, const cef_browser_settings_t*,
    cef_dictionary_value_t*, cef_request_context_t*);
typedef cef_browser_t* (*real_cef_create_browser_sync_fn)(const cef_window_info_t*,
    cef_client_t*, const cef_string_t*, const cef_browser_settings_t*,
    cef_dictionary_value_t*, cef_request_context_t*);

static real_cef_create_browser_fn real_cef_create_browser_ptr = NULL;
static real_cef_create_browser_sync_fn real_cef_create_browser_sync_ptr = NULL;
static volatile int cef_create_count = 0;

int cef_browser_host_create_browser(const cef_window_info_t *windowInfo,
    cef_client_t *client, const cef_string_t *url,
    const cef_browser_settings_t *settings,
    cef_dictionary_value_t *extra_info,
    cef_request_context_t *request_context) {

    if (!real_cef_create_browser_ptr)
        real_cef_create_browser_ptr = (real_cef_create_browser_fn)dlsym(RTLD_NEXT,
            "cef_browser_host_create_browser");
    if (!real_cef_create_browser_sync_ptr)
        real_cef_create_browser_sync_ptr = (real_cef_create_browser_sync_fn)dlsym(RTLD_NEXT,
            "cef_browser_host_create_browser_sync");

    int n = __sync_fetch_and_add(&cef_create_count, 1);
    debug_int("FIX-v136: cef_browser_host_create_browser called #", (long)(n+1));

    /* Try async first */
    int ret = 0;
    if (real_cef_create_browser_ptr) {
        ret = real_cef_create_browser_ptr(windowInfo, client, url, settings,
            extra_info, request_context);
        debug_int("FIX-v136: async CreateBrowser returned: ", (long)ret);
    }

    /* If async fails and sync is available, try sync as fallback */
    if (ret == 0 && real_cef_create_browser_sync_ptr) {
        debug_msg("FIX-v136: async failed, trying sync CreateBrowserSync...\n");
        cef_browser_t *browser = real_cef_create_browser_sync_ptr(windowInfo,
            client, url, settings, extra_info, request_context);
        if (browser) {
            debug_msg("FIX-v136: sync CreateBrowserSync SUCCEEDED!\n");
            ret = 1;
        } else {
            debug_msg("FIX-v136: sync CreateBrowserSync also FAILED\n");
        }
    }

    return ret;
}

/* v135: XCreateWindow wrapper — force minimum 1x1 size to prevent CEF rejection */
typedef unsigned long Window;
typedef unsigned long (*real_XCreateWindow_fn)(void *, Window, int, int,
    unsigned int, unsigned int, unsigned int, int, unsigned int,
    void *, unsigned long, void *);
static real_XCreateWindow_fn real_XCreateWindow_ptr = NULL;

unsigned long XCreateWindow(void *display, Window parent, int x, int y,
    unsigned int width, unsigned int height, unsigned int border_width,
    int depth, unsigned int cls, void *visual, unsigned long valuemask, void *attributes) {
    if (!real_XCreateWindow_ptr)
        real_XCreateWindow_ptr = (real_XCreateWindow_fn)dlsym(RTLD_NEXT, "XCreateWindow");
    /* Force minimum size — CEF rejects 0x0 windows */
    if (width == 0) width = 1920;
    if (height == 0) height = 1080;
    unsigned long win = real_XCreateWindow_ptr(display, parent, x, y, width, height,
        border_width, depth, cls, visual, valuemask, attributes);
    debug_int("FIX: XCreateWindow w=", (long)width);
    debug_int("  h=", (long)height);
    debug_int("  win=", (long)win);
    return win;
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
        real_flags = (oflag & ~(O_EXCL | O_TRUNC)) | O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW;
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

/* v94: sem_open/sem_unlink wrappers — redirect POSIX named semaphores to SHM_REDIR_DIR.
 * On Linux, sem_open("/name") creates /dev/shm/sem.name. FEX overlay isolation
 * makes these invisible across processes. Redirect to shared Android path.
 * Steam's IPC uses named semaphores for SysMgrMutex cross-process synchronization. */
#include <semaphore.h>
typedef sem_t *(*real_sem_open_fn)(const char *, int, ...);
static real_sem_open_fn real_sem_open_ptr = NULL;
typedef int (*real_sem_unlink_fn)(const char *);
static real_sem_unlink_fn real_sem_unlink_ptr = NULL;

sem_t *sem_open(const char *name, int oflag, ...) {
    if (!real_sem_open_ptr)
        real_sem_open_ptr = (real_sem_open_fn)dlsym(RTLD_NEXT, "sem_open");
    static volatile int sem_count = 0;
    int cnt = __sync_fetch_and_add(&sem_count, 1);
    if (cnt < 20) {
        debug_msg("FIX: sem_open(");
        debug_msg(name ? name : "NULL");
        debug_int(") oflag=", oflag);
    }
    /* Pass through — glibc sem_open creates files in /dev/shm/sem.NAME.
     * Our open() wrapper above redirects /dev/shm/ to SHM_REDIR_DIR,
     * so the semaphore file will end up in the shared location. */
    if (oflag & O_CREAT) {
        va_list ap;
        va_start(ap, oflag);
        mode_t mode = va_arg(ap, mode_t);
        unsigned int value = va_arg(ap, unsigned int);
        va_end(ap);
        return real_sem_open_ptr(name, oflag, mode, value);
    }
    return real_sem_open_ptr(name, oflag);
}

int sem_unlink(const char *name) {
    if (!real_sem_unlink_ptr)
        real_sem_unlink_ptr = (real_sem_unlink_fn)dlsym(RTLD_NEXT, "sem_unlink");
    debug_msg("FIX: sem_unlink(");
    debug_msg(name ? name : "NULL");
    debug_msg(")\n");
    return real_sem_unlink_ptr(name);
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

static void detect_ipc_fd(void) {
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) { ipc_socket_fd = 11; return; }
    char buf[8192];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) { ipc_socket_fd = 11; return; }
    buf[n] = '\0';
    for (ssize_t i = 0; i < n; ) {
        const char *arg2 = buf + i;
        ssize_t arglen = strlen(arg2);
        if (strcmp(arg2, "-child-update-ui-socket") == 0) {
            i += arglen + 1;
            if (i < n) {
                /* manual atoi */
                { int val = 0; const char *p = buf + i;
                  while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
                  ipc_socket_fd = val; }
                debug_int("FIX-v135: detected IPC socket FD=", ipc_socket_fd);
            }
            return;
        }
        i += arglen + 1;
    }
    debug_msg("FIX-v135: -child-update-ui-socket not in cmdline, scanning FDs\n");
    struct stat st;
    for (int testfd = 3; testfd <= 20; testfd++) {
        if (fstat(testfd, &st) == 0 && S_ISSOCK(st.st_mode)) {
            ipc_socket_fd = testfd;
            debug_int("FIX-v135: found socket at FD=", testfd);
            return;
        }
    }
    ipc_socket_fd = 11;
}

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

    /* v135: Check that IPC FD is still open */
    struct stat st;
    if (fstat(ipc_socket_fd, &st) != 0) {
        debug_int("FIX: fd_ready: IPC FD closed, fd=", ipc_socket_fd);
        return NULL;
    }

    debug_int("FIX: fd_ready: sending fake ready signal to FD=", ipc_socket_fd);

    if (!real_write_ptr)
        real_write_ptr = (real_write_fn)dlsym(RTLD_NEXT, "write");

    /* First: read any pending data on IPC FD */
    unsigned char rdbuf[256];
    for (int i = 0; i < 5; i++) {
        int flags = fcntl(ipc_socket_fd, F_GETFL);
        fcntl(ipc_socket_fd, F_SETFL, flags | O_NONBLOCK);
        ssize_t r = read(ipc_socket_fd, rdbuf, sizeof(rdbuf));
        fcntl(ipc_socket_fd, F_SETFL, flags);
        if (r > 0) {
            debug_int("FIX: fd_ready: drained pending data, len=", (long)r);
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

    ssize_t n = real_write_ptr(ipc_socket_fd, ready_msg, 56);
    debug_int("FIX: fd_ready: wrote sdPC type=1, ret=", (long)n);

    if (n == 56) {
        char one = '1';
        n = real_write_ptr(ipc_socket_fd, &one, 1);
        debug_int("FIX: fd_ready: wrote '1' status, ret=", (long)n);
    }

    fd11_ready_sent = 1;
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
/* v95: Include PID in bridge socket name to avoid EADDRINUSE from
 * previous webhelper processes that are still alive (parked in crash
 * handler's infinite sleep). Abstract sockets auto-cleanup on process
 * exit but parked processes never exit. */
static char bridge_sock_name[64];
static int bridge_sock_name_len = 0;

static void init_bridge_sock_name(void) {
    if (bridge_sock_name_len > 0) return;
    /* Format: \0shm_bridge_64to32_<PID> */
    bridge_sock_name[0] = '\0'; /* abstract socket prefix */
    const char *prefix = "shm_bridge_64to32_";
    int n = 1;
    while (*prefix) bridge_sock_name[n++] = *prefix++;
    /* Append PID */
    long pid = syscall(SYS_getpid);
    char digits[16]; int dn = 0;
    if (pid == 0) digits[dn++] = '0';
    else { while (pid > 0) { digits[dn++] = '0' + (pid % 10); pid /= 10; } }
    for (int i = dn - 1; i >= 0; i--) bridge_sock_name[n++] = digits[i];
    bridge_sock_name_len = n;
}

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

    init_bridge_sock_name();
    struct sockaddr_un baddr;
    memset(&baddr, 0, sizeof(baddr));
    baddr.sun_family = AF_UNIX;
    memcpy(baddr.sun_path, bridge_sock_name, bridge_sock_name_len);

    if (!real_bind_ptr)
        real_bind_ptr = (real_bind_fn)dlsym(RTLD_NEXT, "bind");
    int bret = real_bind_ptr(listenfd, (struct sockaddr *)&baddr,
                             offsetof(struct sockaddr_un, sun_path) + bridge_sock_name_len);
    if (bret < 0) {
        debug_int("FIX: BRIDGE: bind failed errno=", errno);
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

            /* v102: Patch Shm_ header via mmap BEFORE sending fd.
             * CORRECT SHMemStream header (from local Steam trace):
             *   hdr[0] = get  (read cursor, 0-based into ring buffer)
             *   hdr[1] = put  (write cursor, 0-based into ring buffer)
             *   hdr[2] = capacity (ring buffer size)
             *   hdr[3] = pending (bytes available = put - get)
             * Ring buffer data starts at file offset 16. */
            {
                struct stat bst;
                if (fstat(sfd, &bst) == 0 && bst.st_size >= 16) {
                    void *bmap = mmap(NULL, bst.st_size,
                        PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
                    if (bmap != MAP_FAILED) {
                        unsigned int *bhdr = (unsigned int *)bmap;
                        if (bhdr[2] == 8192) {
                            /* v130: Write REAL CMsgBrowserReady to ring buffer.
                             * From PC trace, the message is 76 bytes:
                             *   u32 field1 = 1  (version/type)
                             *   u32 field2 = 1  (browser_handle)
                             *   u32 pid          (S32 PID)
                             *   char[64] stream_name (NUL-padded)
                             * Header format: {get, put, m_cubBuffer, pending} */
                            unsigned char *ring = (unsigned char *)bmap + 16;
                            memset(ring, 0, 76);

                            /* Get S32 PID — walk up ppid chain past shell wrapper */
                            unsigned int s32_pid = (unsigned int)getppid();
                            {
                                /* ppid might be steamwebhelper.sh, go one more level */
                                char ppid_path[64];
                                int k = 0;
                                const char *pp = "/proc/";
                                while (*pp) ppid_path[k++] = *pp++;
                                /* write ppid digits */
                                char pd[16]; int pn = 0;
                                unsigned int v = s32_pid;
                                if (v == 0) { pd[pn++] = '0'; }
                                else { while (v > 0) { pd[pn++] = '0' + (v % 10); v /= 10; } }
                                for (int i = pn - 1; i >= 0; i--) ppid_path[k++] = pd[i];
                                const char *st = "/status";
                                while (*st) ppid_path[k++] = *st++;
                                ppid_path[k] = '\0';
                                int sfd2 = open(ppid_path, O_RDONLY);
                                if (sfd2 >= 0) {
                                    char sbuf[512];
                                    int rd = read(sfd2, sbuf, sizeof(sbuf) - 1);
                                    close(sfd2);
                                    if (rd > 0) {
                                        sbuf[rd] = '\0';
                                        /* Find "PPid:\t" line */
                                        const char *needle = "PPid:\t";
                                        char *found = strstr(sbuf, needle);
                                        if (found) {
                                            found += 6; /* skip "PPid:\t" */
                                            unsigned int gpp = 0;
                                            while (*found >= '0' && *found <= '9') {
                                                gpp = gpp * 10 + (*found - '0');
                                                found++;
                                            }
                                            if (gpp > 1) s32_pid = gpp;
                                        }
                                    }
                                }
                            }

                            unsigned int f1 = 1, f2 = 1;
                            memcpy(ring + 0, &f1, 4);
                            memcpy(ring + 4, &f2, 4);
                            memcpy(ring + 8, &s32_pid, 4);

                            /* Build stream name: "SteamChrome_MasterStream_PID_PID" */
                            char sname[64];
                            memset(sname, 0, 64);
                            {
                                int n = 0;
                                const char *pfx = "SteamChrome_MasterStream_";
                                while (*pfx && n < 60) sname[n++] = *pfx++;
                                /* Append S32 PID */
                                char pd2[16]; int pn2 = 0;
                                unsigned int vv = s32_pid;
                                if (vv == 0) { pd2[pn2++] = '0'; }
                                else { while (vv > 0) { pd2[pn2++] = '0' + (vv % 10); vv /= 10; } }
                                for (int i = pn2 - 1; i >= 0 && n < 60; i--) sname[n++] = pd2[i];
                                sname[n++] = '_';
                                /* Append webhelper PID as suffix */
                                unsigned int wpid = (unsigned int)getpid();
                                pn2 = 0;
                                vv = wpid;
                                if (vv == 0) { pd2[pn2++] = '0'; }
                                else { while (vv > 0) { pd2[pn2++] = '0' + (vv % 10); vv /= 10; } }
                                for (int i = pn2 - 1; i >= 0 && n < 63; i--) sname[n++] = pd2[i];
                            }
                            memcpy(ring + 12, sname, 64);

                            /* Header: {get=0, put=76, m_cubBuffer=8192, pending=76} */
                            bhdr[0] = 0;    /* get = 0 */
                            bhdr[1] = 76;   /* put = 76 */
                            /* bhdr[2] already = 8192 (m_cubBuffer) */
                            bhdr[3] = 76;   /* pending = 76 */
                            __sync_synchronize();
                            msync(bmap, 96, MS_SYNC);
                            debug_msg("FIX: BRIDGE: patched CMsgBrowserReady (76 bytes, PC format)\n");
                            debug_int("  s32_pid=", (long)s32_pid);
                        }
                        munmap(bmap, bst.st_size);
                    }
                }
            }

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

/* v135: Detect if we're a Chromium subprocess (--type=...) */
static int is_chromium_subprocess(void) {
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return 0;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    for (ssize_t i = 0; i < n; ) {
        const char *arg = buf + i;
        if (strncmp(arg, "--type=", 7) == 0) return 1;
        i += strlen(arg) + 1;
    }
    return 0;
}

__attribute__((constructor))
static void install_fixes(void) {
    real_sigaction_ptr = (real_sigaction_fn)dlsym(RTLD_NEXT, "sigaction");
    real_syscall_ptr = (real_syscall_fn)dlsym(RTLD_NEXT, "syscall");
    if (!real_sigaction_ptr) {
        debug_msg("FIX: WARNING - could not resolve real sigaction!\n");
        return;
    }

    /* v135: Skip most initialization for Chromium subprocesses */
    if (is_chromium_subprocess()) {
        debug_msg("FIX-v135: Chromium subprocess detected, minimal init\n");
        /* Just set up basic shm redirect, no crash handlers or IPC */
        mkdir(SHM_REDIR_DIR, 0777);
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

    /* v135: Detect IPC socket FD from -child-update-ui-socket cmdline arg */
    detect_ipc_fd();
    {
        struct stat st;
        if (fstat(ipc_socket_fd, &st) == 0) {
            debug_int("FIX: IPC FD OPEN, fd=", ipc_socket_fd);
            if (S_ISSOCK(st.st_mode)) debug_msg("  type=socket\n");
            else if (S_ISFIFO(st.st_mode)) debug_msg("  type=pipe\n");
            else debug_int("  type=other mode=", st.st_mode & S_IFMT);
        } else {
            debug_int("FIX: IPC FD CLOSED! fd=", ipc_socket_fd);
        }
        /* Also log FDs 3-15 for debugging */
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

    /* v135: Start IPC ready thread — only if the IPC FD is open */
    {
        struct stat st;
        if (ipc_socket_fd > 0 && fstat(ipc_socket_fd, &st) == 0 && S_ISSOCK(st.st_mode)) {
            pthread_t ready_thread;
            pthread_create(&ready_thread, NULL, fd11_ready_func, NULL);
            pthread_detach(ready_thread);
            debug_int("FIX: started IPC ready thread for FD=", ipc_socket_fd);
        }
    }

    /* Start SHM bridge thread — forwards Shm_ file data from 64-bit
     * overlay to 32-bit side via abstract socket */
    {
        pthread_t bridge_thread;
        pthread_create(&bridge_thread, NULL, shm_bridge_func, NULL);
        pthread_detach(bridge_thread);
        debug_msg("FIX: started shm_bridge thread\n");
    }

    debug_msg("FIX-v131: + dlmopen→dlopen redirect for steamclient.so\n");
}
