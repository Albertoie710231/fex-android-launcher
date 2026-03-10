/* v134: 32-bit shim for the steam client binary.
 * - v133: FIX WebUITransport PID check ("Checked: 0/PID").
 *         S32 uses SO_PEERCRED or /proc/net/tcp to find the PID of the
 *         connecting WebSocket client. On Android, /proc/net/tcp is
 *         restricted and SO_PEERCRED fails on TCP sockets → PID=0.
 *         FIX: getsockopt wrapper fakes SO_PEERCRED with webhelper PID.
 *         FIX: open() generates fake /proc/net/tcp from live socket data.
 *         FIX: readlink/opendir redirect /proc/<WH_PID>/ → /proc/self/.
 * - v132: DIAGNOSTIC for PID check — proved /proc/net/tcp is unreadable.
 * - v130: FIX CMsgBrowserReady format — was 10 bytes (type+size+protobuf),
 *         must be 76 bytes (u32(1)+u32(1)+u32(PID)+char[64](stream_name)).
 *         Also fix header order: PC trace proves {get,put,cap,pending}
 *         NOT {cap,get,put,pending}. "Invalid command" was caused by
 *         wrong message format, not wrong header order.
 * - v129: FIX MasterStream Connect() — O_RDONLY detection for hashed Shm_ names.
 *         ROOT CAUSE FOUND: Connect() calls shm_open("/u1000-Shm_HASH", O_RDONLY)
 *         where HASH is Valve's custom hash of the logical MasterStream name.
 *         Our strstr("MasterStream") check never matched the hashed name.
 *         FIX: On real PC, ONLY MasterStream uses O_RDONLY for Shm_ opens.
 *         Detect Shm_ + O_RDONLY pattern → create proper 8208-byte file
 *         (16 hdr + 8192 ring buffer) with header {get,put,m_cubBuffer=8192,
 *         pending} and CMsgBrowserReady injected in ring buffer.
 * - v128: DIAGNOSTIC — test SYS_open (syscall 5, OLD open) vs SYS_openat (295).
 *         Connect() uses raw int $0x80, bypassing ALL glibc wrappers. If it uses
 *         the OLD SYS_open(5), FEX might not do rootfs path translation for it
 *         on i386, causing ENOENT on Android (no real /dev/shm/ on device).
 * - v127: DIAGNOSTIC — test ALL i386 stat syscall variants on MasterStream fd:
 *         SYS_fstat (108), SYS_fstat64 (197), SYS_fstatat64 (300),
 *         SYS_statx (383), SYS_stat (106) by path.
 *         Connect() uses raw int $0x80 and sees st_size=0 despite file being
 *         8192 bytes. Our SYS_fstat64 (197) always returns 8192 correctly.
 *         Hypothesis: Connect() uses OLD SYS_fstat (108) with different struct
 *         layout, and FEX has a bug translating it. Dump raw bytes of ALL
 *         stat struct variants to find the discrepancy.
 * - v124: FIX root cause of assertion "8192, 0". The __fxstat wrapper caught
 *         MasterStream fds with size=0 via readlink, then called fix_ms_fd()
 *         which uses ftruncate+write — but these FAIL on read-only fds!
 *         Connect() opens O_RDONLY, so fix_ms_fd fails, re-fstat returns 0.
 *         Fix: set buf->st_size = 8192 DIRECTLY (don't re-fstat). The mmap
 *         wrapper ensures the file is physically extended before mapping.
 *         Also: strip O_TRUNC from MasterStream opens in SYS_openat handler
 *         to prevent the file from ever being truncated to 0.
 * - v123: BLOCK ftruncate on MasterStream fds to anything other than 8192.
 *         Create() likely does ftruncate(fd,0) to reset the file, which
 *         empties it. Then ftruncate(fd,8192) may fail, leaving file at 0.
 *         Fix: intercept SYS_ftruncate — if targeting a MasterStream fd,
 *         only allow size=8192. For size=0, return success without doing it.
 *         Also fix fd reuse in is_ms_fd: verify via readlink before acting.
 * - v122: FIX fstat size — file must be 8192 bytes (not 8208!). Connect()
 *         asserts fstat.st_size == 8192. Also intercept SYS_fstat64 in
 *         syscall() wrapper — Connect() uses raw syscall, bypassing __fxstat.
 *         Track MasterStream fds from raw openat. Fix on-the-fly in fstat.
 * - v121: DIAGNOSTIC — wrap glibc syscall() to catch raw SYS_openat for
 *         MasterStream. REMOVE MasterStream cleanup (v120 deleted OTHER
 *         process's active file!). Log shm_unlink for MasterStream.
 * - v120: DIAGNOSTIC — list /dev/shm/ from inside FEX to see what guest
 *         process actually sees. Clean stale MasterStream files from
 *         SHM_REDIR_DIR (were slowing cross-sync with 60+ files).
 *         Log stat dev:ino on raw_fd to detect filesystem differences.
 * - v119: CROSS-PROCESS MasterStream sync. FEX overlay isolation means each
 *         process's /dev/shm/ is independent. Process A creates MasterStream
 *         at SHM_REDIR_DIR (shared) + /dev/shm/ (overlay_A). Process B's
 *         Connect() uses raw syscall on /dev/shm/ (overlay_B) → file missing.
 *         Fix: poller thread scans SHM_REDIR_DIR for MasterStream files from
 *         OTHER processes, copies them to /dev/shm/ (our overlay) via raw
 *         syscall. Re-add mkdir /dev/shm (overlay needs the directory).
 * - v118: CRITICAL FIX — stop tagging ALL SCM_RIGHTS fds as MasterStream.
 *         fd=96 was a Shm_ IPC channel, not MasterStream. Injecting
 *         CMsgBrowserReady into it caused "Invalid command" assertions in
 *         chrome_ipc_server. Now: use readlink in mmap wrapper to verify
 *         the fd actually points to a MasterStream file before injecting.
 * - v117: fstat fix — detect MasterStream fds with size=0 via readlink,
 *         fill with CMsgBrowserReady data. Catches Connect() even when it
 *         bypasses all our shm_open/open/openat wrappers.
 * - v116: MS poller thread — continuously re-create shm via real shm_open.
 *         Add ftruncate wrapper to detect who truncates MasterStream.
 *         Diagnostic: verify shm persistence after 2s in poller.
 * - v115: Pre-create MasterStream via real shm_open (same tmpfs as Connect()).
 * - v114: Fix MasterStream: pre-create at SHM_REDIR_DIR (not /dev/shm/),
 *         remove O_TRUNC from all MasterStream handlers, separate log counter.
 * - v113: Bridge sync — copy WH Shm_ file CONTENT to S32 overlay.
 *         FEX overlay isolation: each FEX process has its own overlay.
 *         Files created by the 64-bit WH are invisible to the 32-bit client.
 *         Fix: connect to WH bridge, receive fds via SCM_RIGHTS, read
 *         content, write to S32 overlay. The client then finds files with
 *         real WH data (CMsgBrowserReady etc.) instead of empty files.
 *         Also: don't add bridge fds to bridge_dup_fds so mmap passes
 *         through to the actual file (no anonymous mapping intercept).
 * - v111: Clean stale Shm_ files + listen port files on startup.
 *         Remove broken Shm_ injection (corrupted GPUProcPIDStream).
 *         Add sendmsg wrapper for chrome_shmem protocol logging.
 *         On chrome_shmem accept, log what client sends before we respond.
 * - v108: Listen wrapper logs TCP ports. Writes port files for webhelper.
 * - v107: Fix Shm_ IPC channels — each Shm_ gets own fd (not bridge fd).
 * - v106: Pre-create MasterStream at /dev/shm/ via RAW SYSCALL.
 * - v105: openat/open/shm_open MasterStream intercepts (bypassed by glibc)
 * - v104: X11 abstract socket path rewriting (FEX /tmp != Android cache/tmp)
 * - v103: FD 11 bridge, stat/access fake for steam_chrome_shmem
 * - v102: Correct SHMemStream header (cursor-based layout)
 * - v101: mmap intercept → singleton ANON mapping
 * - v100: Bridge fd dup via SCM_RIGHTS
 */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stddef.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/types.h>
#include <poll.h>
#include <stdarg.h>

/* SYS_fstat64 may not be defined on all platforms */
#ifndef SYS_fstat64
#define SYS_fstat64 197
#endif
#ifndef SYS_ftruncate64
#define SYS_ftruncate64 194
#endif
#ifndef SYS_fstatat64
#define SYS_fstatat64 300
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>

/* v126: Raw i386 inline asm syscall helpers.
 * CSharedMemStream uses int $0x80 for ALL operations, bypassing glibc.
 * Our __fxstat wrapper always returns st_size=8192 for MasterStream fds,
 * which means ALL glibc fstat results are UNRELIABLE. These raw asm
 * helpers match EXACTLY what CSharedMemStream does. */

/* i386 syscall ABI: eax=nr, ebx=a1, ecx=a2, edx=a3, esi=a4, edi=a5 */
static long raw32_openat(int dfd, const char *path, int flags, int mode) {
    long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "0"(295/*SYS_openat*/), "b"(dfd), "c"(path), "d"(flags), "S"(mode)
        : "memory");
    return ret;
}

static long raw32_fstat64(int fd, struct stat64 *buf) {
    long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "0"(197/*SYS_fstat64*/), "b"(fd), "c"(buf)
        : "memory");
    return ret;
}

static long raw32_ftruncate64(int fd, unsigned long lo, unsigned long hi) {
    long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "0"(194/*SYS_ftruncate64*/), "b"(fd), "c"(lo), "d"(hi)
        : "memory");
    return ret;
}

static long raw32_write(int fd, const void *buf, int count) {
    long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "0"(4/*SYS_write*/), "b"(fd), "c"(buf), "d"(count)
        : "memory");
    return ret;
}

static long raw32_close(int fd) {
    long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "0"(6/*SYS_close*/), "b"(fd)
        : "memory");
    return ret;
}

static long raw32_lseek(int fd, long offset, int whence) {
    long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "0"(19/*SYS_lseek*/), "b"(fd), "c"(offset), "d"(whence)
        : "memory");
    return ret;
}

static long raw32_unlink(const char *path) {
    long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "0"(10/*SYS_unlink*/), "b"(path)
        : "memory");
    return ret;
}

/* v127: OLD SYS_fstat (108) — uses struct __old_kernel_stat with DIFFERENT layout.
 * st_size is 4 bytes at offset 20 (vs struct stat64 where it's 8 bytes at offset 44).
 * CSharedMemStream may use this instead of SYS_fstat64 (197).
 * We use a raw byte buffer to see exactly what FEX returns. */
static long raw32_fstat_old(int fd, void *buf) {
    long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "0"(108/*SYS_fstat*/), "b"(fd), "c"(buf)
        : "memory");
    return ret;
}

/* v127: SYS_stat (106) — stat by path, uses same old struct */
static long raw32_stat_old(const char *path, void *buf) {
    long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "0"(106/*SYS_stat*/), "b"(path), "c"(buf)
        : "memory");
    return ret;
}

/* v127: SYS_fstatat64 (300) — fstatat64 with AT_EMPTY_PATH for by-fd stat */
static long raw32_fstatat64(int dfd, const char *path, struct stat64 *buf, int flags) {
    long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "0"(300/*SYS_fstatat64*/), "b"(dfd), "c"(path), "d"(buf), "S"(flags)
        : "memory");
    return ret;
}

/* v127: SYS_statx (383) — newest stat variant */
struct raw_statx {
    unsigned int stx_mask;
    unsigned int stx_blksize;
    unsigned long long stx_attributes;
    unsigned int stx_nlink;
    unsigned int stx_uid;
    unsigned int stx_gid;
    unsigned short stx_mode;
    unsigned short __pad1;
    unsigned long long stx_ino;
    unsigned long long stx_size;       /* offset 40 */
    unsigned long long stx_blocks;
    unsigned long long stx_attributes_mask;
    /* ... more fields, we only care about stx_size */
    unsigned char __pad2[128];
};

static long raw32_statx(int dfd, const char *path, int flags, unsigned int mask, struct raw_statx *buf) {
    long ret;
    register long r_mask __asm__("esi") = (long)mask;
    register long r_buf __asm__("edi") = (long)buf;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "0"(383/*SYS_statx*/), "b"(dfd), "c"(path), "d"(flags), "r"(r_mask), "r"(r_buf)
        : "memory");
    return ret;
}

/* All debug output uses a single write() per message to avoid
 * FexOutput splitting lines across logcat entries */

static void debug_write(const char *buf, int len) {
    syscall(SYS_write, 2, buf, len);
}

static void debug_msg(const char *msg) {
    int len = 0;
    while (msg[len]) len++;
    debug_write(msg, len);
}

/* Write a complete line: prefix + decimal value + newline in one write */
static void debug_int(const char *prefix, long val) {
    char buf[120];
    char *p = buf;
    const char *s = prefix;
    while (*s && p < buf + 80) *p++ = *s++;
    if (val < 0) { *p++ = '-'; val = -val; }
    char digits[20];
    int n = 0;
    if (val == 0) { digits[n++] = '0'; }
    else { while (val > 0) { digits[n++] = '0' + (val % 10); val /= 10; } }
    for (int i = n - 1; i >= 0; i--) *p++ = digits[i];
    *p++ = '\n';
    debug_write(buf, p - buf);
}

/* Write: prefix + printable string + suffix in one write */
static void debug_str(const char *prefix, const char *str, const char *suffix) {
    char buf[300];
    char *p = buf;
    const char *s;
    for (s = prefix; *s && p < buf + 60; ) *p++ = *s++;
    for (s = str; *s && p < buf + 250; ) *p++ = *s++;
    for (s = suffix; *s && p < buf + 290; ) *p++ = *s++;
    debug_write(buf, p - buf);
}

/* Write: prefix + hex dump of first N bytes + newline in one write.
 * This ensures the hex data stays on the same logcat line as the prefix. */
static void debug_hexline(const char *prefix, const void *data, int datalen) {
    char buf[300];
    char *p = buf;
    const char *s = prefix;
    while (*s && p < buf + 60) *p++ = *s++;
    const unsigned char *d = (const unsigned char *)data;
    int show = datalen > 40 ? 40 : datalen;
    for (int i = 0; i < show; i++) {
        *p++ = "0123456789abcdef"[(d[i] >> 4) & 0xf];
        *p++ = "0123456789abcdef"[d[i] & 0xf];
        *p++ = ' ';
    }
    if (datalen > 40) { *p++ = '.'; *p++ = '.'; *p++ = '.'; }
    *p++ = '\n';
    debug_write(buf, p - buf);
}

/* v134: dlmopen → dlopen redirect.
 * S32 loads steamclient.so via dlmopen(LM_ID_NEWLM, ...) which creates a
 * separate linker namespace. In that namespace, our getsockopt/fopen/open
 * wrappers DON'T exist. The WebUITransport PID check runs inside
 * steamclient.so and completely bypasses all our interceptors.
 * Fix: redirect to dlopen so steamclient.so stays in our namespace.
 * Then patch the PID check in memory. */
void *dlmopen(long lmid, const char *filename, int flags) {
    debug_msg("S32: dlmopen intercepted: ");
    if (filename) debug_msg(filename);
    debug_msg("\n");

    void *handle = dlopen(filename, flags);
    if (handle) {
        debug_msg("S32: dlmopen→dlopen OK\n");
    } else {
        debug_msg("S32: dlmopen→dlopen FAILED: ");
        const char *err = dlerror();
        if (err) debug_msg(err);
        debug_msg("\n");
    }
    return handle;
}

/* v104: Real Android tmpdir extracted from FEX_OUTPUTLOG at init time.
 * Used to rewrite X11 socket paths (FEX /tmp != Android cache/tmp). */
static char real_tmpdir[256] = "";

static socklen_t make_abstract(const struct sockaddr *addr, socklen_t addrlen,
                               struct sockaddr_un *out) {
    if (!addr || addr->sa_family != AF_UNIX || addrlen <= sizeof(sa_family_t))
        return 0;
    const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
    if (un->sun_path[0] == '\0')
        return 0;

    memset(out, 0, sizeof(*out));
    out->sun_family = AF_UNIX;
    out->sun_path[0] = '\0';

    const char *src = un->sun_path;
    int len = 0;
    while (src[len] && len < 105) len++;
    memcpy(&out->sun_path[1], src, len);

    return offsetof(struct sockaddr_un, sun_path) + 1 + len;
}

typedef int (*real_bind_fn)(int, const struct sockaddr *, socklen_t);
static real_bind_fn real_bind_ptr = NULL;

static volatile int bind_count = 0;

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (!real_bind_ptr)
        real_bind_ptr = (real_bind_fn)dlsym(RTLD_NEXT, "bind");
    struct sockaddr_un abstract_addr;
    socklen_t abs_len = make_abstract(addr, addrlen, &abstract_addr);
    if (abs_len > 0) {
        const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
        int ret = real_bind_ptr(sockfd, (struct sockaddr *)&abstract_addr, abs_len);
        int n = __sync_fetch_and_add(&bind_count, 1);
        if (n < 20) {
            debug_str("S32: bind abstract: ", un->sun_path, "\n");
            debug_int("  ret=", ret);
        }
        return ret;
    }
    return real_bind_ptr(sockfd, addr, addrlen);
}

/* v108: listen() wrapper — log fd, backlog, and bound address for socket discovery.
 * For TCP sockets, writes port to a file so the webhelper can discover and connect. */
typedef int (*real_listen_fn)(int, int);
static real_listen_fn real_listen_ptr = NULL;
static volatile int listen_count = 0;

/* Directory where we store port files for the webhelper to read */
#define LISTEN_PORT_DIR "/data/data/com.mediatek.steamlauncher/cache/s/listen_ports"
static volatile int listen_port_file_count = 0;

static void save_listen_port(int port, int sockfd) {
    /* Write port to LISTEN_PORT_DIR/<idx> for webhelper discovery */
    char path[256];
    int n = 0;
    const char *d = LISTEN_PORT_DIR;
    while (*d) path[n++] = *d++;
    path[n++] = '/';
    int idx = __sync_fetch_and_add(&listen_port_file_count, 1);
    if (idx < 10) path[n++] = '0' + idx;
    else { path[n++] = '0' + (idx / 10); path[n++] = '0' + (idx % 10); }
    path[n] = '\0';

    /* Write "port fd\n" */
    char buf[32];
    int len = 0;
    /* port number */
    char digits[10];
    int nd = 0;
    int p = port;
    if (p == 0) digits[nd++] = '0';
    else while (p > 0) { digits[nd++] = '0' + (p % 10); p /= 10; }
    for (int i = nd - 1; i >= 0; i--) buf[len++] = digits[i];
    buf[len++] = ' ';
    /* fd number */
    nd = 0;
    int f = sockfd;
    if (f == 0) digits[nd++] = '0';
    else while (f > 0) { digits[nd++] = '0' + (f % 10); f /= 10; }
    for (int i = nd - 1; i >= 0; i--) buf[len++] = digits[i];
    buf[len++] = '\n';

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        write(fd, buf, len);
        close(fd);
    }
}

int listen(int sockfd, int backlog) {
    if (!real_listen_ptr)
        real_listen_ptr = (real_listen_fn)dlsym(RTLD_NEXT, "listen");
    int ret = real_listen_ptr(sockfd, backlog);
    int n = __sync_fetch_and_add(&listen_count, 1);
    if (n < 30) {
        debug_int("S32: listen fd=", sockfd);
        debug_int("  backlog=", backlog);
        debug_int("  ret=", ret);
        if (ret < 0) debug_int("  errno=", errno);
        /* Log the bound address via getsockname */
        struct sockaddr_storage ss;
        socklen_t salen = sizeof(ss);
        if (getsockname(sockfd, (struct sockaddr *)&ss, &salen) == 0) {
            if (ss.ss_family == AF_UNIX) {
                struct sockaddr_un *un = (struct sockaddr_un *)&ss;
                if (un->sun_path[0] == '\0' && salen > sizeof(sa_family_t) + 1) {
                    debug_str("  addr=@", &un->sun_path[1], "\n");
                } else if (un->sun_path[0] != '\0') {
                    debug_str("  addr=", un->sun_path, "\n");
                }
            } else if (ss.ss_family == AF_INET) {
                struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
                int port = ntohs(sin->sin_port);
                debug_int("  AF_INET port=", port);
                debug_int("  addr=", ntohl(sin->sin_addr.s_addr));
                if (ret == 0) save_listen_port(port, sockfd);
            } else {
                debug_int("  family=", ss.ss_family);
            }
        }
    }
    return ret;
}

typedef int (*real_connect_fn)(int, const struct sockaddr *, socklen_t);
static real_connect_fn real_connect_ptr = NULL;

static volatile int connect_count = 0;
static volatile int connect_abstract_count = 0;

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (!real_connect_ptr)
        real_connect_ptr = (real_connect_fn)dlsym(RTLD_NEXT, "connect");

    /* Log ALL AF_UNIX connects, including already-abstract ones */
    if (addr && addr->sa_family == AF_UNIX && addrlen > sizeof(sa_family_t)) {
        const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
        int pathlen = (int)(addrlen - offsetof(struct sockaddr_un, sun_path));

        if (un->sun_path[0] == '\0') {
            /* Already abstract — check if it's a /tmp/ path that needs rewriting */
            const char *abs_name = &un->sun_path[1];
            if (real_tmpdir[0] && strncmp(abs_name, "/tmp/", 5) == 0 &&
                !strstr(abs_name, "steam_chrome_shmem")) {
                /* Rewrite @/tmp/foo → @$real_tmpdir/foo (NOT for shmem) */
                struct sockaddr_un rewritten;
                memset(&rewritten, 0, sizeof(rewritten));
                rewritten.sun_family = AF_UNIX;
                rewritten.sun_path[0] = '\0';
                const char *suffix = abs_name + 4; /* "/foo..." */
                int rlen = 0;
                const char *s = real_tmpdir;
                while (*s && rlen < 105) rewritten.sun_path[1 + rlen++] = *s++;
                s = suffix;
                while (*s && rlen < 105) rewritten.sun_path[1 + rlen++] = *s++;
                socklen_t rw_len = offsetof(struct sockaddr_un, sun_path) + 1 + rlen;

                int ret = real_connect_ptr(sockfd, (struct sockaddr *)&rewritten, rw_len);
                int saved_errno = errno;
                int n = __sync_fetch_and_add(&connect_abstract_count, 1);
                if (n < 200) {
                    debug_str("S32: connect ABSTRACT→rewrite: ", abs_name, "\n");
                    debug_int("  fd=", sockfd);
                    debug_int("  ret=", ret);
                    if (ret < 0) debug_int("  errno=", saved_errno);
                }
                errno = saved_errno;
                return ret;
            }
            /* Non-/tmp abstract — pass through */
            int ret = real_connect_ptr(sockfd, addr, addrlen);
            int saved_errno = errno;
            int n = __sync_fetch_and_add(&connect_abstract_count, 1);
            /* v112: unconditional log for chrome_shmem; normal limit for others */
            if (strstr(abs_name, "steam_chrome_shmem") || strstr(abs_name, "chrome")) {
                debug_str("S32: connect SHMEM: @", abs_name, "\n");
                debug_int("  fd=", sockfd);
                debug_int("  ret=", ret);
                if (ret < 0) debug_int("  errno=", saved_errno);
            } else if (n < 200) {
                int namelen = pathlen > 1 ? pathlen - 1 : 0;
                if (namelen > 0 && abs_name[0] >= 0x20) {
                    debug_str("S32: connect ABSTRACT: ", abs_name, "\n");
                }
                debug_int("  fd=", sockfd);
                debug_int("  ret=", ret);
                if (ret < 0) debug_int("  errno=", saved_errno);
            }
            errno = saved_errno;
            return ret;
        }

        /* v104: Rewrite /tmp/ paths to real Android tmpdir as abstract sockets.
         * FEX guest sees /tmp but the actual sockets (X11, etc.) live at
         * $CACHE/tmp on the Android side. We rewrite to the correct abstract
         * socket name so they connect to the ARM64 X11 server.
         *
         * v105: Do NOT rewrite steam_chrome_shmem paths — the 64-bit webhelper
         * binds to @/tmp/steam_chrome_shmem_... (via its own make_abstract).
         * Rewriting to @$real_tmpdir/... would create a name mismatch. Let
         * these fall through to make_abstract() which preserves /tmp/. */
        if (real_tmpdir[0] && strncmp(un->sun_path, "/tmp/", 5) == 0 &&
            !strstr(un->sun_path, "steam_chrome_shmem")) {
            /* /tmp/foo → @$real_tmpdir/foo (abstract socket) */
            struct sockaddr_un rewritten;
            memset(&rewritten, 0, sizeof(rewritten));
            rewritten.sun_family = AF_UNIX;
            rewritten.sun_path[0] = '\0'; /* abstract */
            const char *suffix = un->sun_path + 4; /* "/foo..." after /tmp */
            int rlen = 0;
            const char *s = real_tmpdir;
            while (*s && rlen < 105) rewritten.sun_path[1 + rlen++] = *s++;
            s = suffix;
            while (*s && rlen < 105) rewritten.sun_path[1 + rlen++] = *s++;
            socklen_t abs_len = offsetof(struct sockaddr_un, sun_path) + 1 + rlen;

            int ret = real_connect_ptr(sockfd, (struct sockaddr *)&rewritten, abs_len);
            int saved_errno = errno;
            int n = __sync_fetch_and_add(&connect_count, 1);
            if (n < 50) {
                debug_str("S32: connect→rewrite: ", un->sun_path, "\n");
                debug_str("  → @", &rewritten.sun_path[1], "\n");
                debug_int("  fd=", sockfd);
                debug_int("  ret=", ret);
                if (ret < 0) debug_int("  errno=", saved_errno);
            }
            errno = saved_errno;
            return ret;
        }

        /* Steam-related paths → abstract with original name.
         * Other paths → pass through to FEX. */
        struct sockaddr_un abstract_addr;
        socklen_t abs_len = make_abstract(addr, addrlen, &abstract_addr);
        if (abs_len > 0) {
            int ret = real_connect_ptr(sockfd, (struct sockaddr *)&abstract_addr, abs_len);
            int saved_errno = errno;
            int n = __sync_fetch_and_add(&connect_count, 1);
            /* v112: unconditional log for chrome_shmem */
            if (strstr(un->sun_path, "steam_chrome_shmem") || strstr(un->sun_path, "chrome")) {
                debug_str("S32: connect SHMEM→abstract: ", un->sun_path, "\n");
                debug_int("  fd=", sockfd);
                debug_int("  ret=", ret);
                if (ret < 0) debug_int("  errno=", saved_errno);
            } else if (n < 100) {
                debug_str("S32: connect→abstract: ", un->sun_path, "\n");
                debug_int("  fd=", sockfd);
                debug_int("  ret=", ret);
                if (ret < 0) debug_int("  errno=", saved_errno);
            }
            errno = saved_errno;
            return ret;
        }
    }

    return real_connect_ptr(sockfd, addr, addrlen);
}

/* shm_open/shm_unlink + open/open64: redirect /dev/shm to real Android path.
 * Must match the 64-bit shim's redirect path so both processes
 * access the same shared memory files. */

#define SHM_REDIR_DIR "/data/data/com.mediatek.steamlauncher/cache/s/shm"
#define DEV_SHM_PREFIX "/dev/shm/"
#define DEV_SHM_PREFIX_LEN 9

static void build_shm_path(char *out, int outsz, const char *name) {
    int n = 0;
    const char *d = SHM_REDIR_DIR;
    while (*d && n < outsz - 2) out[n++] = *d++;
    if (name[0] != '/') out[n++] = '/';
    const char *s = name;
    while (*s && n < outsz - 1) out[n++] = *s++;
    out[n] = '\0';
}

/* Build redirect path for /dev/shm/NAME → SHM_REDIR_DIR/NAME */
static int build_devshm_redir(const char *pathname, char *out, int outsz) {
    if (!pathname) return 0;
    if (strncmp(pathname, DEV_SHM_PREFIX, DEV_SHM_PREFIX_LEN) != 0) return 0;
    const char *name = pathname + DEV_SHM_PREFIX_LEN;
    if (!*name) return 0;
    int n = 0;
    const char *d = SHM_REDIR_DIR;
    while (*d && n < outsz - 2) out[n++] = *d++;
    out[n++] = '/';
    while (*name && n < outsz - 1) out[n++] = *name++;
    out[n] = '\0';
    return 1;
}

typedef int (*real_open_fn)(const char *, int, ...);
static real_open_fn real_open_ptr = NULL;

static volatile int shm_open_count_32 = 0;
static volatile int shm_poll_fail_count = 0;
static volatile int shm_poll_written = 0;
static volatile int devshm_open_count = 0;
static volatile int ms_connect_detect_count = 0; /* v129: separate counter for MasterStream Connect detection */
static volatile int ms_open_count = 0; /* separate MasterStream log counter */

/* v100: Saved bridge fd from the 64-bit webhelper (via SCM_RIGHTS).
 * v101: Track dup'd fds so mmap interceptor can identify them.
 *       When steam client mmaps a bridge dup fd, return anonymous mapping
 *       with pre-filled header — bypasses FEX page cache incoherence. */
static int bridge_shm_fd = -1;
static char bridge_shm_name[64] = {0};

/* v101: Track fds returned by bridge dup path so mmap can intercept them */
#define MAX_BRIDGE_DUP_FDS 128
static int bridge_dup_fds[MAX_BRIDGE_DUP_FDS];
static volatile int bridge_dup_count = 0;

/* Track MasterStream fds to log mmap activity */
#define MAX_MS_FDS 16
static int masterstream_fds[MAX_MS_FDS];
static volatile int ms_fd_count = 0;
static void add_ms_fd(int fd) {
    int idx = __sync_fetch_and_add(&ms_fd_count, 1);
    if (idx < MAX_MS_FDS) masterstream_fds[idx] = fd;
}
static int is_ms_fd(int fd) {
    if (fd < 0) return 0;
    int n = ms_fd_count;
    for (int i = 0; i < n && i < MAX_MS_FDS; i++)
        if (masterstream_fds[i] == fd) return 1;
    return 0;
}

static void add_bridge_dup_fd(int fd) {
    int idx = __sync_fetch_and_add(&bridge_dup_count, 1);
    if (idx < MAX_BRIDGE_DUP_FDS)
        bridge_dup_fds[idx] = fd;
}

static int is_bridge_dup_fd(int fd) {
    if (fd < 0) return 0;
    if (fd == bridge_shm_fd) return 1;
    int cnt = bridge_dup_count;
    if (cnt > MAX_BRIDGE_DUP_FDS) cnt = MAX_BRIDGE_DUP_FDS;
    for (int i = 0; i < cnt; i++)
        if (bridge_dup_fds[i] == fd) return 1;
    return 0;
}


/* Read webhelper PID from /tmp/steam_webhelper_pid (written by steamwebhelper.sh v93+).
 * Returns webhelper PID, or 0 if file not found/unreadable. Non-blocking. */
static volatile unsigned int cached_wh_pid = 0;

static unsigned int read_webhelper_pid_file(void) {
    /* v133: Must use raw syscall — open() wrapper calls get_webhelper_pid()
     * which calls us → infinite recursion → stack overflow → segfault */
    int fd = (int)syscall(SYS_openat, AT_FDCWD, "/tmp/steam_webhelper_pid", O_RDONLY, 0);
    if (fd < 0) return 0;
    char pidbuf[64];
    ssize_t n = read(fd, pidbuf, sizeof(pidbuf) - 1);
    close(fd);
    if (n <= 0) return 0;
    pidbuf[n] = '\0';
    unsigned int wh_pid = (unsigned int)atoi(pidbuf);
    return wh_pid;
}

static unsigned int get_webhelper_pid(void) {
    /* Check cache first */
    unsigned int p = cached_wh_pid;
    if (p > 0) return p;
    /* Try reading file (non-blocking — file may not exist yet) */
    p = read_webhelper_pid_file();
    if (p > 0) {
        cached_wh_pid = p;
        debug_int("S32: read webhelper PID from file: ", (long)p);
    }
    return p;
}

/* v131: Write REAL CMsgBrowserReady into ring buffer (76 bytes, PC format).
 * From PC IPC trace, the ring buffer message is:
 *   u32 field1 = 1  (version/type)
 *   u32 field2 = 1  (browser_handle)
 *   u32 pid         (WEBHELPER process PID — NOT S32's PID!)
 *   char[64] stream_name ("SteamChrome_MasterStream_PID_SUFFIX", NUL-padded)
 * Total = 76 bytes.
 * Header format (from PC): {get, put, m_cubBuffer, pending} */
static void write_browserready_ring(unsigned char *buf, size_t bufsize) {
    /* buf points to the START of the file (header + ring).
     * Header is 16 bytes, ring starts at offset 16. */
    if (bufsize < 92) return; /* need 16 hdr + 76 msg */
    unsigned int *hdr = (unsigned int *)buf;
    unsigned char *ring = buf + 16;

    memset(ring, 0, 76);
    unsigned int f1 = 1, f2 = 1;
    /* v131: Use WEBHELPER's PID, not S32's. BrowserReady is a message FROM
     * the webhelper, so it must contain the webhelper's PID. S32 uses this
     * PID for SetWebUITransportWebhelperPID → lsof check. */
    unsigned int pid = get_webhelper_pid();
    if (pid == 0) {
        /* Fallback: use S32's PID (wrong but better than 0) */
        pid = (unsigned int)getpid();
        debug_int("S32: WARNING: using S32 PID as fallback: ", (long)pid);
    }
    memcpy(ring + 0, &f1, 4);
    memcpy(ring + 4, &f2, 4);
    memcpy(ring + 8, &pid, 4);

    /* Build stream name: "SteamChrome_MasterStream_PID_PID" */
    char sname[64];
    memset(sname, 0, 64);
    {
        int n = 0;
        const char *pfx = "SteamChrome_MasterStream_";
        while (*pfx && n < 60) sname[n++] = *pfx++;
        /* Append PID */
        char pd[16]; int pn = 0;
        unsigned int v = pid;
        if (v == 0) { pd[pn++] = '0'; }
        else { while (v > 0) { pd[pn++] = '0' + (v % 10); v /= 10; } }
        for (int i = pn - 1; i >= 0 && n < 60; i--) sname[n++] = pd[i];
        sname[n++] = '_';
        /* Use PID again as suffix (simple, deterministic) */
        pn = 0; v = pid;
        if (v == 0) { pd[pn++] = '0'; }
        else { while (v > 0) { pd[pn++] = '0' + (v % 10); v /= 10; } }
        for (int i = pn - 1; i >= 0 && n < 63; i--) sname[n++] = pd[i];
    }
    memcpy(ring + 12, sname, 64);

    /* Header: {get=0, put=76, m_cubBuffer=8192, pending=76} */
    hdr[0] = 0;      /* get = 0 */
    hdr[1] = 76;     /* put = 76 */
    hdr[2] = 8192;   /* m_cubBuffer */
    hdr[3] = 76;     /* pending = 76 bytes */
}

/* v116: MS poller globals — saved from constructor for background thread */
static char ms_poller_shm_name[130]; /* "/SteamChrome_MasterStream_..." for shm_open */
static char ms_poller_devshm_path[256]; /* "/dev/shm/SteamChrome_MasterStream_..." for raw syscall */
static unsigned char ms_poller_buf[8192]; /* pre-built CMsgBrowserReady content */
static volatile int ms_poller_active = 0;
static int ms_precreate_fd = -1; /* kept-open fd from constructor's shm_open */
static int ms_precreate_raw_fd = -1; /* kept-open fd from raw open("/dev/shm/...") */

static void *ms_poller_thread(void *arg) {
    (void)arg;

    /* v126: RAW ASM poller — uses int $0x80 for ALL fstat/ftruncate/write,
     * matching EXACTLY what CSharedMemStream::Connect() does.
     * Previous poller used glibc fstat which went through __fxstat wrapper
     * that ALWAYS overrides st_size to 8192 for MasterStream fds → useless.
     * Runs for 10 minutes to cover the 4.5-minute gap before assertions. */

    debug_msg("S32-v129: MS-POLLER: started (STAT VARIANT DIAGNOSTIC)\n");

    /* v127: Test ALL i386 stat syscall variants on the MasterStream fd.
     * Connect() sees st_size=0 despite file being 8192 bytes.
     * We test every possible fstat syscall to find which one returns 0. */

    int test_fd = ms_precreate_raw_fd;
    if (test_fd < 0) test_fd = ms_precreate_fd;

    if (test_fd >= 0) {
        debug_int("S32-v129: STAT-DIAG on fd=", test_fd);

        /* --- Test 1: SYS_fstat64 (197) — what we've been using --- */
        {
            struct stat64 st;
            memset(&st, 0xCC, sizeof(st));
            long ret = raw32_fstat64(test_fd, &st);
            debug_int("S32-v129: [fstat64/197] ret=", ret);
            debug_int("  st_size=", (long)st.st_size);
            debug_hexline("  raw[0..63]: ", &st, 64);
            debug_hexline("  raw[40..79]: ", ((unsigned char*)&st) + 40, 40);
        }

        /* --- Test 2: SYS_fstat (108) — OLD stat, different struct layout --- */
        {
            unsigned char old_buf[128];
            memset(old_buf, 0xCC, sizeof(old_buf));
            long ret = raw32_fstat_old(test_fd, old_buf);
            debug_int("S32-v129: [fstat/108] ret=", ret);
            /* old struct stat: st_size is at offset 20, 4 bytes (unsigned long) */
            unsigned long old_size = *(unsigned long *)(old_buf + 20);
            debug_int("  st_size@off20=", (long)old_size);
            /* Also check offset 44 in case layout differs under FEX */
            long long alt_size = *(long long *)(old_buf + 44);
            debug_int("  val@off44=", (long)alt_size);
            debug_hexline("  raw[0..63]: ", old_buf, 64);
            debug_hexline("  raw[64..127]: ", old_buf + 64, 64);
        }

        /* --- Test 3: SYS_fstatat64 (300) with AT_EMPTY_PATH on "" --- */
        {
            struct stat64 st;
            memset(&st, 0xCC, sizeof(st));
            long ret = raw32_fstatat64(test_fd, "", &st, AT_EMPTY_PATH);
            debug_int("S32-v129: [fstatat64/300] ret=", ret);
            debug_int("  st_size=", (long)st.st_size);
        }

        /* --- Test 4: SYS_stat (106) by path --- */
        {
            unsigned char old_buf[128];
            memset(old_buf, 0xCC, sizeof(old_buf));
            long ret = raw32_stat_old(ms_poller_devshm_path, old_buf);
            debug_int("S32-v129: [stat/106] ret=", ret);
            unsigned long old_size = *(unsigned long *)(old_buf + 20);
            debug_int("  st_size@off20=", (long)old_size);
            debug_hexline("  raw[0..63]: ", old_buf, 64);
        }

        /* --- Test 5: SYS_statx (383) --- */
        {
            struct raw_statx sx;
            memset(&sx, 0xCC, sizeof(sx));
            long ret = raw32_statx(test_fd, "", 0x1000/*AT_EMPTY_PATH*/, 0x7FF/*STATX_ALL*/, &sx);
            debug_int("S32-v129: [statx/383] ret=", ret);
            debug_int("  stx_size=", (long)sx.stx_size);
            debug_hexline("  raw[0..63]: ", &sx, 64);
        }

        /* --- Test 6: glibc stat64() by path (goes through wrappers) --- */
        {
            struct stat64 st;
            memset(&st, 0xCC, sizeof(st));
            int ret = stat64(ms_poller_devshm_path, &st);
            debug_int("S32-v129: [glibc stat64] ret=", (long)ret);
            debug_int("  st_size=", (long)st.st_size);
        }

        /* --- Test 7: glibc fstat() — goes through our __fxstat wrapper --- */
        {
            struct stat st;
            memset(&st, 0xCC, sizeof(st));
            int ret = fstat(test_fd, &st);
            debug_int("S32-v129: [glibc fstat] ret=", (long)ret);
            debug_int("  st_size=", (long)st.st_size);
            debug_msg("  (NOTE: goes through __fxstat wrapper → always 8192)\n");
        }
    }

    /* Also test on a freshly opened fd (in case fd matters) */
    {
        long fresh_fd = raw32_openat(-100/*AT_FDCWD*/, ms_poller_devshm_path, O_RDONLY, 0);
        if (fresh_fd >= 0) {
            debug_int("S32-v129: FRESH-FD test on fd=", fresh_fd);

            /* fstat64 (197) */
            struct stat64 st64;
            memset(&st64, 0xCC, sizeof(st64));
            long r1 = raw32_fstat64((int)fresh_fd, &st64);
            debug_int("  [fstat64/197] ret=", r1);
            debug_int("  st_size=", (long)st64.st_size);

            /* fstat (108) */
            unsigned char old_buf[128];
            memset(old_buf, 0xCC, sizeof(old_buf));
            long r2 = raw32_fstat_old((int)fresh_fd, old_buf);
            debug_int("  [fstat/108] ret=", r2);
            unsigned long old_size = *(unsigned long *)(old_buf + 20);
            debug_int("  st_size@off20=", (long)old_size);
            debug_hexline("  raw[0..63]: ", old_buf, 64);

            raw32_close((int)fresh_fd);
        } else {
            debug_int("S32-v129: FRESH-FD open FAIL ret=", fresh_fd);
        }
    }

    /* v128: Test SYS_open (syscall 5) — the OLD open syscall.
     * Connect() uses raw int $0x80. If it uses SYS_open(5) instead of
     * SYS_openat(295), FEX might NOT do rootfs path translation for it,
     * causing ENOENT on Android (no real /dev/shm/). */
    {
        long sys_open_fd;
        __asm__ volatile ("int $0x80" : "=a"(sys_open_fd)
            : "0"(5/*SYS_open*/), "b"(ms_poller_devshm_path), "c"(O_RDONLY), "d"(0)
            : "memory");
        debug_int("S32-v129: SYS_open(5) on MasterStream fd=", sys_open_fd);
        if (sys_open_fd >= 0) {
            struct stat64 st;
            memset(&st, 0, sizeof(st));
            raw32_fstat64((int)sys_open_fd, &st);
            debug_int("  st_size=", (long)st.st_size);
            raw32_close((int)sys_open_fd);
            debug_msg("  SYS_open(5) WORKS — FEX handles it\n");
        } else {
            debug_int("  SYS_open(5) FAILED errno=", -sys_open_fd);
            debug_msg("  *** THIS COULD BE THE ROOT CAUSE! ***\n");
        }

        /* Also test SYS_open on /dev/shm/ directory itself */
        long dir_fd;
        __asm__ volatile ("int $0x80" : "=a"(dir_fd)
            : "0"(5/*SYS_open*/), "b"("/dev/shm"), "c"(O_RDONLY|O_DIRECTORY), "d"(0)
            : "memory");
        debug_int("S32-v129: SYS_open(5) on /dev/shm dir fd=", dir_fd);
        if (dir_fd >= 0) raw32_close((int)dir_fd);

        /* Compare: SYS_openat(295) on same path — known to work */
        long openat_fd = raw32_openat(-100/*AT_FDCWD*/, ms_poller_devshm_path, O_RDONLY, 0);
        debug_int("S32-v129: SYS_openat(295) same path fd=", openat_fd);
        if (openat_fd >= 0) raw32_close((int)openat_fd);

        /* Test SYS_open with O_CREAT|O_RDWR — what Connect()/Create() might use */
        long creat_fd;
        __asm__ volatile ("int $0x80" : "=a"(creat_fd)
            : "0"(5/*SYS_open*/), "b"(ms_poller_devshm_path), "c"(O_RDWR), "d"(0666)
            : "memory");
        debug_int("S32-v129: SYS_open(5) O_RDWR fd=", creat_fd);
        if (creat_fd >= 0) {
            struct stat64 st;
            memset(&st, 0, sizeof(st));
            raw32_fstat64((int)creat_fd, &st);
            debug_int("  st_size=", (long)st.st_size);
            raw32_close((int)creat_fd);
        }
    }

    /* v128: CRITICAL TEST — simulate Connect()'s ENTIRE path with raw int $0x80.
     * open → fstat → mmap2 → read header. If mmap2 returns zeros, THAT's the bug.
     * SYS_mmap2 (192) on i386 takes offset in PAGE UNITS (not bytes). */
    {
        debug_msg("S32-v129: === FULL Connect() SIMULATION (raw int $0x80 only) ===\n");

        /* Step 1: open via raw SYS_open(5) */
        long sim_fd;
        __asm__ volatile ("int $0x80" : "=a"(sim_fd)
            : "0"(5/*SYS_open*/), "b"(ms_poller_devshm_path), "c"(O_RDWR), "d"(0666)
            : "memory");
        debug_int("S32-v129: SIM open fd=", sim_fd);

        if (sim_fd >= 0) {
            /* Step 2: fstat via raw SYS_fstat64(197) */
            struct stat64 sim_st;
            memset(&sim_st, 0, sizeof(sim_st));
            long fsr = raw32_fstat64((int)sim_fd, &sim_st);
            debug_int("S32-v129: SIM fstat64 ret=", fsr);
            debug_int("S32-v129: SIM fstat64 st_size=", (long)sim_st.st_size);

            /* Step 3: mmap via raw SYS_mmap2(192) — MAP_SHARED.
             * CRITICAL: SYS_mmap2 has 6 args on i386. The 6th (offset in pages)
             * goes in ebp. Must save/restore ebp and set it to 0. Previous bug:
             * ebp had garbage → mmap from random offset → SIGBUS on access! */
            long sim_map;
            __asm__ volatile (
                "pushl %%ebp\n\t"
                "xorl %%ebp, %%ebp\n\t"  /* arg6: offset = 0 pages */
                "int $0x80\n\t"
                "popl %%ebp\n\t"
                : "=a"(sim_map)
                : "0"(192/*SYS_mmap2*/),
                  "b"(0/*addr*/),
                  "c"((long)sim_st.st_size/*length*/),
                  "d"(3/*PROT_READ|PROT_WRITE*/),
                  "S"(1/*MAP_SHARED*/),
                  "D"((int)sim_fd)
                : "memory");
            debug_int("S32-v129: SIM mmap2 ret=", sim_map);

            /* mmap returns high addresses on i386 (above 0x80000000) which are
             * negative as signed long. Error is only when ret is in [-4096, -1]. */
            if ((unsigned long)sim_map < 0xFFFFF000UL) {
                /* Step 4: read first 32 bytes from mapped memory */
                unsigned char *mp = (unsigned char *)(unsigned long)sim_map;
                debug_hexline("S32-v129: SIM mapped hdr: ", mp, 32);
                unsigned int mhdr0 = *(unsigned int *)mp;
                unsigned int mhdr4 = *(unsigned int *)(mp + 4);
                unsigned int mhdr8 = *(unsigned int *)(mp + 8);
                unsigned int mhdr12 = *(unsigned int *)(mp + 12);
                debug_int("S32-v129: SIM hdr[0]=", (long)mhdr0);
                debug_int("S32-v129: SIM hdr[4]=", (long)mhdr4);
                debug_int("S32-v129: SIM hdr[8]=", (long)mhdr8);
                debug_int("S32-v129: SIM hdr[12]=", (long)mhdr12);

                /* Also read via raw SYS_read for comparison */
                unsigned char readbuf[32];
                raw32_lseek((int)sim_fd, 0, 0);
                long rr;
                __asm__ volatile ("int $0x80" : "=a"(rr)
                    : "0"(3/*SYS_read*/), "b"((int)sim_fd), "c"(readbuf), "d"(32)
                    : "memory");
                debug_int("S32-v129: SIM read ret=", rr);
                debug_hexline("S32-v129: SIM read hdr: ", readbuf, 32);

                if (mhdr0 == 0 && mhdr4 == 0 && mhdr8 == 0) {
                    debug_msg("S32-v129: *** MMAP RETURNED ALL ZEROS — THIS IS THE BUG! ***\n");
                } else if (mhdr0 != *(unsigned int *)readbuf) {
                    debug_msg("S32-v129: *** MMAP vs READ MISMATCH! ***\n");
                } else {
                    debug_msg("S32-v129: SIM mmap content matches read — mmap OK\n");
                }

                /* Unmap */
                long munr;
                __asm__ volatile ("int $0x80" : "=a"(munr)
                    : "0"(91/*SYS_munmap*/), "b"(sim_map), "c"((long)sim_st.st_size)
                    : "memory");
            } else {
                debug_msg("S32-v129: *** SIM mmap2 FAILED! ***\n");
                debug_int("S32-v129: SIM mmap2 errno=", -sim_map);
            }

            /* Also test mmap2 via SYS_mmap (old, syscall 90) - struct-based */
            {
                unsigned long mmap_args[6];
                mmap_args[0] = 0; /* addr */
                mmap_args[1] = (unsigned long)sim_st.st_size; /* length */
                mmap_args[2] = 3; /* PROT_READ|PROT_WRITE */
                mmap_args[3] = 1; /* MAP_SHARED */
                mmap_args[4] = (unsigned long)(int)sim_fd; /* fd */
                mmap_args[5] = 0; /* offset (bytes) */
                long old_map;
                __asm__ volatile ("int $0x80" : "=a"(old_map)
                    : "0"(90/*SYS_mmap*/), "b"(mmap_args)
                    : "memory");
                debug_int("S32-v129: SIM old_mmap(90) ret=", old_map);
                if ((unsigned long)old_map < 0xFFFFF000UL) {
                    unsigned char *op = (unsigned char *)(unsigned long)old_map;
                    unsigned int oh0 = *(unsigned int *)op;
                    debug_int("S32-v129: SIM old_mmap hdr[0]=", (long)oh0);
                    debug_hexline("S32-v129: SIM old_mmap hdr: ", op, 32);
                    /* Unmap */
                    long munr2;
                    __asm__ volatile ("int $0x80" : "=a"(munr2)
                        : "0"(91/*SYS_munmap*/), "b"(old_map), "c"((long)sim_st.st_size)
                        : "memory");
                } else {
                    debug_int("S32-v129: SIM old_mmap(90) errno=", -old_map);
                }
            }

            raw32_close((int)sim_fd);
        }
        debug_msg("S32-v129: === END Connect() SIMULATION ===\n");
    }

    /* v131: Wait for webhelper PID file, then update ms_poller_buf with correct PID.
     * The constructor wrote BrowserReady with S32's PID (fallback), but the webhelper
     * PID is needed for SetWebUITransportWebhelperPID. steamwebhelper.sh v93+ writes
     * the PID to /tmp/steam_webhelper_pid before exec. */
    int pid_updated = 0;
    for (int wait = 0; wait < 300 && ms_poller_active; wait++) { /* up to 30s */
        unsigned int wh_pid = read_webhelper_pid_file();
        if (wh_pid > 0 && wh_pid != cached_wh_pid) {
            cached_wh_pid = wh_pid;
            debug_int("S32-v131: ms_poller: webhelper PID detected: ", (long)wh_pid);
            /* Re-build ms_poller_buf with correct webhelper PID */
            write_browserready_ring(ms_poller_buf, 8192);
            /* Re-fill all MasterStream files with corrected PID */
            long rfd = raw32_openat(-100, ms_poller_devshm_path, O_RDWR, 0666);
            if (rfd >= 0) {
                raw32_lseek((int)rfd, 0, 0);
                raw32_write((int)rfd, ms_poller_buf, 8192);
                raw32_close((int)rfd);
                debug_msg("S32-v131: re-injected BrowserReady with webhelper PID\n");
            }
            /* Note: bridge_anon_map (Shm_ mmap singleton) will pick up
             * the updated ms_poller_buf on next refill via write_browserready_ring. */
            pid_updated = 1;
            break;
        }
        struct timespec ts = {0, 100000000}; /* 100ms */
        nanosleep(&ts, NULL);
    }
    if (!pid_updated) {
        debug_msg("S32-v131: WARNING: webhelper PID file not found after 30s\n");
    }

    long last_size = 8192;
    unsigned char last_hdr[32];
    memset(last_hdr, 0xFF, sizeof(last_hdr));
    int refill_count = 0;
    int content_change_count = 0;

    /* v127: Fast 10ms poll for first 60s (6000 iters), then 100ms.
     * ALSO read file content header (first 32 bytes) to detect if
     * someone (webhelper Create()?) truncates+reinitializes the file.
     * The file could be 8192 bytes (fstat correct) but all ZEROS
     * if ftruncate(8192) zeroed it and the header hasn't been rewritten. */
    for (int iter = 0; iter < 12000 && ms_poller_active; iter++) {
        struct timespec ts;
        if (iter < 6000) { ts.tv_sec = 0; ts.tv_nsec = 10000000; } /* 10ms for 60s */
        else { ts.tv_sec = 0; ts.tv_nsec = 100000000; } /* 100ms after */
        nanosleep(&ts, NULL);

        /* Open via raw int $0x80 and check BOTH size AND content */
        long rfd = raw32_openat(-100, ms_poller_devshm_path, O_RDWR, 0666);
        if (rfd >= 0) {
            struct stat64 rst;
            memset(&rst, 0, sizeof(rst));
            long fsr = raw32_fstat64((int)rfd, &rst);
            long cur_size = (fsr == 0) ? (long)rst.st_size : -1;

            /* Read first 32 bytes of content via raw asm */
            unsigned char cur_hdr[32];
            memset(cur_hdr, 0xEE, sizeof(cur_hdr));
            raw32_lseek((int)rfd, 0, 0/*SEEK_SET*/);
            long rd = 0;
            if (cur_size >= 32) {
                /* raw read via int $0x80 */
                long rr;
                __asm__ volatile ("int $0x80" : "=a"(rr)
                    : "0"(3/*SYS_read*/), "b"((int)rfd), "c"(cur_hdr), "d"(32)
                    : "memory");
                rd = rr;
            }

            /* Check if content header changed */
            int hdr_changed = (memcmp(cur_hdr, last_hdr, 32) != 0);
            int is_zeros = 1;
            for (int i = 0; i < 32; i++) {
                if (cur_hdr[i] != 0) { is_zeros = 0; break; }
            }

            /* Log on: size change, content change, periodic, or all zeros */
            if (cur_size != last_size || hdr_changed || is_zeros ||
                iter < 10 || (iter < 6000 && (iter % 500 == 0)) ||
                (iter >= 6000 && (iter % 100 == 0))) {

                debug_int("S32-v129: POLL[", iter);
                debug_int("] size=", cur_size);

                if (hdr_changed) {
                    content_change_count++;
                    debug_int("] *** CONTENT CHANGED *** #", content_change_count);
                    debug_hexline("  new_hdr: ", cur_hdr, 32);
                    debug_hexline("  old_hdr: ", last_hdr, 32);
                }

                if (is_zeros) {
                    debug_msg("  *** CONTENT IS ALL ZEROS! ***\n");
                }

                if (cur_size != last_size && last_size >= 0) {
                    debug_int("  WAS=", last_size);
                    debug_msg("  *** SIZE CHANGED ***\n");
                }

                /* Log hdr[0] specifically (might be m_cubBuffer) */
                unsigned int hdr0 = *(unsigned int *)(cur_hdr);
                unsigned int hdr8 = *(unsigned int *)(cur_hdr + 8);
                debug_int("  hdr[0]=", (long)hdr0);
                debug_int("  hdr[8]=", (long)hdr8);
            }

            memcpy(last_hdr, cur_hdr, 32);

            /* If content is all zeros or size wrong, refill */
            if ((is_zeros || (fsr == 0 && rst.st_size < 8192)) && refill_count < 200) {
                raw32_ftruncate64((int)rfd, 8192, 0);
                raw32_lseek((int)rfd, 0, 0/*SEEK_SET*/);
                long wr = raw32_write((int)rfd, ms_poller_buf, 8192);
                refill_count++;
                debug_int("S32-v129: REFILLED #", refill_count);
                debug_int("  raw_write=", wr);
                memcpy(last_hdr, ms_poller_buf, 32); /* update tracked header */
            }

            last_size = cur_size;
            raw32_close((int)rfd);
        } else {
            if (iter < 10 || (iter % 500 == 0)) {
                debug_int("S32-v129: POLL[", iter);
                debug_int("] OPEN FAIL ret=", rfd);
            }
            if (rfd == -2 /*ENOENT*/ && refill_count < 200) {
                long cfd = raw32_openat(-100, ms_poller_devshm_path,
                                        O_CREAT | O_RDWR, 0666);
                if (cfd >= 0) {
                    raw32_ftruncate64((int)cfd, 8192, 0);
                    raw32_write((int)cfd, ms_poller_buf, 8192);
                    raw32_close((int)cfd);
                    refill_count++;
                    debug_int("S32-v129: RECREATED #", refill_count);
                }
            }
            last_size = -1;
        }
    }
    debug_msg("S32-v129: MS-POLLER: done\n");
    return NULL;
}

/* v116: ftruncate wrapper — detect who truncates MasterStream fds */
typedef int (*real_ftruncate_fn)(int, off_t);
static real_ftruncate_fn real_ftruncate_ptr = NULL;
static volatile int ftrunc_log_count = 0;

int ftruncate(int fd, off_t length) {
    if (!real_ftruncate_ptr)
        real_ftruncate_ptr = (real_ftruncate_fn)dlsym(RTLD_NEXT, "ftruncate");
    /* v123: BLOCK ftruncate on MasterStream fds to anything other than 8192.
     * Create() does ftruncate(fd, 0) to reset → empties the file → assertion.
     * Only allow 8192 (the correct size). Block everything else. */
    if ((is_ms_fd(fd) || fd == ms_precreate_fd) && ms_poller_active) {
        int cnt = __sync_fetch_and_add(&ftrunc_log_count, 1);
        if (cnt < 30) {
            debug_int("S32: ftruncate MS fd=", fd);
            debug_int("  length=", (long)length);
        }
        if (length == 8192) {
            return real_ftruncate_ptr(fd, length);
        }
        if (cnt < 30)
            debug_int("S32: ftruncate MS BLOCKED! wanted=", (long)length);
        return 0; /* fake success */
    }
    return real_ftruncate_ptr(fd, length);
}

int ftruncate64(int fd, __off64_t length) {
    return ftruncate(fd, (off_t)length);
}

/* v113: Bridge sync — connect to WH bridge, receive Shm_ fds via SCM_RIGHTS,
 * read their content, and write to S32 overlay. This makes WH-created Shm_
 * files visible to the 32-bit client despite FEX overlay isolation.
 * Returns: number of files synced, or -1 if bridge not available. */
static volatile int bridge_sync_count = 0;
static volatile int bridge_sync_done = 0;

static int do_bridge_sync(void) {
    if (!real_connect_ptr)
        real_connect_ptr = (real_connect_fn)dlsym(RTLD_NEXT, "connect");
    if (!real_open_ptr)
        real_open_ptr = (real_open_fn)dlsym(RTLD_NEXT, "open");

    int bsock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bsock < 0) return -1;

    struct sockaddr_un baddr;
    memset(&baddr, 0, sizeof(baddr));
    baddr.sun_family = AF_UNIX;
    memcpy(baddr.sun_path, "\0shm_bridge_64to32", 19);

    int cret = real_connect_ptr(bsock, (struct sockaddr *)&baddr,
        offsetof(struct sockaddr_un, sun_path) + 19);
    if (cret != 0) {
        close(bsock);
        return -1;
    }

    int synced = 0;

    while (1) {
        unsigned char nbuf[66];
        struct iovec iov;
        iov.iov_base = nbuf;
        iov.iov_len = sizeof(nbuf);

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

        ssize_t r = recvmsg(bsock, &msg, 0);
        if (r < 2) break;

        int bname_len = nbuf[0] | (nbuf[1] << 8);
        if (bname_len == 0) break; /* terminator */
        if (bname_len < 0 || bname_len > 63) break;
        if (r < 2 + bname_len) break;

        char bname[64];
        memset(bname, 0, sizeof(bname));
        memcpy(bname, nbuf + 2, bname_len);

        /* Extract fd from cmsg */
        int received_fd = -1;
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
            cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
            memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(int));
        }

        if (received_fd < 0) continue;

        /* Read WH file content and write to S32 overlay */
        struct stat wst;
        if (fstat(received_fd, &wst) == 0 && wst.st_size > 0) {
            char spath[256];
            build_shm_path(spath, sizeof(spath), bname);

            int ofd = real_open_ptr(spath, O_CREAT | O_RDWR | O_TRUNC, 0666);
            if (ofd >= 0) {
                ftruncate(ofd, wst.st_size);
                char cbuf[4096];
                lseek(received_fd, 0, SEEK_SET);
                ssize_t rr;
                off_t off = 0;
                while ((rr = read(received_fd, cbuf, sizeof(cbuf))) > 0) {
                    pwrite(ofd, cbuf, rr, off);
                    off += rr;
                }
                close(ofd);
                synced++;
                if (synced <= 10) {
                    debug_str("S32: BRIDGE-SYNC: ", bname, "\n");
                    debug_int("  size=", (long)wst.st_size);
                }
            }
        }
        close(received_fd);
    }

    close(bsock);
    __sync_fetch_and_add(&bridge_sync_count, synced);
    bridge_sync_done = 1;
    return synced;
}

int shm_open(const char *name, int oflag, mode_t mode) {
    char path[256];
    build_shm_path(path, sizeof(path), name);

    if (!real_open_ptr)
        real_open_ptr = (real_open_fn)dlsym(RTLD_NEXT, "open");

    /* v110: Log ALL shm_open calls to catch MasterStream */
    {
        int cnt = __sync_fetch_and_add(&shm_open_count_32, 0);
        if (cnt < 30) {
            debug_str("S32: shm_open ALL: ", name, "\n");
            debug_int("  flags=", oflag);
        }
    }

    /* v127: Log ALL shm_open for MasterStream — even at Connect time */
    if (strstr(name, "MasterStream") != NULL) {
        debug_str("S32-v129: shm_open() CAUGHT MasterStream: ", name, "\n");
        debug_int("  oflag=", oflag);
    }
    /* v119: MasterStream — fill with CMsgBrowserReady if empty.
     * DUAL WRITE: create at SHM_REDIR_DIR (for cross-process sharing)
     * AND at /dev/shm/ (our overlay, for Connect()'s raw syscall). */
    if (strstr(name, "SteamChrome_MasterStream") != NULL) {
        int msfd = real_open_ptr(path, O_CREAT | O_RDWR, 0666);
        if (msfd >= 0) {
            add_ms_fd(msfd);
            struct stat mst;
            int cnt = __sync_fetch_and_add(&ms_open_count, 1);
            if (cnt < 20)
                debug_str("S32: MS shm_open: ", name, "\n");

            /* Prepare CMsgBrowserReady buffer */
            unsigned char msbuf[8192];
            memset(msbuf, 0, sizeof(msbuf));
            unsigned int *hdr = (unsigned int *)msbuf;
            hdr[0] = 0; hdr[1] = 10; hdr[2] = 8192; hdr[3] = 10;
            unsigned int mt = 0, msz = 2;
            memcpy(msbuf + 16, &mt, 4);
            memcpy(msbuf + 20, &msz, 4);
            msbuf[24] = 0x08; msbuf[25] = 0x01;

            if (fstat(msfd, &mst) == 0) {
                if (cnt < 20) {
                    debug_int("  fd=", msfd);
                    debug_int("  size=", (long)mst.st_size);
                }
                if (mst.st_size < 8192) {
                    debug_int("S32: MS shm_open: FILLING, was=", (long)mst.st_size);
                    ftruncate(msfd, 8192);
                    lseek(msfd, 0, SEEK_SET);
                    write(msfd, msbuf, 8192);
                    lseek(msfd, 0, SEEK_SET);
                    debug_msg("S32: MS shm_open: filled OK\n");
                }
            }

            /* v119: Also write to /dev/shm/ via raw syscall (our overlay).
             * This ensures Connect()'s raw openat finds the file. */
            {
                /* Strip leading "/" from shm_open name to get bare filename */
                const char *fname = name;
                if (fname[0] == '/') fname++;
                char devshm_path[256];
                {
                    int n = 0;
                    const char *d = "/dev/shm/";
                    while (*d) devshm_path[n++] = *d++;
                    const char *s = fname;
                    while (*s && n < 250) devshm_path[n++] = *s++;
                    devshm_path[n] = '\0';
                }
                int ofd = (int)syscall(SYS_openat, AT_FDCWD, devshm_path,
                                       O_CREAT | O_RDWR | O_TRUNC, 0666);
                if (ofd >= 0) {
                    syscall(SYS_ftruncate, ofd, (long)8192);
                    syscall(SYS_write, ofd, msbuf, (long)8192);
                    close(ofd);
                    if (cnt < 10)
                        debug_str("S32: MS shm_open: dual-write /dev/shm/", fname, "\n");
                }
            }
        } else {
            debug_str("S32: MS shm_open FAIL: ", name, "\n");
            debug_int("  errno=", errno);
        }
        return msfd;
    }

    /* v107: Shm_ IPC channels — let client create its own files.
     * On a real PC, the client creates 3 Shm_ files (O_CREAT|O_EXCL succeeds)
     * and opens 5 more from webhelper (O_EXCL→EEXIST then O_RDWR).
     * Previously we returned the bridge fd for ALL Shm_ → single channel
     * → IPC init stalled (60s timeout). Now each Shm_ gets its own fd. */
    if (strstr(name, "Shm_") != NULL) {
        int count = __sync_fetch_and_add(&shm_open_count_32, 1);

        if ((oflag & O_CREAT) && (oflag & O_EXCL)) {
            /* v113: Before local create, check if the WH already created
             * this file (in its overlay, invisible to us due to FEX isolation).
             * If the bridge has it, sync the content to our overlay and
             * return EEXIST so the client opens with O_RDWR (reads WH data).
             * Without this, the client creates empty files for WH channels
             * and misses CMsgBrowserReady etc. */
            {
                /* First: try opening — maybe already synced */
                int existing = real_open_ptr(path, O_RDWR | O_CLOEXEC, 0666);
                if (existing >= 0) {
                    struct stat est;
                    if (fstat(existing, &est) == 0 && est.st_size > 0) {
                        /* File exists with content (synced or previous create) */
                        close(existing);
                        if (count < 60)
                            debug_str("S32: shm Shm_ EXCL→EEXIST(synced): ", name, "\n");
                        errno = EEXIST;
                        return -1;
                    }
                    close(existing);
                }

                /* Not found locally — try bridge sync */
                if (!bridge_sync_done) {
                    int synced = do_bridge_sync();
                    if (synced > 0 && count < 10)
                        debug_int("S32: bridge sync in EXCL path, synced=", synced);
                }

                /* Re-check after sync */
                existing = real_open_ptr(path, O_RDWR | O_CLOEXEC, 0666);
                if (existing >= 0) {
                    struct stat est;
                    if (fstat(existing, &est) == 0 && est.st_size > 0) {
                        close(existing);
                        if (count < 60)
                            debug_str("S32: shm Shm_ EXCL→EEXIST(bridge): ", name, "\n");
                        errno = EEXIST;
                        return -1;
                    }
                    close(existing);
                }
            }

            /* Normal O_CREAT|O_EXCL — client creating its own channel */
            int fd = real_open_ptr(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC,
                                   mode ? mode : 0666);
            if (fd >= 0) {
                if (count < 60) {
                    debug_str("S32: shm Shm_ CREATE OK: ", name, "\n");
                    debug_int("  fd=", fd);
                }
                return fd;
            }
            int saved_errno = errno;
            if (saved_errno == EEXIST) {
                /* Already exists (webhelper or previous run) → open O_RDWR */
                fd = real_open_ptr(path, O_RDWR | O_CLOEXEC, 0666);
                if (count < 60) {
                    debug_str("S32: shm Shm_ EEXIST→RDWR: ", name, "\n");
                    debug_int("  fd=", fd);
                }
                return fd;
            }
            /* v109: FEX overlay returns wrong errno (ENOENT instead of EEXIST).
             * Check if file actually exists — if so, return -1/EEXIST. */
            fd = real_open_ptr(path, O_RDWR | O_CLOEXEC, 0666);
            if (fd >= 0) {
                close(fd);
                if (count < 60) {
                    debug_str("S32: shm Shm_ EXCL→EEXIST: ", name, "\n");
                    debug_int("  orig_errno=", saved_errno);
                }
                errno = EEXIST;
                return -1;
            }
            if (count < 60) {
                debug_str("S32: shm Shm_ CREATE FAIL: ", name, "\n");
                debug_int("  errno=", saved_errno);
                debug_str("  path=", path, "\n");
            }
            errno = saved_errno;
            return -1;
        }

        if (oflag & O_CREAT) {
            /* O_CREAT without O_EXCL — just create or open */
            int fd = real_open_ptr(path, O_CREAT | O_RDWR | O_CLOEXEC,
                                   mode ? mode : 0666);
            if (count < 30) {
                debug_str("S32: shm Shm_ O_CREAT: ", name, "\n");
                debug_int("  fd=", fd);
            }
            return fd;
        }

        /* v129: Detect MasterStream Connect() — O_RDONLY on Shm_ file.
         * On a real PC, ONLY MasterStream uses O_RDONLY for shm_open.
         * The name is u1000-Shm_HASH (Valve's custom hash of the logical
         * MasterStream name like "SteamChrome_MasterStream_PID_PORT").
         * Our literal "MasterStream" check never matched the hashed name.
         * Create a proper 8208-byte file (16 hdr + 8192 ring buffer). */
        if ((oflag & O_ACCMODE) == O_RDONLY && !(oflag & O_CREAT)) {
            int mc = __sync_fetch_and_add(&ms_connect_detect_count, 1);
            if (mc < 200)
                debug_str("S32-v129: shm_open O_RDONLY Shm_: ", name, "\n");

            /* First try opening existing file with correct size */
            int msfd = real_open_ptr(path, O_RDWR | O_CLOEXEC, 0666);
            if (msfd >= 0) {
                struct stat mst;
                if (fstat(msfd, &mst) == 0 && mst.st_size >= 8208) {
                    if (mc < 200) {
                        debug_str("S32-v139: MS Connect found: ", name, "\n");
                        debug_int("  size=", (long)mst.st_size);
                    }
                    /* v139: The file at SHM_REDIR_DIR may be empty (created by
                     * O_CREAT handler without content). Read the REAL content
                     * from rootfs /dev/shm/ and inject into both the SHM_REDIR_DIR
                     * file AND FEX overlay via raw int $0x80. */
                    {
                        /* Try reading from the rootfs /dev/shm/ hashed file first */
                        unsigned char ibuf[8208];
                        long rd = 0;
                        {
                            char devshm[256];
                            int dn = 0;
                            const char *d = "/dev/shm/";
                            while (*d) devshm[dn++] = *d++;
                            const char *nm = name;
                            if (*nm == '/') nm++;
                            while (*nm && dn < 254) devshm[dn++] = *nm++;
                            devshm[dn] = '\0';
                            long rfd = raw32_openat(-100, devshm, O_RDONLY, 0);
                            if (rfd >= 0) {
                                rd = read((int)rfd, ibuf, 8208);
                                raw32_close((int)rfd);
                            }
                        }
                        /* If overlay file empty, use proper CMsgBrowserReady */
                        if (rd < 16) {
                            memset(ibuf, 0, 8208);
                            write_browserready_ring(ibuf, 8208);
                            rd = 8208;
                        }
                        /* Write content to SHM_REDIR_DIR file */
                        ftruncate(msfd, rd);
                        lseek(msfd, 0, SEEK_SET);
                        write(msfd, ibuf, rd);
                        lseek(msfd, 0, SEEK_SET);
                        /* Also inject into FEX overlay */
                        if (rd >= 8208) {
                            char devshm[256];
                            int n = 0;
                            const char *d = "/dev/shm/";
                            while (*d) devshm[n++] = *d++;
                            const char *nm = name;
                            if (*nm == '/') nm++;
                            while (*nm && n < 254) devshm[n++] = *nm++;
                            devshm[n] = '\0';
                            long ofd = raw32_openat(-100, devshm, O_CREAT | O_RDWR | O_TRUNC, 0666);
                            if (ofd >= 0) {
                                raw32_ftruncate64((int)ofd, 8208, 0);
                                raw32_write((int)ofd, ibuf, 8208);
                                raw32_close((int)ofd);
                                if (mc < 20)
                                    debug_str("S32-v139: injected to overlay: ", devshm, "\n");
                            }
                        }
                    }
                    return msfd;
                }
                close(msfd);
            }

            /* File doesn't exist or wrong size — create as MasterStream */
            msfd = real_open_ptr(path, O_CREAT | O_RDWR | O_CLOEXEC, 0666);
            if (msfd >= 0) {
                /* v130: Write proper 76-byte CMsgBrowserReady (PC format) */
                unsigned char ms_data[8208];
                memset(ms_data, 0, sizeof(ms_data));
                write_browserready_ring(ms_data, sizeof(ms_data));

                ftruncate(msfd, 8208);
                pwrite(msfd, ms_data, sizeof(ms_data), 0);

                if (mc < 200) {
                    debug_str("S32-v129: MS shm_open CREATE: ", name, "\n");
                    debug_int("  fd=", msfd);
                }

                /* Also write to /dev/shm/ via raw syscall (for any raw access) */
                {
                    const char *fname = name;
                    if (fname[0] == '/') fname++;
                    char devshm_ms[256];
                    {
                        int n = 0;
                        const char *d = "/dev/shm/";
                        while (*d) devshm_ms[n++] = *d++;
                        const char *s = fname;
                        while (*s && n < 250) devshm_ms[n++] = *s++;
                        devshm_ms[n] = '\0';
                    }
                    int ofd = (int)syscall(SYS_openat, AT_FDCWD, devshm_ms,
                                           O_CREAT | O_RDWR | O_TRUNC, 0666);
                    if (ofd >= 0) {
                        syscall(SYS_ftruncate, ofd, (long)8208);
                        syscall(SYS_write, ofd, ms_data, (long)8208);
                        close(ofd);
                    }
                }
                return msfd;
            }
            /* Fall through to general non-O_CREAT handler if create failed */
        }

        /* v113: Non-O_CREAT: client polling for webhelper-created files.
         * Try local overlay first. If not found, bridge-sync ALL WH files
         * to our overlay, then retry. If still not found, auto-create
         * an empty ring buffer as fallback. */
        {
            int fd = real_open_ptr(path, O_RDWR | O_CLOEXEC, 0666);
            if (fd >= 0) {
                if (count < 200) {
                    debug_str("S32: shm Shm_ POLL found: ", name, "\n");
                    debug_int("  fd=", fd);
                }
                {
                    struct stat st;
                    if (fstat(fd, &st) == 0 && count < 30)
                        debug_int("  size=", (long)st.st_size);
                }
                return fd;
            }
        }

        /* Not found locally — try bridge sync (copies WH files to our overlay) */
        {
            int pn = __sync_fetch_and_add(&shm_poll_fail_count, 1);
            if (pn == 0 || (pn >= 5 && (pn % 10) == 0)) {
                int synced = do_bridge_sync();
                if (synced > 0) {
                    /* Bridge synced files — retry local open */
                    int fd = real_open_ptr(path, O_RDWR | O_CLOEXEC, 0666);
                    if (fd >= 0) {
                        if (count < 60) {
                            debug_str("S32: shm Shm_ POLL→synced: ", name, "\n");
                            debug_int("  fd=", fd);
                            struct stat st;
                            if (fstat(fd, &st) == 0)
                                debug_int("  size=", (long)st.st_size);
                        }
                        return fd;
                    }
                } else if (pn < 20 || (pn % 25) == 0) {
                    debug_int("S32: BRIDGE-SYNC: no files, pn=", pn);
                }
            }

            /* v113: Auto-create as fallback — empty ring buffer */
            {
                int cfd = real_open_ptr(path, O_CREAT | O_RDWR | O_CLOEXEC, 0666);
                if (cfd >= 0) {
                    struct stat cst;
                    if (fstat(cfd, &cst) == 0 && cst.st_size == 0) {
                        ftruncate(cfd, 8192); /* v122: total 8192 bytes (header + ring) */
                        unsigned char hdr_buf[16];
                        memset(hdr_buf, 0, sizeof(hdr_buf));
                        unsigned int *h = (unsigned int *)hdr_buf;
                        h[2] = 8192; /* capacity */
                        pwrite(cfd, hdr_buf, 16, 0);
                        if (count < 30)
                            debug_str("S32: Shm_ AUTO-CREATE: ", name, "\n");
                    }
                    return cfd;
                }
                if (count < 30) {
                    debug_str("S32: shm Shm_ POLL FAIL: ", name, "\n");
                    debug_int("  errno=", errno);
                }
                errno = ENOENT;
                return -1;
            }
        }
    }

    /* Generic SHM path (non-Shm_, non-MasterStream) */
    {
        int real_flags;
        if (oflag & O_CREAT) {
            real_flags = (oflag & ~O_EXCL) | O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW;
            if (!mode) mode = 0666;
        } else {
            real_flags = oflag | O_CLOEXEC | O_NOFOLLOW;
            if (!mode) mode = 0666;
        }
        int fd = real_open_ptr(path, real_flags, mode);
        int count = __sync_fetch_and_add(&shm_open_count_32, 1);
        if (count < 30 || !(oflag & O_CREAT)) {
            debug_str("S32: shm_open(", name, ")\n");
            debug_int("  flags=", oflag);
            debug_int("  fd=", fd);
        }
        return fd;
    }
}

int shm_unlink(const char *name) {
    char path[256];
    build_shm_path(path, sizeof(path), name);
    int ret = unlink(path);
    /* v121: Log MasterStream unlinks prominently */
    if (strstr(name, "MasterStream")) {
        debug_str("S32: shm_unlink MASTERSTREAM: ", name, "\n");
        debug_int("  ret=", ret);
        debug_int("  errno=", errno);
    } else {
        debug_str("S32: shm_unlink(", name, ")\n");
    }
    return ret;
}

/* v132: sem_open/sem_unlink wrappers — redirect POSIX named semaphores to SHM_REDIR_DIR.
 * sem_open("/name") creates /dev/shm/sem.name on Linux. FEX overlay isolation
 * makes these invisible across processes. Redirect so S32 and WH share them. */
#include <semaphore.h>
typedef sem_t *(*real_sem_open_fn)(const char *, int, ...);
static real_sem_open_fn real_sem_open_ptr = NULL;

sem_t *sem_open(const char *name, int oflag, ...) {
    if (!real_sem_open_ptr)
        real_sem_open_ptr = (real_sem_open_fn)dlsym(RTLD_NEXT, "sem_open");
    static volatile int sem_count = 0;
    int cnt = __sync_fetch_and_add(&sem_count, 1);
    if (cnt < 20) {
        debug_str("S32: sem_open(", name ? name : "NULL", ")\n");
        debug_int("  oflag=", oflag);
    }
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
    typedef int (*fn)(const char *);
    static fn real = NULL;
    if (!real) real = (fn)dlsym(RTLD_NEXT, "sem_unlink");
    debug_str("S32: sem_unlink(", name ? name : "NULL", ")\n");
    return real(name);
}

/* Forward declaration */
static int is_shmem_path(const char *path);

/* v133: Forward declarations for fopen wrappers */
static int generate_proc_net_tcp(int is_ipv6);
static int build_proc_pid_prefix(char *buf, int bufsz, unsigned int pid);

/* v133: fopen/fopen64 wrapper — intercept /proc/net/tcp (glibc fopen uses
 * internal __openat which bypasses our open/openat/syscall wrappers).
 * Also intercept /proc/<WH_PID>/ paths for FD scanning. */
typedef FILE *(*real_fopen_fn)(const char *, const char *);
static real_fopen_fn real_fopen_ptr = NULL;
static real_fopen_fn real_fopen64_ptr = NULL;

static FILE *fopen_common(const char *pathname, const char *mode,
                          real_fopen_fn real_fn, const char *wrapper_name) {
    if (!pathname) return real_fn(pathname, mode);

    /* Intercept /proc/net/tcp — generate fake content */
    if (strcmp(pathname, "/proc/net/tcp") == 0 ||
        strcmp(pathname, "/proc/self/net/tcp") == 0) {
        debug_str("S32: fopen PROC-NET-TCP intercepted: ", pathname, "\n");
        int fd = generate_proc_net_tcp(0);
        if (fd >= 0) return fdopen(fd, "r");
    }
    if (strcmp(pathname, "/proc/net/tcp6") == 0 ||
        strcmp(pathname, "/proc/self/net/tcp6") == 0) {
        debug_str("S32: fopen PROC-NET-TCP6 intercepted: ", pathname, "\n");
        int fd = generate_proc_net_tcp(1);
        if (fd >= 0) return fdopen(fd, "r");
    }

    /* Redirect /proc/<WH_PID>/xxx → /proc/self/xxx */
    if (strncmp(pathname, "/proc/", 6) == 0) {
        unsigned int wh_pid = get_webhelper_pid();
        if (wh_pid > 0) {
            char prefix[32];
            int prefixlen = build_proc_pid_prefix(prefix, sizeof(prefix), wh_pid);
            if (strncmp(pathname, prefix, prefixlen) == 0) {
                char redir[256];
                int n = 0;
                const char *s = "/proc/self/";
                while (*s && n < 240) redir[n++] = *s++;
                s = pathname + prefixlen;
                while (*s && n < 254) redir[n++] = *s++;
                redir[n] = '\0';
                static volatile int fopen_redir_count = 0;
                int cnt = __sync_fetch_and_add(&fopen_redir_count, 1);
                if (cnt < 20)
                    debug_str("S32: fopen /proc/<WH_PID> redirect: ", redir, "\n");
                return real_fn(redir, mode);
            }
        }
        /* Diagnostic: log /proc/ fopen calls */
        static volatile int fopen_proc_count = 0;
        int cnt = __sync_fetch_and_add(&fopen_proc_count, 1);
        if (cnt < 30)
            debug_str("S32: fopen /proc/: ", pathname, "\n");
    }

    return real_fn(pathname, mode);
}

FILE *fopen(const char *pathname, const char *mode) {
    if (!real_fopen_ptr)
        real_fopen_ptr = (real_fopen_fn)dlsym(RTLD_NEXT, "fopen");
    return fopen_common(pathname, mode, real_fopen_ptr, "fopen");
}

FILE *fopen64(const char *pathname, const char *mode) {
    if (!real_fopen64_ptr)
        real_fopen64_ptr = (real_fopen_fn)dlsym(RTLD_NEXT, "fopen64");
    return fopen_common(pathname, mode, real_fopen64_ptr, "fopen64");
}

/* v133: popen wrapper — intercept lsof calls for WebUITransport PID check.
 * S32 runs: popen("/usr/bin/lsof -P -F upnR -i TCP@127.0.0.1:<port>", "r")
 * to find the PID of the process owning a TCP connection.
 * On Android/FEX, lsof can't read /proc/net/tcp (SELinux), so we return
 * fake output with the webhelper PID directly. */
typedef FILE *(*real_popen_fn)(const char *, const char *);
static real_popen_fn real_popen_ptr = NULL;

FILE *popen(const char *command, const char *type) {
    if (!real_popen_ptr)
        real_popen_ptr = (real_popen_fn)dlsym(RTLD_NEXT, "popen");

    /* Intercept lsof calls for TCP connections.
     * Must use real popen("printf ...") so pclose() works (needs real child). */
    if (command && strstr(command, "lsof") && strstr(command, "TCP@127.0.0.1:")) {
        unsigned int wh_pid = get_webhelper_pid();
        if (wh_pid > 0) {
            const char *port_str = strstr(command, "TCP@127.0.0.1:");
            if (port_str) {
                port_str += 14; /* skip "TCP@127.0.0.1:" */
                /* Build fake lsof -F output. Real lsof -F upnR output:
                 *   p<PID>\n R<PPID>\n u<UID>\n f<FD>\n n<local>-><remote>\n
                 * S32 queries -i TCP@127.0.0.1:<server_port>, wants to find
                 * the webhelper's connection. n field needs -> format.
                 * Use printf to emit lines with proper format. */
                char port_buf[8];
                int pi = 0;
                while (*port_str && *port_str != '"' && *port_str != '\''
                       && *port_str != ' ' && pi < 7)
                    port_buf[pi++] = *port_str++;
                port_buf[pi] = '\0';

                char fake_cmd[512];
                snprintf(fake_cmd, sizeof(fake_cmd),
                    "printf 'p%u\\nR1\\nu1000\\nf3\\n"
                    "n127.0.0.1:%s->127.0.0.1:%s\\n'",
                    wh_pid, port_buf, port_buf);


                static volatile int lsof_fake_count = 0;
                int cnt = __sync_fetch_and_add(&lsof_fake_count, 1);
                if (cnt < 20) {
                    debug_int("S32: popen lsof FAKED pid=", (long)wh_pid);
                    debug_str("S32: fake cmd: ", fake_cmd, "");
                }
                return real_popen_ptr(fake_cmd, type);
            }
        }
    }

    static volatile int popen_count = 0;
    int cnt = __sync_fetch_and_add(&popen_count, 1);
    if (cnt < 30)
        debug_str("S32: popen: ", command, "\n");
    return real_popen_ptr(command, type);
}

/* v133: execve wrapper — diagnostic to see if S32 execs lsof/ss */
typedef int (*real_execve_fn)(const char *, char *const[], char *const[]);
static real_execve_fn real_execve_ptr = NULL;

int execve(const char *pathname, char *const argv[], char *const envp[]) {
    if (!real_execve_ptr)
        real_execve_ptr = (real_execve_fn)dlsym(RTLD_NEXT, "execve");
    static volatile int execve_count = 0;
    int cnt = __sync_fetch_and_add(&execve_count, 1);
    if (cnt < 20)
        debug_str("S32: execve: ", pathname, "\n");
    return real_execve_ptr(pathname, argv, envp);
}

/* v133: getsockopt wrapper — fake SO_PEERCRED on TCP sockets.
 * WebUITransport calls getsockopt(accepted_fd, SOL_SOCKET, SO_PEERCRED, ...)
 * to determine the PID of the connecting WebSocket client.
 * On TCP sockets, SO_PEERCRED returns ENOPROTOOPT. We fake success
 * with the webhelper PID so S32 thinks the connection is from the webhelper. */
typedef int (*real_getsockopt_fn)(int, int, int, void *, socklen_t *);
static real_getsockopt_fn real_getsockopt_ptr = NULL;

/* SO_PEERCRED = 17 on Linux, struct ucred from sys/socket.h (with _GNU_SOURCE) */
#ifndef SO_PEERCRED
#define SO_PEERCRED 17
#endif

int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
    if (!real_getsockopt_ptr)
        real_getsockopt_ptr = (real_getsockopt_fn)dlsym(RTLD_NEXT, "getsockopt");
    int ret = real_getsockopt_ptr(sockfd, level, optname, optval, optlen);

    if (level == SOL_SOCKET && optname == SO_PEERCRED) {
        unsigned int wh_pid = get_webhelper_pid();
        if (ret == 0 && optval && optlen && *optlen >= sizeof(struct ucred)) {
            /* Success path: patch the returned PID */
            struct ucred *cred = (struct ucred *)optval;
            static volatile int peercred_log_count = 0;
            int cnt = __sync_fetch_and_add(&peercred_log_count, 1);
            if (cnt < 20) {
                debug_int("S32: SO_PEERCRED(ok) fd=", sockfd);
                debug_int("  pid=", (long)cred->pid);
            }
            if (wh_pid > 0 && cred->pid != (pid_t)wh_pid) {
                cred->pid = (pid_t)wh_pid;
                if (cnt < 20)
                    debug_int("  PATCHED to: ", (long)wh_pid);
            }
        } else if (ret < 0 && wh_pid > 0 && optval && optlen) {
            /* Failure path (TCP socket): fake success with webhelper PID */
            struct ucred *cred = (struct ucred *)optval;
            cred->pid = (pid_t)wh_pid;
            cred->uid = 1000;
            cred->gid = 1000;
            *optlen = sizeof(struct ucred);
            errno = 0;
            static volatile int peercred_fake_count = 0;
            int cnt = __sync_fetch_and_add(&peercred_fake_count, 1);
            if (cnt < 20) {
                debug_int("S32: SO_PEERCRED(faked) fd=", sockfd);
                debug_int("  pid=", (long)wh_pid);
            }
            ret = 0;
        }
    }
    return ret;
}

/* v133: Helper to write an integer as hex string (for /proc/net/tcp format) */
static int hex32(char *out, unsigned int val) {
    static const char hex[] = "0123456789ABCDEF";
    int n = 0;
    for (int i = 28; i >= 0; i -= 4) {
        int started = (n > 0 || ((val >> i) & 0xf) != 0 || i == 0);
        if (started) out[n++] = hex[(val >> i) & 0xf];
    }
    return n;
}

/* v133: Generate fake /proc/net/tcp content from live socket data.
 * Scans our own FDs to find TCP sockets and builds proper format. */
static int generate_proc_net_tcp(int is_ipv6) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    const char *header = "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n";
    int hlen = 0;
    while (header[hlen]) hlen++;
    write(pipefd[1], header, hlen);

    int sl = 0;
    for (int fd = 3; fd <= 300; fd++) {
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISSOCK(st.st_mode)) continue;

        struct sockaddr_in sin;
        socklen_t slen = sizeof(sin);
        if (getsockname(fd, (struct sockaddr *)&sin, &slen) != 0) continue;
        if ((!is_ipv6 && sin.sin_family != AF_INET) ||
            (is_ipv6 && sin.sin_family != AF_INET6)) continue;

        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int has_peer = (getpeername(fd, (struct sockaddr *)&peer, &plen) == 0);

        /* Build /proc/net/tcp line:
         * sl local_addr:port remote_addr:port st ... uid timeout inode */
        char line[256];
        int n = 0;
        /* sl field (right-justified, 4 chars) */
        line[n++] = ' '; line[n++] = ' '; line[n++] = ' ';
        n += hex32(line + n, sl);
        line[n++] = ':'; line[n++] = ' ';
        /* local_address:port (hex) — note: /proc/net/tcp uses host byte order! */
        {
            unsigned int addr = sin.sin_addr.s_addr; /* already network order */
            for (int i = 0; i < 8; i++)
                line[n++] = "0123456789ABCDEF"[(addr >> (i * 4)) & 0xf];
            line[n++] = ':';
            unsigned int port = ntohs(sin.sin_port);
            line[n++] = "0123456789ABCDEF"[(port >> 12) & 0xf];
            line[n++] = "0123456789ABCDEF"[(port >> 8) & 0xf];
            line[n++] = "0123456789ABCDEF"[(port >> 4) & 0xf];
            line[n++] = "0123456789ABCDEF"[port & 0xf];
        }
        line[n++] = ' ';
        /* remote_address:port */
        {
            unsigned int addr = has_peer ? peer.sin_addr.s_addr : 0;
            for (int i = 0; i < 8; i++)
                line[n++] = "0123456789ABCDEF"[(addr >> (i * 4)) & 0xf];
            line[n++] = ':';
            unsigned int port = has_peer ? ntohs(peer.sin_port) : 0;
            line[n++] = "0123456789ABCDEF"[(port >> 12) & 0xf];
            line[n++] = "0123456789ABCDEF"[(port >> 8) & 0xf];
            line[n++] = "0123456789ABCDEF"[(port >> 4) & 0xf];
            line[n++] = "0123456789ABCDEF"[port & 0xf];
        }
        /* st (state) — 01=ESTABLISHED, 0A=LISTEN */
        line[n++] = ' ';
        line[n++] = '0';
        line[n++] = has_peer ? '1' : 'A';
        /* tx_queue:rx_queue */
        line[n++] = ' ';
        { const char *z = "00000000:00000000"; while (*z) line[n++] = *z++; }
        /* tr:tm->when */
        line[n++] = ' ';
        { const char *z = "00:00000000"; while (*z) line[n++] = *z++; }
        /* retrnsmt */
        line[n++] = ' ';
        { const char *z = "00000000"; while (*z) line[n++] = *z++; }
        /* uid */
        line[n++] = ' ';
        { const char *z = "  1000"; while (*z) line[n++] = *z++; }
        /* timeout */
        line[n++] = ' ';
        line[n++] = '0';
        /* inode — use stat st_ino */
        line[n++] = ' ';
        {
            unsigned long ino = (unsigned long)st.st_ino;
            char inobuf[16];
            int inopos = 0;
            if (ino == 0) { inobuf[inopos++] = '0'; }
            else {
                char tmp[16]; int tmplen = 0;
                while (ino > 0) { tmp[tmplen++] = '0' + (ino % 10); ino /= 10; }
                for (int i = tmplen - 1; i >= 0; i--) inobuf[inopos++] = tmp[i];
            }
            for (int i = 0; i < inopos; i++) line[n++] = inobuf[i];
        }
        line[n++] = '\n';
        write(pipefd[1], line, n);
        sl++;
    }
    close(pipefd[1]);
    debug_int("S32: generated /proc/net/tcp with entries=", sl);
    return pipefd[0];
}

/* v133: Helper to build "/proc/<pid>/" prefix string */
static int build_proc_pid_prefix(char *buf, int bufsz, unsigned int pid) {
    const char *p = "/proc/";
    int n = 0;
    while (*p && n < bufsz - 1) buf[n++] = *p++;
    char pidbuf[12];
    int pidlen = 0;
    unsigned int tmp = pid;
    if (tmp == 0) { pidbuf[pidlen++] = '0'; }
    else {
        char rev[12]; int revlen = 0;
        while (tmp > 0) { rev[revlen++] = '0' + (tmp % 10); tmp /= 10; }
        for (int i = revlen - 1; i >= 0; i--) pidbuf[pidlen++] = rev[i];
    }
    for (int i = 0; i < pidlen && n < bufsz - 1; i++) buf[n++] = pidbuf[i];
    if (n < bufsz - 1) buf[n++] = '/';
    buf[n] = '\0';
    return n;
}

/* open/open64 wrapper: redirect /dev/shm/ paths to our Android directory. */
int open(const char *pathname, int flags, ...) {
    if (!real_open_ptr)
        real_open_ptr = (real_open_fn)dlsym(RTLD_NEXT, "open");
    va_list ap;
    va_start(ap, flags);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE))
        mode = va_arg(ap, mode_t);
    va_end(ap);

    /* v133: Intercept /proc/net/tcp — generate fake content from live socket data.
     * S32 reads this to find the PID of the WebSocket client.
     * On Android, /proc/net/tcp is restricted (SELinux) → returns empty → PID=0. */
    if (pathname && (strcmp(pathname, "/proc/net/tcp") == 0 ||
                     strcmp(pathname, "/proc/self/net/tcp") == 0)) {
        debug_str("S32: PROC-NET-TCP intercepted: ", pathname, "\n");
        int fd = generate_proc_net_tcp(0);
        if (fd >= 0) return fd;
        /* fallthrough to real open if generation fails */
    }
    if (pathname && (strcmp(pathname, "/proc/net/tcp6") == 0 ||
                     strcmp(pathname, "/proc/self/net/tcp6") == 0)) {
        debug_str("S32: PROC-NET-TCP6 intercepted: ", pathname, "\n");
        int fd = generate_proc_net_tcp(1);
        if (fd >= 0) return fd;
    }

    /* v133: Redirect /proc/<WH_PID>/xxx → /proc/self/xxx */
    {
        unsigned int wh_pid = get_webhelper_pid();
        if (wh_pid > 0 && pathname && strncmp(pathname, "/proc/", 6) == 0) {
            char prefix[32];
            int prefixlen = build_proc_pid_prefix(prefix, sizeof(prefix), wh_pid);
            if (strncmp(pathname, prefix, prefixlen) == 0) {
                char redir[256];
                int n = 0;
                const char *s = "/proc/self/";
                while (*s && n < 240) redir[n++] = *s++;
                s = pathname + prefixlen;
                while (*s && n < 254) redir[n++] = *s++;
                redir[n] = '\0';
                static volatile int proc_redir_count = 0;
                int cnt = __sync_fetch_and_add(&proc_redir_count, 1);
                if (cnt < 10)
                    debug_str("S32: open /proc/<WH_PID> redirect: ", redir, "\n");
                return real_open_ptr(redir, flags, mode);
            }
        }
    }

    /* v127: Log ALL opens that touch MasterStream or shmem paths */
    if (pathname && (strstr(pathname, "steam_chrome") || strstr(pathname, "chrome_shmem"))) {
        debug_str("S32: open(shmem-related): ", pathname, "\n");
        debug_int("  flags=", flags);
    }
    if (pathname && strstr(pathname, "MasterStream")) {
        debug_str("S32-v129: open() CAUGHT MasterStream: ", pathname, "\n");
        debug_int("  flags=", flags);
    }

    char redir[256];
    if (build_devshm_redir(pathname, redir, sizeof(redir))) {
        /* v119: MasterStream — fill + dual-write to /dev/shm/ overlay */
        if (strstr(pathname, "SteamChrome_MasterStream") != NULL) {
            int msfd = real_open_ptr(redir, O_CREAT | O_RDWR, 0666);
            if (msfd >= 0) {
                add_ms_fd(msfd);
                struct stat mst;
                int cnt = __sync_fetch_and_add(&ms_open_count, 1);
                if (cnt < 20) debug_str("S32: MS open: ", redir, "\n");

                unsigned char buf[8192];
                memset(buf, 0, sizeof(buf));
                unsigned int *h = (unsigned int *)buf;
                /* v127: hdr[0] = m_cubBuffer! */
                h[0]=8192; h[1]=0; h[2]=10; h[3]=10;
                unsigned int mt2=0, msz2=2;
                memcpy(buf+16,&mt2,4); memcpy(buf+20,&msz2,4);
                buf[24]=0x08; buf[25]=0x01;

                if (fstat(msfd, &mst) == 0) {
                    if (cnt < 20) debug_int("  size=", (long)mst.st_size);
                    if (mst.st_size < 8192) {
                        debug_int("S32: MS open: FILLING, was=", (long)mst.st_size);
                        ftruncate(msfd, 8192);
                        lseek(msfd, 0, SEEK_SET);
                        write(msfd, buf, 8192);
                        lseek(msfd, 0, SEEK_SET);
                        debug_msg("S32: MS open: filled OK\n");
                    }
                }

                /* v119: dual-write to /dev/shm/ overlay */
                {
                    int ofd = (int)syscall(SYS_openat, AT_FDCWD, pathname,
                                           O_CREAT | O_RDWR | O_TRUNC, 0666);
                    if (ofd >= 0) {
                        syscall(SYS_ftruncate, ofd, (long)8192);
                        syscall(SYS_write, ofd, buf, (long)8192);
                        close(ofd);
                    }
                }
            }
            return msfd;
        }

        /* v129: Detect MasterStream Connect() via open("/dev/shm/u1000-Shm_HASH", O_RDONLY).
         * On real PC, Connect() may use open() instead of shm_open().
         * Only MasterStream uses O_RDONLY for Shm_ files. */
        if (strstr(pathname, "Shm_") && (flags & O_ACCMODE) == O_RDONLY && !(flags & O_CREAT)) {
            int mc = __sync_fetch_and_add(&ms_connect_detect_count, 1);
            if (mc < 200)
                debug_str("S32-v129: open() O_RDONLY Shm_: ", pathname, "\n");

            /* Try opening existing at redir path */
            int msfd = real_open_ptr(redir, O_RDWR | O_CLOEXEC, 0666);
            if (msfd >= 0) {
                struct stat mst;
                if (fstat(msfd, &mst) == 0 && mst.st_size >= 8208) {
                    if (mc < 200)
                        debug_str("S32-v129: MS open found: ", pathname, "\n");
                    return msfd;
                }
                close(msfd);
            }

            /* Create as MasterStream — 8208 bytes */
            msfd = real_open_ptr(redir, O_CREAT | O_RDWR | O_CLOEXEC, 0666);
            if (msfd >= 0) {
                /* v130: Write proper 76-byte CMsgBrowserReady (PC format) */
                unsigned char ms_data[8208];
                memset(ms_data, 0, sizeof(ms_data));
                write_browserready_ring(ms_data, sizeof(ms_data));
                ftruncate(msfd, 8208);
                pwrite(msfd, ms_data, sizeof(ms_data), 0);
                if (mc < 200) {
                    debug_str("S32-v130: MS open() CREATE: ", pathname, "\n");
                    debug_int("  fd=", msfd);
                }
                /* Also write to /dev/shm/ via raw syscall */
                {
                    int ofd = (int)syscall(SYS_openat, AT_FDCWD, pathname,
                                           O_CREAT | O_RDWR | O_TRUNC, 0666);
                    if (ofd >= 0) {
                        syscall(SYS_ftruncate, ofd, (long)8208);
                        syscall(SYS_write, ofd, ms_data, (long)8208);
                        close(ofd);
                    }
                }
                return msfd;
            }
        }

        int real_flags = (flags & ~O_EXCL) | O_CREAT | O_RDWR;
        if (!mode) mode = 0666;
        int fd = real_open_ptr(redir, real_flags, mode);
        int count = __sync_fetch_and_add(&devshm_open_count, 1);
        if (count < 30) {
            debug_str("S32: open /dev/shm → ", redir, "\n");
            debug_int("  fd=", fd);
        }
        return fd;
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

/* v105: openat/openat64 wrapper — glibc shm_open may use openat internally.
 * The MasterStream goes through openat(fd_to_devshm, name, O_EXCL) which
 * bypasses our open() wrapper. Catch it here. */
typedef int (*real_openat_fn)(int, const char *, int, ...);
static real_openat_fn real_openat_ptr = NULL;
static volatile int openat_shm_count = 0;

int openat(int dirfd, const char *pathname, int flags, ...) {
    if (!real_openat_ptr)
        real_openat_ptr = (real_openat_fn)dlsym(RTLD_NEXT, "openat");
    va_list ap;
    va_start(ap, flags);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE))
        mode = va_arg(ap, mode_t);
    va_end(ap);

    /* v133: Intercept /proc/net/tcp in openat — same as open() */
    if (dirfd == AT_FDCWD && pathname) {
        if (strcmp(pathname, "/proc/net/tcp") == 0 || strcmp(pathname, "/proc/self/net/tcp") == 0) {
            debug_str("S32: openat PROC-NET-TCP: ", pathname, "\n");
            int fd = generate_proc_net_tcp(0);
            if (fd >= 0) return fd;
        }
        if (strcmp(pathname, "/proc/net/tcp6") == 0 || strcmp(pathname, "/proc/self/net/tcp6") == 0) {
            int fd = generate_proc_net_tcp(1);
            if (fd >= 0) return fd;
        }
        /* v133: Redirect /proc/<WH_PID>/xxx → /proc/self/xxx */
        if (strncmp(pathname, "/proc/", 6) == 0) {
            unsigned int wh_pid = get_webhelper_pid();
            if (wh_pid > 0) {
                char prefix[32];
                int prefixlen = build_proc_pid_prefix(prefix, sizeof(prefix), wh_pid);
                if (strncmp(pathname, prefix, prefixlen) == 0) {
                    char redir[256];
                    int n = 0;
                    const char *s = "/proc/self/";
                    while (*s && n < 240) redir[n++] = *s++;
                    s = pathname + prefixlen;
                    while (*s && n < 254) redir[n++] = *s++;
                    redir[n] = '\0';
                    return real_openat_ptr(AT_FDCWD, redir, flags, mode);
                }
            }
        }
    }

    /* v127: log ALL openat for MasterStream */
    if (pathname && strstr(pathname, "MasterStream") != NULL) {
        debug_str("S32-v129: openat() CAUGHT MasterStream: ", pathname, "\n");
        debug_int("  dirfd=", dirfd);
        debug_int("  flags=", flags);
    }
    /* Detect MasterStream via openat on /dev/shm dir fd */
    if (pathname && strstr(pathname, "SteamChrome_MasterStream") != NULL) {
        if (!real_open_ptr)
            real_open_ptr = (real_open_fn)dlsym(RTLD_NEXT, "open");

        /* Build full /dev/shm/ redirect path */
        char redir[256];
        int n = 0;
        const char *d = SHM_REDIR_DIR;
        while (*d && n < 250) redir[n++] = *d++;
        redir[n++] = '/';
        const char *s = pathname;
        while (*s && n < 255) redir[n++] = *s++;
        redir[n] = '\0';

        /* v119: Fill + dual-write to /dev/shm/ overlay */
        int msfd = real_open_ptr(redir, O_CREAT | O_RDWR, 0666);
        if (msfd >= 0) {
            add_ms_fd(msfd);
            struct stat mst;
            int cnt = __sync_fetch_and_add(&ms_open_count, 1);
            if (cnt < 20) debug_str("S32: MS openat: ", pathname, "\n");

            unsigned char buf[8192];
            memset(buf, 0, sizeof(buf));
            unsigned int *h = (unsigned int *)buf;
            /* v127: hdr[0] = m_cubBuffer! */
            h[0]=8192; h[1]=0; h[2]=10; h[3]=10;
            unsigned int mt2=0, msz2=2;
            memcpy(buf+16,&mt2,4); memcpy(buf+20,&msz2,4);
            buf[24]=0x08; buf[25]=0x01;

            if (fstat(msfd, &mst) == 0) {
                if (cnt < 20) debug_int("  size=", (long)mst.st_size);
                if (mst.st_size < 8192) {
                    debug_int("S32: MS openat: FILLING, was=", (long)mst.st_size);
                    ftruncate(msfd, 8192);
                    lseek(msfd, 0, SEEK_SET);
                    write(msfd, buf, 8192);
                    lseek(msfd, 0, SEEK_SET);
                    debug_msg("S32: MS openat: filled OK\n");
                }
            }

            /* v119: dual-write to /dev/shm/ overlay */
            {
                char devshm_oa[256];
                int n = 0;
                const char *d = "/dev/shm/";
                while (*d) devshm_oa[n++] = *d++;
                const char *s = pathname;
                while (*s && n < 250) devshm_oa[n++] = *s++;
                devshm_oa[n] = '\0';
                int ofd = (int)syscall(SYS_openat, AT_FDCWD, devshm_oa,
                                       O_CREAT | O_RDWR | O_TRUNC, 0666);
                if (ofd >= 0) {
                    syscall(SYS_ftruncate, ofd, (long)8192);
                    syscall(SYS_write, ofd, buf, (long)8192);
                    close(ofd);
                }
            }
        } else {
            debug_str("S32: MS openat FAIL: ", pathname, "\n");
        }
        return msfd;
    }

    /* Log other /dev/shm openat calls */
    if (pathname && strstr(pathname, "Shm_") != NULL) {
        int cnt = __sync_fetch_and_add(&openat_shm_count, 1);
        if (cnt < 10) {
            debug_str("S32: openat Shm_: ", pathname, "\n");
            debug_int("  dirfd=", dirfd);
            debug_int("  flags=", flags);
        }
    }

    return real_openat_ptr(dirfd, pathname, flags, mode);
}

int openat64(int dirfd, const char *pathname, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE))
        mode = va_arg(ap, mode_t);
    va_end(ap);
    return openat(dirfd, pathname, flags, mode);
}

/* stat/lstat wrappers: fake S_IFSOCK for steam_chrome_shmem paths.
 * The 64-bit webhelper binds to abstract sockets (no filesystem entry)
 * and creates a dummy REGULAR file at the path. But the steam client
 * stat()s the path and checks S_ISSOCK() before connecting. If it
 * sees S_IFREG, it skips the connect. Fake S_IFSOCK to fix this.
 *
 * On i386 glibc, stat() goes through __xstat() and lstat() through __lxstat().
 * We wrap all variants to catch every code path. */

static int is_shmem_path(const char *path) {
    if (!path) return 0;
    return (strstr(path, "steam_chrome_shmem") != NULL);
}

typedef int (*real_xstat_fn)(int, const char *, struct stat *);
static real_xstat_fn real_xstat_ptr = NULL;
typedef int (*real_lxstat_fn)(int, const char *, struct stat *);
static real_lxstat_fn real_lxstat_ptr = NULL;

static volatile int stat_fake_count = 0;

int __xstat(int ver, const char *path, struct stat *buf) {
    if (!real_xstat_ptr)
        real_xstat_ptr = (real_xstat_fn)dlsym(RTLD_NEXT, "__xstat");
    int ret = real_xstat_ptr(ver, path, buf);

    if (is_shmem_path(path)) {
        if (ret == 0 && S_ISREG(buf->st_mode)) {
            /* File exists as regular → fake S_IFSOCK */
            buf->st_mode = (buf->st_mode & ~S_IFMT) | S_IFSOCK;
        } else if (ret != 0) {
            /* v103: File doesn't exist in 32-bit overlay (it's in 64-bit overlay).
             * Fake a successful stat with S_IFSOCK so the client tries to connect.
             * Our connect wrapper converts the filesystem path to abstract socket. */
            memset(buf, 0, sizeof(*buf));
            buf->st_mode = S_IFSOCK | 0777;
            buf->st_nlink = 1;
            ret = 0;
        }
        int n = __sync_fetch_and_add(&stat_fake_count, 1);
        if (n < 20) {
            debug_str("S32: stat→S_IFSOCK: ", path, "\n");
        }
    }
    return ret;
}

int __lxstat(int ver, const char *path, struct stat *buf) {
    if (!real_lxstat_ptr)
        real_lxstat_ptr = (real_lxstat_fn)dlsym(RTLD_NEXT, "__lxstat");
    int ret = real_lxstat_ptr(ver, path, buf);

    if (is_shmem_path(path)) {
        if (ret == 0 && S_ISREG(buf->st_mode)) {
            buf->st_mode = (buf->st_mode & ~S_IFMT) | S_IFSOCK;
        } else if (ret != 0) {
            /* v103: fake for non-existent file too */
            memset(buf, 0, sizeof(*buf));
            buf->st_mode = S_IFSOCK | 0777;
            buf->st_nlink = 1;
            ret = 0;
        }
        int n = __sync_fetch_and_add(&stat_fake_count, 1);
        if (n < 20) {
            debug_str("S32: lstat→S_IFSOCK: ", path, "\n");
        }
    }
    return ret;
}

/* Also wrap stat/lstat directly in case glibc uses them */
int stat(const char *path, struct stat *buf) {
    return __xstat(3, path, buf);
}

int lstat(const char *path, struct stat *buf) {
    return __lxstat(3, path, buf);
}

/* v116: 64-bit stat variants — Steam client may use stat64/lstat64
 * which map to __xstat64/__lxstat64 on 32-bit glibc.
 * Without these, the S_IFSOCK fake never triggers and the client
 * never discovers the chrome_shmem socket. */
typedef int (*real_xstat64_fn)(int, const char *, struct stat64 *);
static real_xstat64_fn real_xstat64_ptr = NULL;
typedef int (*real_lxstat64_fn)(int, const char *, struct stat64 *);
static real_lxstat64_fn real_lxstat64_ptr = NULL;

int __xstat64(int ver, const char *path, struct stat64 *buf) {
    if (!real_xstat64_ptr)
        real_xstat64_ptr = (real_xstat64_fn)dlsym(RTLD_NEXT, "__xstat64");
    int ret = real_xstat64_ptr(ver, path, buf);

    if (is_shmem_path(path)) {
        if (ret == 0 && S_ISREG(buf->st_mode)) {
            buf->st_mode = (buf->st_mode & ~S_IFMT) | S_IFSOCK;
        } else if (ret != 0) {
            memset(buf, 0, sizeof(*buf));
            buf->st_mode = S_IFSOCK | 0777;
            buf->st_nlink = 1;
            ret = 0;
        }
        int n = __sync_fetch_and_add(&stat_fake_count, 1);
        if (n < 30) {
            debug_str("S32: stat64→S_IFSOCK: ", path, "\n");
        }
    }
    return ret;
}

int __lxstat64(int ver, const char *path, struct stat64 *buf) {
    if (!real_lxstat64_ptr)
        real_lxstat64_ptr = (real_lxstat64_fn)dlsym(RTLD_NEXT, "__lxstat64");
    int ret = real_lxstat64_ptr(ver, path, buf);

    if (is_shmem_path(path)) {
        if (ret == 0 && S_ISREG(buf->st_mode)) {
            buf->st_mode = (buf->st_mode & ~S_IFMT) | S_IFSOCK;
        } else if (ret != 0) {
            memset(buf, 0, sizeof(*buf));
            buf->st_mode = S_IFSOCK | 0777;
            buf->st_nlink = 1;
            ret = 0;
        }
        int n = __sync_fetch_and_add(&stat_fake_count, 1);
        if (n < 30) {
            debug_str("S32: lstat64→S_IFSOCK: ", path, "\n");
        }
    }
    return ret;
}

int stat64(const char *path, struct stat64 *buf) {
    return __xstat64(3, path, buf);
}

int lstat64(const char *path, struct stat64 *buf) {
    return __lxstat64(3, path, buf);
}

/* access() wrapper: if the file exists, succeed for shmem paths */
typedef int (*real_access_fn)(const char *, int);
static real_access_fn real_access_ptr = NULL;

int access(const char *path, int mode) {
    if (!real_access_ptr)
        real_access_ptr = (real_access_fn)dlsym(RTLD_NEXT, "access");
    int ret = real_access_ptr(path, mode);
    /* v103: fake success for steam_chrome_shmem even if file doesn't exist */
    if (is_shmem_path(path)) {
        if (ret != 0) ret = 0; /* force success */
        int n = __sync_fetch_and_add(&stat_fake_count, 1);
        if (n < 20) {
            debug_str("S32: access OK (faked): ", path, "\n");
        }
    }
    return ret;
}

/* Forward declarations for write/read wrappers (used by fd11 thread) */
typedef ssize_t (*real_write32_fn)(int, const void *, size_t);
static real_write32_fn real_write32_ptr = NULL;
typedef ssize_t (*real_read32_fn)(int, void *, size_t);
static real_read32_fn real_read32_ptr = NULL;

/* socketpair wrapper: log when steam client creates IPC sockets */
typedef int (*real_socketpair_fn)(int, int, int, int[2]);
static real_socketpair_fn real_socketpair_ptr = NULL;
static volatile int socketpair_count = 0;

/* v103: FD 11 bridge — saved dup of the child's end (sv[0]) of the IPC
 * socketpair. We write to this fd to simulate the webhelper's responses.
 * Data written here appears when the parent reads from sv[1] (fd 12). */
static int ipc_child_fd = -1;
static volatile int fd11_ready_done = 0;

/* v103: Thread that fakes the webhelper's sdPC handshake on the IPC socket.
 *
 * Protocol (from webhelper's perspective, which we simulate):
 *   1. Read parent's handshake messages (drain pending data)
 *   2. Write 56-byte sdPC response: "sdPC" + version=2 + type=1
 *   3. Write 1-byte status: '1' (ready)
 *
 * We write to ipc_child_fd (= dup of sv[0]). The parent reads from
 * sv[1] (fd 12) and thinks the webhelper responded. */
static void *fd11_fake_ready_func(void *arg) {
    (void)arg;
    int fd = ipc_child_fd;
    debug_int("S32: fd11_fake: starting on child fd=", fd);

    if (!real_write32_ptr)
        real_write32_ptr = (real_write32_fn)dlsym(RTLD_NEXT, "write");
    if (!real_read32_ptr)
        real_read32_ptr = (real_read32_fn)dlsym(RTLD_NEXT, "read");

    /* v103c: The socketpair (fd 11/12) is shared between the child-update-ui
     * process and (later) the webhelper. Our fd 200 is a dup of the same socket
     * endpoint as fd 11.
     *
     * CRITICAL: We must NOT read from fd 200 during the child-update-ui phase,
     * or we'll steal its messages and corrupt the protocol. Instead:
     * 1. Sleep 3 seconds (child-update-ui finishes well within this time)
     * 2. Drain any leftover data (non-blocking, don't echo back)
     * 3. Write sdPC ready signal (what the webhelper would send)
     *
     * The parent reads from fd 12 and sees our sdPC as the webhelper's response.
     * The 60-second timeout for "Failed connecting" gives us plenty of room. */

    /* v105: Reduced from 3s to 500ms. The first socketpair (child-update-ui)
     * gets closed quickly. We need to write before EPIPE. If this is the
     * wrong socketpair, the thread just EPIPE's and exits harmlessly. */
    debug_msg("S32: fd11_fake: sleeping 500ms...\n");
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000000 };
    nanosleep(&ts, NULL);

    /* Drain any pending data the parent wrote for the webhelper phase.
     * Do NOT echo — just consume so the stream is clean for our sdPC. */
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    unsigned char rbuf[512];
    int total_drained = 0;
    for (int i = 0; i < 10; i++) {
        ssize_t r = real_read32_ptr(fd, rbuf, sizeof(rbuf));
        if (r > 0) {
            debug_int("S32: fd11_fake: drained len=", (long)r);
            debug_hexline("  data: ", rbuf, r > 40 ? 40 : (int)r);
            total_drained += (int)r;
        } else {
            break;
        }
    }
    fcntl(fd, F_SETFL, flags); /* restore blocking */
    debug_int("S32: fd11_fake: total drained=", total_drained);

    /* Write sdPC ready response (56 bytes).
     * This is what the real webhelper writes to fd 11 after Chromium init. */
    unsigned char ready_msg[56];
    memset(ready_msg, 0, sizeof(ready_msg));
    ready_msg[0] = 's'; ready_msg[1] = 'd';
    ready_msg[2] = 'P'; ready_msg[3] = 'C';
    ready_msg[4] = 0x02; /* version = 2 */
    ready_msg[8] = 0x01; /* type = 1 (echo/ready) */

    ssize_t n = real_write32_ptr(fd, ready_msg, 56);
    debug_int("S32: fd11_fake: wrote sdPC ret=", (long)n);
    if (n < 0) debug_int("  errno=", errno);

    /* Write status byte '1' (ready) */
    if (n == 56) {
        char one = '1';
        n = real_write32_ptr(fd, &one, 1);
        debug_int("S32: fd11_fake: wrote '1' ret=", (long)n);
        if (n < 0) debug_int("  errno=", errno);
    }

    if (n == 56 || n == 1) {
        fd11_ready_done = 1;
        debug_msg("S32: fd11_fake: DONE — ready signal sent\n");
    } else {
        debug_msg("S32: fd11_fake: EPIPE — wrong socketpair, will retry on next\n");
    }
    return NULL;
}

int socketpair(int domain, int type, int protocol, int sv[2]) {
    if (!real_socketpair_ptr)
        real_socketpair_ptr = (real_socketpair_fn)dlsym(RTLD_NEXT, "socketpair");
    int ret = real_socketpair_ptr(domain, type, protocol, sv);
    int n = __sync_fetch_and_add(&socketpair_count, 1);
    if (n < 20) {
        debug_int("S32: socketpair fd0=", sv[0]);
        debug_int("  fd1=", sv[1]);
    }

    /* v105: Log all socketpairs but do NOT write fake sdPC.
     * socketpair #0 is for child-update-ui (writing corrupts it → crash).
     * The webhelper uses steam_chrome_shmem socket, NOT a socketpair. */
    if (ret == 0 && domain == AF_UNIX) {
        debug_int("S32: socketpair #", (long)n);
        debug_int("  sv[0]=", sv[0]);
        debug_int("  sv[1]=", sv[1]);
    }

    return ret;
}

/* write wrapper: monitor writes that look like IPC to webhelper */
static volatile int write_ipc_count = 0;

ssize_t write(int fd, const void *buf, size_t count) {
    if (!real_write32_ptr)
        real_write32_ptr = (real_write32_fn)dlsym(RTLD_NEXT, "write");
    ssize_t ret = real_write32_ptr(fd, buf, count);
    /* Log writes that look like sdPC IPC messages */
    if (ret > 0 && count >= 4 && buf) {
        const unsigned char *b = (const unsigned char *)buf;
        if (b[0] == 's' && b[1] == 'd' && b[2] == 'P' && b[3] == 'C') {
            int n = __sync_fetch_and_add(&write_ipc_count, 1);
            if (n < 30) {
                debug_int("S32: write sdPC fd=", fd);
                debug_int("  count=", (long)count);
                debug_int("  ret=", (long)ret);
            }
        }
    }
    return ret;
}

/* read wrapper: monitor reads of sdPC messages */
static volatile int read_ipc_count = 0;

ssize_t read(int fd, void *buf, size_t count) {
    if (!real_read32_ptr)
        real_read32_ptr = (real_read32_fn)dlsym(RTLD_NEXT, "read");
    ssize_t ret = real_read32_ptr(fd, buf, count);
    if (ret >= 4 && buf) {
        const unsigned char *b = (const unsigned char *)buf;
        /* Log HTTP GET requests (WebSocket upgrade) */
        if (b[0] == 'G' && b[1] == 'E' && b[2] == 'T' && b[3] == ' ') {
            debug_int("S32: read HTTP GET fd=", fd);
            debug_int("  ret=", (long)ret);
            int show = (int)(ret > 300 ? 300 : ret);
            char tbuf[304];
            for (int i = 0; i < show; i++)
                tbuf[i] = (b[i] >= 32 && b[i] < 127) ? b[i] : '|';
            tbuf[show] = '\n';
            tbuf[show+1] = '\0';
            debug_msg(tbuf);
        }
        /* Log sdPC IPC messages */
        else if (b[0] == 's' && b[1] == 'd' && b[2] == 'P' && b[3] == 'C') {
            int n = __sync_fetch_and_add(&read_ipc_count, 1);
            if (n < 30) {
                debug_int("S32: read sdPC fd=", fd);
                debug_int("  count=", (long)count);
                debug_int("  ret=", (long)ret);
                debug_hexline("  data: ", buf, ret > 40 ? 40 : (int)ret);
            }
        }
    }
    return ret;
}

/* v110: recvmsg wrapper — detect SCM_RIGHTS received by client (e.g. MasterStream fd) */
typedef ssize_t (*real_recvmsg_fn)(int, struct msghdr *, int);
static real_recvmsg_fn real_recvmsg_ptr = NULL;
static volatile int recvmsg_count = 0;

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
    if (!real_recvmsg_ptr)
        real_recvmsg_ptr = (real_recvmsg_fn)dlsym(RTLD_NEXT, "recvmsg");
    ssize_t ret = real_recvmsg_ptr(sockfd, msg, flags);
    if (ret >= 0 && msg && msg->msg_controllen > 0) {
        struct cmsghdr *cmsg;
        for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                int received_fd = -1;
                memcpy(&received_fd, CMSG_DATA(cmsg), sizeof(int));
                int n = __sync_fetch_and_add(&recvmsg_count, 1);
                if (n < 20) {
                    debug_int("S32: recvmsg SCM_RIGHTS fd=", received_fd);
                    debug_int("  sockfd=", sockfd);
                    debug_int("  datalen=", (long)ret);
                    /* v118: DON'T blindly tag as MasterStream!
                     * Use readlink to check what the fd actually points to.
                     * Previous bug: fd=96 was a Shm_ IPC channel, not MasterStream.
                     * Injecting CMsgBrowserReady into it caused "Invalid command". */
                    {
                        char fdpath[64], target[256];
                        int fi = 0;
                        const char *pp = "/proc/self/fd/";
                        while (*pp) fdpath[fi++] = *pp++;
                        int fv = received_fd;
                        char fb[16]; int fn2 = 0;
                        if (fv == 0) fb[fn2++] = '0';
                        else while (fv > 0) { fb[fn2++] = '0' + (fv % 10); fv /= 10; }
                        for (int j = fn2 - 1; j >= 0; j--) fdpath[fi++] = fb[j];
                        fdpath[fi] = '\0';
                        int tl = readlink(fdpath, target, sizeof(target) - 1);
                        if (tl > 0) {
                            target[tl] = '\0';
                            debug_str("S32:   fd→", target, "\n");
                            if (strstr(target, "MasterStream")) {
                                add_ms_fd(received_fd);
                                debug_msg("S32:   TAGGED as MasterStream\n");
                            }
                        }
                    }
                }
            }
        }
    }
    return ret;
}

/* v111: sendmsg wrapper — log what client sends (especially on chrome_shmem socket) */
typedef ssize_t (*real_sendmsg_fn)(int, const struct msghdr *, int);
static real_sendmsg_fn real_sendmsg_ptr = NULL;
static volatile int sendmsg_count = 0;

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    if (!real_sendmsg_ptr)
        real_sendmsg_ptr = (real_sendmsg_fn)dlsym(RTLD_NEXT, "sendmsg");
    ssize_t ret = real_sendmsg_ptr(sockfd, msg, flags);
    int n = __sync_fetch_and_add(&sendmsg_count, 1);
    if (n < 30 && msg) {
        debug_int("S32: sendmsg fd=", sockfd);
        debug_int("  ret=", (long)ret);
        if (msg->msg_iovlen > 0 && msg->msg_iov && msg->msg_iov[0].iov_len > 0) {
            int datalen = (int)msg->msg_iov[0].iov_len;
            if (datalen > 40) datalen = 40;
            debug_int("  iovlen=", (long)msg->msg_iov[0].iov_len);
            debug_hexline("  data: ", msg->msg_iov[0].iov_base, datalen);
        }
        /* Check for SCM_RIGHTS */
        if (msg->msg_controllen > 0) {
            struct cmsghdr *cmsg;
            for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR((struct msghdr *)msg, cmsg)) {
                if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                    int sent_fd = -1;
                    memcpy(&sent_fd, CMSG_DATA(cmsg), sizeof(int));
                    debug_int("  SCM_RIGHTS sent_fd=", sent_fd);
                }
            }
        }
    }
    return ret;
}

/* v111: send() wrapper — log data client sends on sockets */
typedef ssize_t (*real_send_fn)(int, const void *, size_t, int);
static real_send_fn real_send_ptr = NULL;
static volatile int send_count = 0;

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    if (!real_send_ptr)
        real_send_ptr = (real_send_fn)dlsym(RTLD_NEXT, "send");
    ssize_t ret = real_send_ptr(sockfd, buf, len, flags);
    int n = __sync_fetch_and_add(&send_count, 1);
    if (n < 50) {
        debug_int("S32: send fd=", sockfd);
        debug_int("  len=", (long)len);
        debug_int("  ret=", (long)ret);
        if (buf && len > 0) {
            int show = (int)(len > 40 ? 40 : len);
            debug_hexline("  data: ", buf, show);
        }
    }
    return ret;
}

/* v111: recv() wrapper — log data client receives on sockets */
typedef ssize_t (*real_recv_fn)(int, void *, size_t, int);
static real_recv_fn real_recv_ptr = NULL;
static volatile int recv_count = 0;

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    if (!real_recv_ptr)
        real_recv_ptr = (real_recv_fn)dlsym(RTLD_NEXT, "recv");
    ssize_t ret = real_recv_ptr(sockfd, buf, len, flags);
    int n = __sync_fetch_and_add(&recv_count, 1);
    if (ret > 0) {
        /* Log ALL recv to find WebSocket incoming requests.
         * Check if data starts with "GET " (HTTP request) */
        const unsigned char *d = (const unsigned char *)buf;
        if (ret >= 4 && d[0] == 'G' && d[1] == 'E' && d[2] == 'T' && d[3] == ' ') {
            debug_int("S32: recv HTTP GET fd=", sockfd);
            debug_int("  ret=", (long)ret);
            /* Print as text (up to 200 chars) */
            int show = (int)(ret > 200 ? 200 : ret);
            char tbuf[204];
            for (int i = 0; i < show; i++)
                tbuf[i] = (d[i] >= 32 && d[i] < 127) ? d[i] : '.';
            tbuf[show] = '\n';
            tbuf[show+1] = '\0';
            debug_msg(tbuf);
        } else if (n < 50) {
            debug_int("S32: recv fd=", sockfd);
            debug_int("  len=", (long)len);
            debug_int("  ret=", (long)ret);
            int show = (int)(ret > 40 ? 40 : ret);
            debug_hexline("  data: ", buf, show);
        }
    }
    return ret;
}

/* accept/accept4 wrappers: log when client accepts connections */
typedef int (*real_accept_fn)(int, struct sockaddr *, socklen_t *);
typedef int (*real_accept4_fn)(int, struct sockaddr *, socklen_t *, int);
static real_accept_fn real_accept_ptr = NULL;
static real_accept4_fn real_accept4_ptr = NULL;

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    if (!real_accept_ptr)
        real_accept_ptr = (real_accept_fn)dlsym(RTLD_NEXT, "accept");
    int ret = real_accept_ptr(sockfd, addr, addrlen);
    if (ret >= 0) {
        debug_int("S32: accept fd=", sockfd);
        debug_int("  new_fd=", ret);
    }
    return ret;
}

int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
    if (!real_accept4_ptr)
        real_accept4_ptr = (real_accept4_fn)dlsym(RTLD_NEXT, "accept4");
    int ret = real_accept4_ptr(sockfd, addr, addrlen, flags);
    if (ret >= 0) {
        debug_int("S32: accept4 fd=", sockfd);
        debug_int("  new_fd=", ret);
    }
    return ret;
}

/* ============================================================
 * getsockopt wrapper: intercept SO_PEERCRED to diagnose & fix
 * WebUITransport PID check ("Checked: %d/%d").
 *
 * On FEX/Android, SO_PEERCRED on a TCP loopback socket may return
 * the FEX host PID instead of the guest PID. S32 compares
 * peercred.pid against the expected webhelper PID → mismatch → reject.
 *
 * Fix: if SO_PEERCRED returns a PID that doesn't match the expected
 * webhelper PID, substitute the expected PID (read from
 * /tmp/.steam_webhelper_pid written by steamwebhelper.sh).
 * ============================================================ */
/* v133: getsockopt() moved to before open() — handles SO_PEERCRED faking */

/* ============================================================
 * SHM BRIDGE LISTENER: Receives Shm_ file data from 64-bit side
 * via abstract socket and writes to our (32-bit) overlay.
 *
 * Protocol (from 64-bit bridge):
 *   [2 bytes] name_len (LE)
 *   [name_len bytes] shm name (e.g. "/u1000-Shm_abc123")
 *   [4 bytes] data_len (LE)
 *   [data_len bytes] file content
 * ============================================================ */
#define BRIDGE_SOCK_NAME "\0shm_bridge_64to32"
#define BRIDGE_SOCK_NAME_LEN 19  /* includes leading \0 */

static volatile int bridge_recv_count = 0;

/* Helper: read exactly n bytes from fd */
static int read_exact(int fd, void *buf, int n) {
    int total = 0;
    while (total < n) {
        ssize_t r = recv(fd, (char *)buf + total, n - total, 0);
        if (r <= 0) return -1;
        total += (int)r;
    }
    return 0;
}

static void *shm_bridge_listener(void *arg) {
    (void)arg;
    debug_msg("S32: BRIDGE: starting listener thread\n");

    /* Create listening abstract socket */
    int listenfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenfd < 0) {
        debug_msg("S32: BRIDGE: socket() failed\n");
        return NULL;
    }

    struct sockaddr_un baddr;
    memset(&baddr, 0, sizeof(baddr));
    baddr.sun_family = AF_UNIX;
    memcpy(baddr.sun_path, BRIDGE_SOCK_NAME, BRIDGE_SOCK_NAME_LEN);

    /* Use real bind (not our wrapper — abstract socket already) */
    if (!real_bind_ptr)
        real_bind_ptr = (real_bind_fn)dlsym(RTLD_NEXT, "bind");
    int bret = real_bind_ptr(listenfd, (struct sockaddr *)&baddr,
                             offsetof(struct sockaddr_un, sun_path) + BRIDGE_SOCK_NAME_LEN);
    if (bret < 0) {
        debug_int("S32: BRIDGE: bind failed errno=", errno);
        close(listenfd);
        return NULL;
    }

    if (listen(listenfd, 5) < 0) {
        debug_msg("S32: BRIDGE: listen failed\n");
        close(listenfd);
        return NULL;
    }

    debug_msg("S32: BRIDGE: listening for 64-bit Shm_ data\n");

    /* Accept loop — each connection sends one Shm_ file update */
    while (1) {
        int cfd = accept(listenfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            debug_int("S32: BRIDGE: accept error errno=", errno);
            break;
        }

        /* Read name_len (2 bytes LE) */
        unsigned char hdr[6];
        if (read_exact(cfd, hdr, 2) < 0) { close(cfd); continue; }
        int name_len = hdr[0] | (hdr[1] << 8);
        if (name_len <= 0 || name_len > 63) { close(cfd); continue; }

        /* Read name */
        char name[64];
        memset(name, 0, sizeof(name));
        if (read_exact(cfd, name, name_len) < 0) { close(cfd); continue; }

        /* Read data_len (4 bytes LE) */
        if (read_exact(cfd, hdr, 4) < 0) { close(cfd); continue; }
        int data_len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | (hdr[3] << 24);
        if (data_len <= 0 || data_len > 16384) { close(cfd); continue; }

        /* Read data */
        unsigned char *data = (unsigned char *)__builtin_alloca(data_len);
        if (read_exact(cfd, data, data_len) < 0) { close(cfd); continue; }
        close(cfd);

        /* Write to our local Shm_ file */
        char path[256];
        build_shm_path(path, sizeof(path), name);

        /* Open/create the file, truncate, write new content */
        if (!real_open_ptr)
            real_open_ptr = (real_open_fn)dlsym(RTLD_NEXT, "open");
        int wfd = real_open_ptr(path, O_CREAT | O_RDWR | O_TRUNC, 0666);
        if (wfd >= 0) {
            int written = 0;
            while (written < data_len) {
                ssize_t w = write(wfd, data + written, data_len - written);
                if (w <= 0) break;
                written += (int)w;
            }
            ftruncate(wfd, data_len);
            close(wfd);

            int cnt = __sync_fetch_and_add(&bridge_recv_count, 1);
            if (cnt < 20) {
                debug_str("S32: BRIDGE: updated Shm_: ", name, "\n");
                debug_int("  size=", data_len);
                debug_int("  total_recv=", cnt + 1);
            }
        } else {
            debug_str("S32: BRIDGE: open failed for: ", path, "\n");
            debug_int("  errno=", errno);
        }
    }

    close(listenfd);
    debug_msg("S32: BRIDGE: listener exiting\n");
    return NULL;
}

/* ============================================================
 * v101b: mmap/mmap64/munmap interceptor — singleton anonymous mapping.
 *
 * v101 proved anonymous mappings work (header accepted, mmap burst),
 * but each mmap created a FRESH mapping → client reads m_cubData=10,
 * consumes, munmaps, re-mmaps, gets fresh m_cubData=10 → infinite
 * loop → client destroys the stream after ~2s.
 *
 * v101b fix: return the SAME singleton mapping every time. Client's
 * writes persist across re-mmap calls. munmap is intercepted to keep
 * the mapping alive. Client reads header, processes ring data, writes
 * m_cubData=0 → next re-mmap sees the same buffer with m_cubData=0
 * → no infinite loop.
 * ============================================================ */
typedef void *(*real_mmap_fn)(void *, size_t, int, int, int, off_t);
static real_mmap_fn real_mmap_ptr = NULL;

typedef void *(*real_mmap64_fn)(void *, size_t, int, int, int, __off64_t);
static real_mmap64_fn real_mmap64_ptr = NULL;

typedef int (*real_munmap_fn)(void *, size_t);
static real_munmap_fn real_munmap_ptr = NULL;

static volatile int mmap_intercept_count = 0;

/* Singleton anonymous mapping for the bridge Shm_ */
static void *bridge_anon_map = NULL;
static size_t bridge_anon_size = 0;

/* v130: FIXED SHMemStream header + CMsgBrowserReady format.
 * From PC IPC trace, CORRECT layout:
 *   hdr[0] = get   (read cursor, 0-based into ring buffer)
 *   hdr[1] = put   (write cursor, 0-based into ring buffer)
 *   hdr[2] = m_cubBuffer (ring buffer capacity)
 *   hdr[3] = pending (bytes available = put - get)
 *   Ring buffer data starts at file/mapping offset 16.
 *
 * CMsgBrowserReady is 76 bytes (not 10!):
 *   u32(1) + u32(1) + u32(PID) + char[64](stream_name) */
static void fill_shm_header(void *mapping, size_t length) {
    if (length < 92) return; /* need 16 hdr + 76 msg */
    memset(mapping, 0, length < 256 ? length : 256);
    write_browserready_ring((unsigned char *)mapping, length);
}

/* Return the singleton bridge mapping (create + fill ONCE on first call) */
static void *get_bridge_mapping(size_t length) {
    if (!bridge_anon_map) {
        if (!real_mmap_ptr)
            real_mmap_ptr = (real_mmap_fn)dlsym(RTLD_NEXT, "mmap");

        void *anon = real_mmap_ptr(NULL, length, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (anon == MAP_FAILED) return anon;
        memset(anon, 0, length);
        bridge_anon_map = anon;
        bridge_anon_size = length;

        /* v102: fill header + CMsgBrowserReady once.
         * After client reads and Clear()s, the ring stays empty (get=put=0, pending=0).
         * That's correct — client got our BrowserReady, now waits for more. */
        fill_shm_header(bridge_anon_map, bridge_anon_size);
        debug_int("S32: created ANON mapping + CMsgBrowserReady, size=", (long)length);
        debug_hexline("  hdr: ", bridge_anon_map, 32);
    }

    return bridge_anon_map;
}

/* v110: ftruncate wrapper — MOVED to v116 section above (near ms_poller) */

/* v117: fstat wrapper — DETECT and FIX MasterStream fds with size=0.
 * CSharedMemStream::Connect() opens MasterStream via unknown path (bypasses
 * all our shm_open/open/openat wrappers), then fstats it and asserts size.
 * We catch it here: if fstat returns size=0, readlink the fd to check if
 * it's a MasterStream, and if so, fill it with CMsgBrowserReady data. */
/* v122: Forward-declare syscall wrapper's function pointer for use in fix_ms_fd */
typedef long (*real_syscall_fn)(long, ...);
static real_syscall_fn real_syscall_ptr = NULL;

/* v124: Forward-declare is_ms_fd_verified.
 * Uses readlink on /proc/self/fd/<fd> to check if fd points to MasterStream.
 * Handles fd reuse: verifies current target, not stale ms_fd[] entry. */
static int is_ms_fd_verified(int fd) {
    if (fd < 3) return 0;
    char fdpath[32], target[256];
    {
        char *p = fdpath;
        const char *pfx = "/proc/self/fd/";
        while (*pfx) *p++ = *pfx++;
        int tmp = fd, d = 1;
        while (tmp >= 10) { d *= 10; tmp /= 10; }
        tmp = fd;
        while (d > 0) { *p++ = '0' + (tmp / d); tmp %= d; d /= 10; }
        *p = '\0';
    }
    int len = readlink(fdpath, target, sizeof(target)-1);
    if (len > 0) {
        target[len] = '\0';
        return strstr(target, "MasterStream") != NULL;
    }
    return 0;
}

typedef int (*real_fstat_fn)(int, struct stat *);
static real_fstat_fn real_fstat_ptr = NULL;
static volatile int fstat_ms_count = 0;
static volatile int fstat_fix_count = 0;

static int is_ms_path(const char *path) {
    return path && strstr(path, "MasterStream") != NULL;
}

static void fix_ms_fd(int fd) {
    /* Fill the fd with CMsgBrowserReady data (same as ms_poller_buf).
     * v122: Use real_syscall_ptr to avoid recursion through our syscall() wrapper.
     * File MUST be exactly 8192 bytes — Connect() asserts fstat.st_size == 8192. */
    int cnt = __sync_fetch_and_add(&fstat_fix_count, 1);
    if (cnt < 10) {
        debug_int("S32: FSTAT-FIX: filling MS fd=", fd);
    }
    if (real_syscall_ptr) {
        real_syscall_ptr(SYS_ftruncate, (long)fd, (long)8192, 0, 0, 0, 0);
        lseek(fd, 0, SEEK_SET);
        real_syscall_ptr(SYS_write, (long)fd, (long)ms_poller_buf, (long)8192, 0, 0, 0);
        lseek(fd, 0, SEEK_SET);
    } else {
        /* Fallback — this may recurse through our wrapper but better than nothing */
        syscall(SYS_ftruncate, fd, (long)8192);
        lseek(fd, 0, SEEK_SET);
        syscall(SYS_write, fd, ms_poller_buf, (long)8192);
        lseek(fd, 0, SEEK_SET);
    }
}

int __fxstat(int ver, int fd, struct stat *buf) {
    /* __fxstat is glibc's internal fstat implementation on 32-bit */
    typedef int (*real_fxstat_fn)(int, int, struct stat *);
    static real_fxstat_fn real_fxstat_ptr = NULL;
    if (!real_fxstat_ptr)
        real_fxstat_ptr = (real_fxstat_fn)dlsym(RTLD_NEXT, "__fxstat");
    int ret = real_fxstat_ptr(ver, fd, buf);
    /* v124: For MasterStream fds, ALWAYS override st_size = 8192.
     * KEY FIX: Don't re-fstat after fix_ms_fd — fix_ms_fd fails on read-only
     * fds (Connect() opens O_RDONLY), so re-fstat would still return 0.
     * Instead, set buf->st_size directly. Mmap wrapper handles physical size.
     * No S_ISREG or ms_poller_active guards — be maximally aggressive. */
    if (ret == 0 && fd >= 3 && buf->st_size != 8192) {
        int is_ms = is_ms_fd(fd);
        if (!is_ms) is_ms = is_ms_fd_verified(fd);
        if (is_ms) {
            int cnt = __sync_fetch_and_add(&fstat_fix_count, 1);
            if (cnt < 50) {
                debug_int("S32: v124 FXSTAT-OVERRIDE fd=", fd);
                debug_int("  real_size=", (long)buf->st_size);
            }
            if (!is_ms_fd(fd)) add_ms_fd(fd);
            buf->st_size = 8192;  /* THE FIX: override directly */
            fix_ms_fd(fd);  /* best effort — may fail on RO fd, that's OK */
        }
    }
    return ret;
}

/* v124: __fxstat64 — same direct override as __fxstat */
int __fxstat64(int ver, int fd, struct stat64 *buf) {
    typedef int (*real_fxstat64_fn)(int, int, struct stat64 *);
    static real_fxstat64_fn real_fxstat64_ptr = NULL;
    if (!real_fxstat64_ptr)
        real_fxstat64_ptr = (real_fxstat64_fn)dlsym(RTLD_NEXT, "__fxstat64");
    int ret = real_fxstat64_ptr(ver, fd, buf);
    /* v124: Same direct override — don't re-fstat, just set st_size = 8192 */
    if (ret == 0 && fd >= 3 && buf->st_size != 8192) {
        int is_ms = is_ms_fd(fd);
        if (!is_ms) is_ms = is_ms_fd_verified(fd);
        if (is_ms) {
            int cnt = __sync_fetch_and_add(&fstat_fix_count, 1);
            if (cnt < 50) {
                debug_int("S32: v124 FXSTAT64-OVERRIDE fd=", fd);
                debug_int("  real_size=", (long)buf->st_size);
            }
            if (!is_ms_fd(fd)) add_ms_fd(fd);
            buf->st_size = 8192;
            fix_ms_fd(fd);  /* best effort */
        }
    }
    return ret;
}

/* v124: __fxstatat64 — catches glibc 2.33+ internal fstat path.
 * On glibc 2.35, fstat() may internally use fstatat64(fd, "", buf, AT_EMPTY_PATH)
 * instead of the old __fxstat64 path. Intercept this to override st_size. */
int __fxstatat64(int ver, int dirfd, const char *pathname,
                 struct stat64 *buf, int flags) {
    typedef int (*real_fn)(int, int, const char *, struct stat64 *, int);
    static real_fn real_ptr = NULL;
    if (!real_ptr)
        real_ptr = (real_fn)dlsym(RTLD_NEXT, "__fxstatat64");
    int ret = real_ptr(ver, dirfd, pathname, buf, flags);
    /* AT_EMPTY_PATH with empty pathname = fstat-like (dirfd is the target) */
    if (ret == 0 && (flags & AT_EMPTY_PATH) && pathname && pathname[0] == '\0'
        && dirfd >= 3 && buf->st_size != 8192) {
        int is_ms = is_ms_fd(dirfd);
        if (!is_ms) is_ms = is_ms_fd_verified(dirfd);
        if (is_ms) {
            int cnt = __sync_fetch_and_add(&fstat_fix_count, 1);
            if (cnt < 50) {
                debug_int("S32: v124 FXSTATAT64-OVERRIDE fd=", dirfd);
                debug_int("  real_size=", (long)buf->st_size);
            }
            if (!is_ms_fd(dirfd)) add_ms_fd(dirfd);
            buf->st_size = 8192;
            fix_ms_fd(dirfd);
        }
    }
    return ret;
}

/* v124: fstatat64 — catches the new GLIBC_2.33 direct fstatat64 symbol */
int fstatat64(int dirfd, const char *pathname, struct stat64 *buf, int flags) {
    typedef int (*real_fn)(int, const char *, struct stat64 *, int);
    static real_fn real_ptr = NULL;
    if (!real_ptr)
        real_ptr = (real_fn)dlsym(RTLD_NEXT, "fstatat64");
    int ret = real_ptr(dirfd, pathname, buf, flags);
    if (ret == 0 && (flags & AT_EMPTY_PATH) && pathname && pathname[0] == '\0'
        && dirfd >= 3 && buf->st_size != 8192) {
        int is_ms = is_ms_fd(dirfd);
        if (!is_ms) is_ms = is_ms_fd_verified(dirfd);
        if (is_ms) {
            int cnt = __sync_fetch_and_add(&fstat_fix_count, 1);
            if (cnt < 50) {
                debug_int("S32: v124 FSTATAT64-OVERRIDE fd=", dirfd);
                debug_int("  real_size=", (long)buf->st_size);
            }
            if (!is_ms_fd(dirfd)) add_ms_fd(dirfd);
            buf->st_size = 8192;
            fix_ms_fd(dirfd);
        }
    }
    return ret;
}

/* v117: syscall() wrapper REMOVED — too invasive, causes early exit.
 * Connect() bypasses all glibc wrappers. Since the IPC TCP handshake
 * actually WORKS (IPC-LOOP runs with messages), the real issue may be
 * that chrome_ipc_client's MasterStream timeout fires before our
 * CMsgBrowserReady injection. Try a different approach below. */

/* v117: MasterStream injection — inject INLINE in mmap wrapper, not in a
 * separate thread. The client reads the ring buffer immediately after mmap
 * returns, so any delay (even 10ms) is too late. We pre-fill the ring buffer
 * with CMsgBrowserReady BEFORE returning from mmap(). The client's init code
 * will then set capacity — but we also set it, so the client sees data
 * immediately. We also start a background thread to RE-INJECT if the client
 * clears the ring buffer during its own initialization. */
static volatile void *ms_mmap_ptr = NULL;
static volatile size_t ms_mmap_len = 0;
static volatile int ms_inject_done = 0;

static void inject_browserready(void *m) {
    /* v130: Write proper 76-byte CMsgBrowserReady (PC format) */
    write_browserready_ring((unsigned char *)m, 8208);
    __sync_synchronize();
}

static void *ms_reinject_thread(void *arg) {
    (void)arg;
    /* Re-inject if the client clears the ring during its init.
     * Poll for up to 5 seconds, re-injecting whenever put gets reset to 0. */
    volatile unsigned int *hdr = (volatile unsigned int *)ms_mmap_ptr;
    for (int i = 0; i < 500 && ms_mmap_ptr; i++) {
        usleep(10000); /* 10ms */
        /* v127: hdr[0]=m_cubBuffer, hdr[1]=get, hdr[2]=put, hdr[3]=pending */
        unsigned int cubBuf = hdr[0];
        unsigned int put = hdr[2];
        if (cubBuf > 0 && put == 0) {
            /* Client cleared our data — re-inject */
            debug_int("S32: MS-REINJECT: cubBuf=", (long)cubBuf);
            inject_browserready((void *)ms_mmap_ptr);
            debug_msg("S32: MS-REINJECT: re-injected CMsgBrowserReady\n");
        }
        if (put >= 10 && cubBuf > 0) {
            /* Data is there and client initialized — we're done */
            if (!ms_inject_done) {
                ms_inject_done = 1;
                debug_msg("S32: MS-INJECT: confirmed in ring buffer\n");
            }
            break;
        }
    }
    return NULL;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if (!real_mmap_ptr)
        real_mmap_ptr = (real_mmap_fn)dlsym(RTLD_NEXT, "mmap");

    /* v118: Verify fd points to MasterStream via readlink BEFORE injecting.
     * Previous bug: SCM_RIGHTS fds were blindly tagged as MasterStream,
     * causing CMsgBrowserReady injection into Shm_ IPC channels → "Invalid command". */
    if (fd >= 0 && is_ms_fd(fd)) {
        /* Double-check with readlink */
        int is_real_ms = 0;
        {
            char fdpath[64], target[256];
            int fi = 0;
            const char *pp = "/proc/self/fd/";
            while (*pp) fdpath[fi++] = *pp++;
            int fv = fd;
            char fb[16]; int fn2 = 0;
            if (fv == 0) fb[fn2++] = '0';
            else while (fv > 0) { fb[fn2++] = '0' + (fv % 10); fv /= 10; }
            for (int j = fn2 - 1; j >= 0; j--) fdpath[fi++] = fb[j];
            fdpath[fi] = '\0';
            int tl = readlink(fdpath, target, sizeof(target) - 1);
            if (tl > 0) {
                target[tl] = '\0';
                if (strstr(target, "MasterStream"))
                    is_real_ms = 1;
                else {
                    static int misid = 0;
                    if (misid++ < 5)
                        debug_str("S32: mmap NOT MasterStream fd→", target, "\n");
                }
            }
        }
        if (is_real_ms) {
            debug_int("S32: mmap MasterStream fd=", fd);
            debug_int("  len=", (long)length);
            debug_int("  prot=", prot);
            debug_int("  flags=", flags);

            /* v124: Ensure file is physically >= length bytes before mmap.
             * If fd is read-only and file is 0 bytes, mmap succeeds but
             * accessing the mapping triggers SIGBUS. Fix by opening the
             * file ourselves via readlink path with O_RDWR and extending. */
            off_t msz = lseek(fd, 0, SEEK_END);
            lseek(fd, 0, SEEK_SET);
            if (msz >= 0 && msz < (off_t)length) {
                debug_int("S32: mmap MS: file too small, size=", (long)msz);
                /* Try ftruncate on the fd directly first */
                int fixed = 0;
                if (real_syscall_ptr) {
                    long tr = real_syscall_ptr(SYS_ftruncate, (long)fd, (long)length, 0, 0, 0, 0);
                    if (tr == 0) {
                        lseek(fd, 0, SEEK_SET);
                        real_syscall_ptr(SYS_write, (long)fd, (long)ms_poller_buf, (long)8192, 0, 0, 0);
                        lseek(fd, 0, SEEK_SET);
                        fixed = 1;
                        debug_msg("S32: mmap MS: fixed via fd ftruncate\n");
                    }
                }
                /* If ftruncate failed (read-only fd), open via readlink path */
                if (!fixed) {
                    char fdp[64], tgt[256];
                    int fi2 = 0;
                    const char *pp2 = "/proc/self/fd/";
                    while (*pp2) fdp[fi2++] = *pp2++;
                    int fv2 = fd;
                    char fb2[16]; int fn3 = 0;
                    if (fv2 == 0) fb2[fn3++] = '0';
                    else while (fv2 > 0) { fb2[fn3++] = '0' + (fv2 % 10); fv2 /= 10; }
                    for (int j = fn3 - 1; j >= 0; j--) fdp[fi2++] = fb2[j];
                    fdp[fi2] = '\0';
                    int tlen = readlink(fdp, tgt, sizeof(tgt) - 1);
                    if (tlen > 0) {
                        tgt[tlen] = '\0';
                        if (!real_open_ptr)
                            real_open_ptr = (real_open_fn)dlsym(RTLD_NEXT, "open");
                        int wfd = real_open_ptr(tgt, O_RDWR, 0);
                        if (wfd >= 0) {
                            if (real_syscall_ptr)
                                real_syscall_ptr(SYS_ftruncate, (long)wfd, (long)length, 0, 0, 0, 0);
                            lseek(wfd, 0, SEEK_SET);
                            write(wfd, ms_poller_buf, 8192);
                            close(wfd);
                            debug_msg("S32: mmap MS: fixed via readlink reopen\n");
                        } else {
                            debug_int("S32: mmap MS: readlink reopen FAIL errno=", errno);
                        }
                    }
                }
            }
        }
        void *m = real_mmap_ptr(addr, length, prot, flags, fd, offset);
        if (m != MAP_FAILED && is_real_ms) {
            debug_msg("S32: mmap MasterStream OK\n");
            /* v117: inject CMsgBrowserReady INLINE before returning to client.
             * The client reads the ring buffer immediately after mmap returns. */
            if (!ms_mmap_ptr) {
                ms_mmap_ptr = m;
                ms_mmap_len = length;
                inject_browserready(m);
                debug_msg("S32: MS-INJECT: CMsgBrowserReady INJECTED (inline)!\n");
                /* Start background re-inject thread in case client clears it */
                pthread_t tid;
                pthread_create(&tid, NULL, ms_reinject_thread, NULL);
                pthread_detach(tid);
            }
        } else if (m == MAP_FAILED && is_real_ms) {
            debug_int("S32: mmap MasterStream FAIL errno=", errno);
        }
        return m;
    }

    if (fd >= 0 && offset == 0 && length >= 16 && is_bridge_dup_fd(fd)) {
        int n = __sync_fetch_and_add(&mmap_intercept_count, 1);
        if (n < 20 || (n % 50) == 0) {
            debug_int("S32: mmap→ANON fd=", fd);
            debug_int("  count=", n + 1);
            if (bridge_anon_map)
                debug_hexline("  current hdr: ", bridge_anon_map, 16);
        }
        void *m = get_bridge_mapping(length);
        return m;
    }

    return real_mmap_ptr(addr, length, prot, flags, fd, offset);
}

void *mmap64(void *addr, size_t length, int prot, int flags, int fd, __off64_t offset) {
    if (!real_mmap64_ptr)
        real_mmap64_ptr = (real_mmap64_fn)dlsym(RTLD_NEXT, "mmap64");

    /* v118: readlink-verified MasterStream mmap64 */
    if (fd >= 0 && is_ms_fd(fd)) {
        int is_real_ms = 0;
        {
            char fdpath[64], target[256];
            int fi = 0;
            const char *pp = "/proc/self/fd/";
            while (*pp) fdpath[fi++] = *pp++;
            int fv = fd;
            char fb[16]; int fn2 = 0;
            if (fv == 0) fb[fn2++] = '0';
            else while (fv > 0) { fb[fn2++] = '0' + (fv % 10); fv /= 10; }
            for (int j = fn2 - 1; j >= 0; j--) fdpath[fi++] = fb[j];
            fdpath[fi] = '\0';
            int tl = readlink(fdpath, target, sizeof(target) - 1);
            if (tl > 0) { target[tl] = '\0'; if (strstr(target, "MasterStream")) is_real_ms = 1; }
        }
        if (is_real_ms) debug_int("S32: mmap64 MasterStream fd=", fd);
        void *m = real_mmap64_ptr(addr, length, prot, flags, fd, offset);
        if (m != MAP_FAILED && is_real_ms)
            debug_hexline("  data: ", m, 32);
        return m;
    }

    if (fd >= 0 && offset == 0 && length >= 16 && is_bridge_dup_fd(fd)) {
        int n = __sync_fetch_and_add(&mmap_intercept_count, 1);
        if (n < 20 || (n % 50) == 0) {
            debug_int("S32: mmap64→ANON fd=", fd);
            if (bridge_anon_map)
                debug_hexline("  current hdr: ", bridge_anon_map, 16);
        }
        void *m = get_bridge_mapping(length);
        return m;
    }

    return real_mmap64_ptr(addr, length, prot, flags, fd, offset);
}

/* munmap interceptor: block unmap of our singleton */
int munmap(void *addr, size_t length) {
    if (!real_munmap_ptr)
        real_munmap_ptr = (real_munmap_fn)dlsym(RTLD_NEXT, "munmap");

    if (addr == bridge_anon_map && bridge_anon_map != NULL) {
        /* Don't actually unmap — keep the singleton alive.
         * Log what the client wrote, then DON'T re-fill here
         * (re-fill happens in the next mmap call). */
        int n = __sync_fetch_and_add(&mmap_intercept_count, 0);
        if (n < 30 || (n % 50) == 0) {
            debug_msg("S32: munmap→BLOCKED\n");
            debug_hexline("  client left: ", addr, 16);
        }
        return 0;  /* pretend success */
    }

    return real_munmap_ptr(addr, length);
}

/* v103: Create the steam_chrome_shmem dummy file in the 32-bit overlay.
 * The webhelper creates this file in the 64-bit overlay (invisible to us).
 * We create it here so stat/access in the 32-bit client finds it.
 * The actual IPC goes through the abstract socket (same kernel namespace). */
static void create_shmem_dummy_file(void) {
    char path[256];
    char pidbuf[20];
    long pid = (long)getpid();

    /* Build: /tmp/steam_chrome_shmem_uid1000_spid<PID> */
    int n = 0;
    const char *prefix = "/tmp/steam_chrome_shmem_uid1000_spid";
    while (*prefix && n < 200) path[n++] = *prefix++;

    /* Append PID */
    int pn = 0;
    if (pid == 0) { pidbuf[pn++] = '0'; }
    else { long v = pid; while (v > 0) { pidbuf[pn++] = '0' + (v % 10); v /= 10; } }
    for (int i = pn - 1; i >= 0; i--) path[n++] = pidbuf[i];
    path[n] = '\0';

    if (!real_open_ptr)
        real_open_ptr = (real_open_fn)dlsym(RTLD_NEXT, "open");

    /* Create as regular file — our stat wrapper will fake S_IFSOCK */
    int fd = real_open_ptr(path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd >= 0) {
        close(fd);
        debug_str("S32: created shmem dummy: ", path, "\n");
    } else {
        debug_str("S32: shmem dummy FAILED: ", path, "\n");
        debug_int("  errno=", errno);
    }
}

/* v108: Create FD 11 from scratch + fake parent handshake.
 * The Java app launches "steam -child-update-ui -child-update-ui-socket 11"
 * directly — there is NO parent steam process, so FD 11 never existed.
 * Fix: create a socketpair, dup2 one end to FD 11, and on the other end
 * act as the "fake parent" that sends the sdPC handshake + reads ready. */
static volatile int fake_parent_fd = -1;

static void *fake_parent_thread(void *arg) {
    int pfd = (int)(long)arg;
    debug_int("S32: fake_parent: thread started, fd=", pfd);

    /* The child (steam -child-update-ui) expects to READ the sdPC handshake
     * from FD 11. We send it from our "parent" end.
     * sdPC handshake format: 56 bytes, starts with "sdPC", version=2.
     * Then a 1-byte status '0' (not ready yet). */
    unsigned char handshake[57];
    memset(handshake, 0, sizeof(handshake));
    handshake[0] = 's'; handshake[1] = 'd';
    handshake[2] = 'P'; handshake[3] = 'C';
    handshake[4] = 0x02; /* version = 2 */
    handshake[8] = 0x01; /* type = 1 (init) */
    /* PID of the "parent" — use our own PID */
    long pid = (long)getpid();
    memcpy(&handshake[12], &pid, 4);
    handshake[56] = '0'; /* status = not ready yet */

    ssize_t w = write(pfd, handshake, 57);
    debug_int("S32: fake_parent: wrote sdPC handshake ret=", (long)w);

    /* Wait 5s for the child to read the handshake, then try to read reply.
     * Use a short timeout since the child might not reply quickly. */
    usleep(5000000);  /* 5 seconds */

    /* Try a non-blocking read first to see if child replied */
    int oldflags = fcntl(pfd, F_GETFL);
    fcntl(pfd, F_SETFL, oldflags | O_NONBLOCK);
    unsigned char reply[128];
    ssize_t r = read(pfd, reply, sizeof(reply));
    fcntl(pfd, F_SETFL, oldflags);

    if (r > 0) {
        debug_int("S32: fake_parent: child replied, len=", (long)r);
    } else {
        debug_msg("S32: fake_parent: no reply from child (expected)\n");
    }

    /* Wait 20s more for the webhelper to initialize, then if no reply,
     * write the echo + ready ourselves to unblock the child-update-ui.
     * The webhelper's ready signal can't reach the child-update-ui because
     * FD 11 is lost during exec, so we fake the reply from the parent side. */
    debug_msg("S32: fake_parent: waiting 20s for webhelper init...\n");
    usleep(20000000);

    /* Check if child replied while we waited */
    fcntl(pfd, F_SETFL, oldflags | O_NONBLOCK);
    r = read(pfd, reply, sizeof(reply));
    fcntl(pfd, F_SETFL, oldflags);

    if (r > 0) {
        debug_int("S32: fake_parent: child replied late, len=", (long)r);
    } else {
        debug_msg("S32: fake_parent: injecting ready signal (status='1')\n");
        /* Send another sdPC message with status='1' to tell the
         * child-update-ui that the webhelper is ready. On a real PC the
         * parent relays this from the webhelper — we fake it directly. */
        unsigned char ready[57];
        memset(ready, 0, sizeof(ready));
        ready[0] = 's'; ready[1] = 'd';
        ready[2] = 'P'; ready[3] = 'C';
        ready[4] = 0x02; /* version = 2 */
        ready[8] = 0x01; /* type = 1 */
        memcpy(&ready[12], &pid, 4);
        ready[56] = '1'; /* status = READY */
        w = write(pfd, ready, 57);
        debug_int("S32: fake_parent: wrote ready signal ret=", (long)w);
    }

    /* Keep the socket open indefinitely */
    debug_msg("S32: fake_parent: keeping socket alive\n");
    while (1) {
        usleep(30000000); /* 30s */
        /* Drain any pending data */
        fcntl(pfd, F_SETFL, oldflags | O_NONBLOCK);
        while ((r = read(pfd, reply, sizeof(reply))) > 0) {
            debug_int("S32: fake_parent: got data len=", (long)r);
        }
        fcntl(pfd, F_SETFL, oldflags);
    }

    return NULL;
}

/* v121: Wrap glibc's syscall() to intercept raw SYS_openat for MasterStream.
 * Connect() bypasses all glibc wrappers. This catches it at the lowest
 * glibc level before the actual syscall instruction. On x86-32, syscalls
 * have at most 6 args, so we always extract 6. Only log SYS_openat when
 * the path contains "MasterStream". For SYS_openat with MasterStream,
 * also dual-write to both /dev/shm/ and SHM_REDIR_DIR.
 * NOTE: v117's syscall wrapper was removed for being "too invasive."
 * v122: Also intercepts SYS_fstat64 for MasterStream fds — fixes file size
 * on-the-fly so Connect()'s fstat assertion passes. */
/* real_syscall_fn and real_syscall_ptr declared above (before fix_ms_fd) */
static volatile int raw_ms_openat_count = 0;
static volatile int raw_ms_fstat_count = 0;
static volatile int raw_ms_ftruncate_count = 0;

/* v124: is_ms_fd_verified moved above __fxstat (before first use) */

long syscall(long number, ...) {
    if (!real_syscall_ptr)
        real_syscall_ptr = (real_syscall_fn)dlsym(RTLD_NEXT, "syscall");

    va_list ap;
    va_start(ap, number);
    long a1 = va_arg(ap, long);
    long a2 = va_arg(ap, long);
    long a3 = va_arg(ap, long);
    long a4 = va_arg(ap, long);
    long a5 = va_arg(ap, long);
    long a6 = va_arg(ap, long);
    va_end(ap);

    if (number == SYS_openat) {
        const char *path = (const char *)a2;

        /* v132: Intercept /proc/net/tcp opens for WebUITransport PID detection.
         * Steam reads /proc/net/tcp to find socket inodes, then scans
         * /proc/<pid>/fd/ to map inode→PID. On Android, /proc/net/tcp is
         * restricted by SELinux. Try /proc/self/net/tcp instead. */
        if (path) {
            static volatile int proc_open_count = 0;
            /* Log /proc/ opens for diagnostic (first 50) */
            if (strncmp(path, "/proc/", 6) == 0 &&
                strstr(path, "MasterStream") == NULL) {
                int cnt = __sync_fetch_and_add(&proc_open_count, 1);
                if (cnt < 50)
                    debug_str("S32: PROC-OPEN: ", path, "\n");
            }
            /* Redirect /proc/net/tcp → /proc/self/net/tcp */
            if (strcmp(path, "/proc/net/tcp") == 0 ||
                strcmp(path, "/proc/net/tcp6") == 0) {
                const char *self_path = (strcmp(path, "/proc/net/tcp") == 0)
                    ? "/proc/self/net/tcp" : "/proc/self/net/tcp6";
                long fd = real_syscall_ptr(SYS_openat, (long)AT_FDCWD,
                    (long)self_path, a3, a4, a5, a6);
                if (fd >= 0) {
                    debug_str("S32: PROC-NET-TCP redirect OK: ", self_path, "\n");
                    debug_int("  fd=", fd);
                    return fd;
                }
                debug_str("S32: PROC-NET-TCP redirect FAILED: ", self_path, "\n");
                /* Fallback: let original open proceed (will probably fail too) */
            }
        }

        if (path && strstr(path, "MasterStream")) {
            int cnt = __sync_fetch_and_add(&raw_ms_openat_count, 1);
            /* v124: Strip O_TRUNC from MasterStream opens.
             * Create() opens with O_CREAT|O_RDWR|O_TRUNC (578) which truncates
             * the file to 0 bytes. Our pre-filled 8192-byte file gets emptied.
             * Without O_TRUNC, Create() opens the existing file and overwrites. */
            long orig_flags = a3;
            if (a3 & O_TRUNC) {
                a3 &= ~((long)O_TRUNC);
                if (cnt < 200)
                    debug_int("S32: RAW-OPENAT: STRIPPED O_TRUNC, was=", orig_flags);
            }
            if (cnt < 200) {
                debug_str("S32: RAW-OPENAT MasterStream: ", path, "\n");
                debug_int("  dirfd=", a1);
                debug_int("  flags=", a3);
            }
            long ret = real_syscall_ptr(number, a1, a2, a3, a4, a5, a6);
            if (cnt < 200) {
                debug_int("  ret_fd=", ret);
                if (ret < 0) debug_int("  errno=", errno);
            }
            /* v122: Track MasterStream fds from raw openat */
            if (ret >= 0) {
                add_ms_fd((int)ret);
            }
            /* v122: fix file size to exactly 8192 if wrong */
            if (ret >= 0 && ms_poller_active) {
                off_t cur = lseek((int)ret, 0, SEEK_CUR);
                off_t sz = lseek((int)ret, 0, SEEK_END);
                if (cur >= 0) lseek((int)ret, cur, SEEK_SET);
                if (cnt < 200) debug_int("  fstat_size=", (long)sz);
                if (sz >= 0 && sz != 8192) {
                    if (cnt < 200)
                        debug_int("S32: RAW-OPENAT: FIXING MS size from ", (long)sz);
                    fix_ms_fd((int)ret);
                    if (cnt < 200) {
                        sz = lseek((int)ret, 0, SEEK_END);
                        lseek((int)ret, 0, SEEK_SET);
                        debug_int("  fstat_size=", (long)sz);
                    }
                }
            }
            return ret;
        }

        /* v129: Detect MasterStream Connect() via raw SYS_openat on /dev/shm/Shm_
         * with O_RDONLY. Connect() may bypass glibc entirely. */
        if (path && strstr(path, "/dev/shm/") && strstr(path, "Shm_") &&
            (a3 & O_ACCMODE) == O_RDONLY && !(a3 & O_CREAT)) {
            int mc = __sync_fetch_and_add(&ms_connect_detect_count, 1);
            if (mc < 200)
                debug_str("S32-v129: RAW-OPENAT O_RDONLY Shm_: ", path, "\n");

            /* Build redirect path */
            char redir[256];
            if (build_devshm_redir(path, redir, sizeof(redir))) {
                if (!real_open_ptr)
                    real_open_ptr = (real_open_fn)dlsym(RTLD_NEXT, "open");

                /* Try existing with correct size */
                int msfd = real_open_ptr(redir, O_RDWR | O_CLOEXEC, 0666);
                if (msfd >= 0) {
                    struct stat mst;
                    if (fstat(msfd, &mst) == 0 && mst.st_size >= 8208) {
                        if (mc < 200)
                            debug_str("S32-v129: MS RAW found: ", redir, "\n");
                        return (long)msfd;
                    }
                    close(msfd);
                }

                /* Create as MasterStream */
                msfd = real_open_ptr(redir, O_CREAT | O_RDWR | O_CLOEXEC, 0666);
                if (msfd >= 0) {
                    /* v130: Write proper 76-byte CMsgBrowserReady (PC format) */
                    unsigned char ms_data[8208];
                    memset(ms_data, 0, sizeof(ms_data));
                    write_browserready_ring(ms_data, sizeof(ms_data));
                    ftruncate(msfd, 8208);
                    pwrite(msfd, ms_data, sizeof(ms_data), 0);
                    if (mc < 200) {
                        debug_str("S32-v130: MS RAW CREATE: ", redir, "\n");
                        debug_int("  fd=", msfd);
                    }
                    /* Also write to /dev/shm/ */
                    {
                        int ofd = (int)real_syscall_ptr(SYS_openat, (long)AT_FDCWD, (long)path,
                                                        (long)(O_CREAT | O_RDWR | O_TRUNC), (long)0666, 0, 0);
                        if (ofd >= 0) {
                            real_syscall_ptr(SYS_ftruncate, (long)ofd, (long)8208, 0, 0, 0, 0);
                            real_syscall_ptr(SYS_write, (long)ofd, (long)ms_data, (long)8208, 0, 0, 0);
                            close(ofd);
                        }
                    }
                    return (long)msfd;
                }
            }
        }
    }

    /* v123: Intercept SYS_ftruncate for MasterStream fds.
     * BLOCK ftruncate to anything other than 8192. Create() likely does
     * ftruncate(fd, 0) to reset, which empties the file. Then maybe
     * ftruncate(fd, 8192) which we allow. For size=0, return 0 (fake success). */
    if ((number == SYS_ftruncate || number == SYS_ftruncate64) && (int)a1 >= 3 && ms_poller_active) {
        int fd = (int)a1;
        long new_size = a2;
        if (is_ms_fd(fd) && is_ms_fd_verified(fd)) {
            int cnt = __sync_fetch_and_add(&raw_ms_ftruncate_count, 1);
            if (cnt < 30) {
                debug_int("S32: RAW-FTRUNCATE MS fd=", fd);
                debug_int("  new_size=", new_size);
            }
            if (new_size == 8192) {
                /* Allow: this is the correct size */
                return real_syscall_ptr(number, a1, a2, a3, a4, a5, a6);
            } else {
                /* BLOCK: don't let anyone truncate MasterStream to wrong size */
                if (cnt < 30)
                    debug_int("S32: RAW-FTRUNCATE BLOCKED! wanted=", new_size);
                return 0; /* fake success */
            }
        }
    }

    /* v124: Intercept SYS_fstat64 for MasterStream fds.
     * Override st_size in the kernel stat64 struct directly.
     * On i386, struct stat64 has st_size at offset 44 (8-byte field). */
    if (number == SYS_fstat64 && (int)a1 >= 3) {
        int fd = (int)a1;
        long ret = real_syscall_ptr(number, a1, a2, a3, a4, a5, a6);
        if (ret == 0 && a2) {
            /* Read st_size from kernel stat64 struct (offset 44, 8 bytes) */
            long long *psize = (long long *)((char *)a2 + 44);
            if (*psize != 8192) {
                int is_ms = is_ms_fd(fd);
                if (!is_ms) is_ms = is_ms_fd_verified(fd);
                if (is_ms) {
                    int cnt = __sync_fetch_and_add(&raw_ms_fstat_count, 1);
                    if (cnt < 50) {
                        debug_int("S32: v124 RAW-FSTAT64-OVERRIDE fd=", fd);
                        debug_int("  real_size=", (long)*psize);
                    }
                    *psize = 8192;  /* Override directly in kernel buffer */
                    if (!is_ms_fd(fd)) add_ms_fd(fd);
                    fix_ms_fd(fd);  /* best effort */
                }
            }
        }
        return ret;
    }

    /* v124: Intercept SYS_fstatat64 (glibc 2.33+ internal fstat path).
     * fstat(fd) may internally be fstatat64(fd, "", buf, AT_EMPTY_PATH). */
    if (number == SYS_fstatat64 && (int)a1 >= 3) {
        const char *path = (const char *)a2;
        int flags = (int)a4;
        /* AT_EMPTY_PATH with empty path = fstat-like, dirfd is target fd */
        if ((flags & AT_EMPTY_PATH) && path && path[0] == '\0') {
            int fd = (int)a1;
            long ret = real_syscall_ptr(number, a1, a2, a3, a4, a5, a6);
            if (ret == 0 && a3) {
                long long *psize = (long long *)((char *)a3 + 44);
                if (*psize != 8192) {
                    int is_ms = is_ms_fd(fd);
                    if (!is_ms) is_ms = is_ms_fd_verified(fd);
                    if (is_ms) {
                        int cnt = __sync_fetch_and_add(&raw_ms_fstat_count, 1);
                        if (cnt < 50) {
                            debug_int("S32: v124 RAW-FSTATAT64-OVERRIDE fd=", fd);
                            debug_int("  real_size=", (long)*psize);
                        }
                        *psize = 8192;
                        if (!is_ms_fd(fd)) add_ms_fd(fd);
                        fix_ms_fd(fd);
                    }
                }
            }
            return ret;
        }
    }

    /* v132: Intercept SYS_socketcall for socket(), recvmsg(), getsockopt().
     * On i386, all socket operations go through SYS_socketcall (102).
     * Subcalls: 1=socket, 12=recvmsg, 15=getsockopt */
    if (number == 102) { /* SYS_socketcall */
        long subcall = a1;

        /* v132: Detect AF_NETLINK socket creation (subcall 1 = SYS_SOCKET).
         * Args: socket(domain, type, protocol)
         * AF_NETLINK=16, NETLINK_SOCK_DIAG=4, NETLINK_INET_DIAG=4 */
        if (subcall == 1) { /* SYS_SOCKET */
            unsigned long *args = (unsigned long *)a2;
            long ret = real_syscall_ptr(number, a1, a2, a3, a4, a5, a6);
            if (ret >= 0 && args) {
                int domain = (int)args[0];
                int type = (int)args[1];
                int protocol = (int)args[2];
                if (domain == 16) { /* AF_NETLINK */
                    static volatile int nl_count = 0;
                    int cnt = __sync_fetch_and_add(&nl_count, 1);
                    if (cnt < 10) {
                        debug_int("S32: NETLINK socket created fd=", ret);
                        debug_int("  type=", (long)type);
                        debug_int("  protocol=", (long)protocol);
                    }
                }
            }
            return ret;
        }

        /* v132: Intercept recvmsg on netlink sockets (subcall 12 = SYS_RECVMSG).
         * If INET_DIAG response, log the UID and inode. */
        if (subcall == 12) { /* SYS_RECVMSG */
            unsigned long *args = (unsigned long *)a2;
            long ret = real_syscall_ptr(number, a1, a2, a3, a4, a5, a6);
            if (ret >= 0 && args) {
                int fd = (int)args[0];
                /* Check if this is a netlink socket by checking getsockname */
                struct sockaddr_storage ss;
                socklen_t slen = sizeof(ss);
                if (getsockname(fd, (struct sockaddr *)&ss, &slen) == 0 &&
                    ss.ss_family == 16) { /* AF_NETLINK */
                    static volatile int nl_recv_count = 0;
                    int cnt = __sync_fetch_and_add(&nl_recv_count, 1);
                    if (cnt < 20) {
                        debug_int("S32: NETLINK recvmsg fd=", fd);
                        debug_int("  ret=", ret);
                    }
                }
            }
            return ret;
        }

        if (subcall == 15) { /* SYS_GETSOCKOPT */
            unsigned long *args = (unsigned long *)a2;
            long ret = real_syscall_ptr(number, a1, a2, a3, a4, a5, a6);
            if (ret == 0 && args) {
                int level = (int)args[1];
                int optname = (int)args[2];
                void *optval = (void *)args[3];
                socklen_t *optlen = (socklen_t *)args[4];
                /* SOL_SOCKET=1, SO_PEERCRED=17 */
                if (level == 1 && optname == 17 && optval && optlen && *optlen >= sizeof(struct ucred)) {
                    struct ucred *cred = (struct ucred *)optval;
                    debug_int("S32: SO_PEERCRED(syscall) fd=", (long)args[0]);
                    debug_int("  pid=", (long)cred->pid);
                    debug_int("  uid=", (long)cred->uid);
                    debug_int("  my_pid=", (long)getpid());
                    /* Patch: replace peer PID with webhelper PID from file */
                    unsigned int wh_pid = get_webhelper_pid();
                    if (wh_pid > 0 && cred->pid != (pid_t)wh_pid) {
                        debug_int("  PATCHED pid to: ", (long)wh_pid);
                        cred->pid = (pid_t)wh_pid;
                    }
                }
            }
            return ret;
        }
    }

    return real_syscall_ptr(number, a1, a2, a3, a4, a5, a6);
}

__attribute__((constructor))
static void init(void) {
    /* v121: Init syscall wrapper early */
    if (!real_syscall_ptr)
        real_syscall_ptr = (real_syscall_fn)dlsym(RTLD_NEXT, "syscall");

    mkdir(SHM_REDIR_DIR, 0777);
    mkdir(LISTEN_PORT_DIR, 0777);
    mkdir("/tmp", 0777);
    debug_int("S32-v133c: pid=", (long)getpid());
    debug_msg("S32-v129: bridge-sync WH Shm_ files to S32 overlay\n");

    /* v111: Clean stale Shm_ files from previous runs.
     * ~100 accumulated files confuse the client (wrong sizes, stale data). */
    {
        int cleaned = 0;
        long dfd = syscall(SYS_openat, AT_FDCWD, SHM_REDIR_DIR, O_RDONLY | O_DIRECTORY);
        if (dfd >= 0) {
            char dirbuf[4096];
            long nread;
            while ((nread = syscall(SYS_getdents64, dfd, dirbuf, sizeof(dirbuf))) > 0) {
                long bpos = 0;
                while (bpos < nread) {
                    struct linux_dirent64 {
                        unsigned long long d_ino;
                        long long d_off;
                        unsigned short d_reclen;
                        unsigned char d_type;
                        char d_name[];
                    } *de = (void *)(dirbuf + bpos);
                    if (de->d_name[0] != '.' && strstr(de->d_name, "Shm_")) {
                        char rmpath[320];
                        int n = 0;
                        const char *d = SHM_REDIR_DIR;
                        while (*d && n < 250) rmpath[n++] = *d++;
                        rmpath[n++] = '/';
                        const char *s = de->d_name;
                        while (*s && n < 310) rmpath[n++] = *s++;
                        rmpath[n] = '\0';
                        syscall(SYS_unlinkat, dfd, de->d_name, 0);
                        cleaned++;
                    }
                    bpos += de->d_reclen;
                }
            }
            close((int)dfd);
        }
        debug_int("S32: cleaned stale Shm_ files=", cleaned);
    }

    /* v121: DON'T clean MasterStream files — v120 proved this deletes
     * the OTHER process's active file! Leave them alone. */

    /* v111: Clean stale listen port files from previous run */
    {
        for (int i = 0; i < 10; i++) {
            char path[256];
            int n = 0;
            const char *d = LISTEN_PORT_DIR;
            while (*d) path[n++] = *d++;
            path[n++] = '/';
            path[n++] = '0' + i;
            path[n] = '\0';
            unlink(path);
        }
    }

    /* Create FD 11 if it doesn't exist (no parent steam process on Android).
     * The Java app launches "steam -child-update-ui-socket 11" directly. */
    {
        struct stat st;
        if (fstat(11, &st) != 0) {
            debug_msg("S32: FD 11 missing — creating fake parent socketpair\n");
            int sv[2];
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
                dup2(sv[0], 11);
                close(sv[0]);
                fake_parent_fd = sv[1];
                /* Clear CLOEXEC on FD 11 so it can be inherited by webhelper */
                fcntl(11, F_SETFD, 0);
                debug_int("S32: created FD 11, fake_parent_fd=", sv[1]);
                /* Start fake parent thread */
                pthread_t fp_thread;
                pthread_create(&fp_thread, NULL, fake_parent_thread, (void *)(long)sv[1]);
                pthread_detach(fp_thread);
            } else {
                debug_int("S32: socketpair FAILED errno=", errno);
            }
        } else {
            debug_msg("S32: FD 11 already exists\n");
        }
    }

    /* v114: Pre-create MasterStream at SHM_REDIR_DIR (where wrappers redirect).
     * Previously at /dev/shm/ but wrappers redirect to SHM_REDIR_DIR with O_TRUNC
     * → file was empty → assertion "8192, 0". Now: pre-create at SHM_REDIR_DIR,
     * wrappers open WITHOUT O_TRUNC → client finds CMsgBrowserReady. */
    /* v119: RE-ADD mkdir /dev/shm. We no longer rely on the base-rootfs symlink.
     * Instead, the poller thread copies MasterStream files from SHM_REDIR_DIR
     * (shared Android path) into /dev/shm/ (per-process overlay). We need the
     * directory to exist in our overlay for raw syscall writes to work. */
    syscall(SYS_mkdir, "/dev/shm", 0777);
    {
        /* Build filename: SteamChrome_MasterStream_uid1000_spidNNNN_mem */
        char ms_name[128];
        {
            const char *p1 = "SteamChrome_MasterStream_uid1000_spid";
            int n = 0;
            while (*p1) ms_name[n++] = *p1++;
            long pid = (long)getpid();
            char pidbuf[16];
            int pn = 0;
            if (pid == 0) { pidbuf[pn++] = '0'; }
            else { long v = pid; while (v > 0) { pidbuf[pn++] = '0' + (v % 10); v /= 10; } }
            for (int i = pn - 1; i >= 0; i--) ms_name[n++] = pidbuf[i];
            const char *suf = "_mem";
            while (*suf) ms_name[n++] = *suf++;
            ms_name[n] = '\0';
        }

        /* Build SHM_REDIR_DIR path */
        char ms_redir[256];
        {
            int n = 0;
            const char *d = SHM_REDIR_DIR;
            while (*d && n < 200) ms_redir[n++] = *d++;
            ms_redir[n++] = '/';
            const char *s = ms_name;
            while (*s && n < 250) ms_redir[n++] = *s++;
            ms_redir[n] = '\0';
        }

        /* Also build /dev/shm/ path for backup */
        char ms_devshm[256];
        {
            int n = 0;
            const char *d = "/dev/shm/";
            while (*d && n < 200) ms_devshm[n++] = *d++;
            const char *s = ms_name;
            while (*s && n < 250) ms_devshm[n++] = *s++;
            ms_devshm[n] = '\0';
        }

        /* v130: Fill ring buffer header + CMsgBrowserReady (76 bytes, PC format).
         * Header: {get=0, put=76, m_cubBuffer=8192, pending=76} */
        unsigned char buf[8192];
        memset(buf, 0, sizeof(buf));
        write_browserready_ring(buf, sizeof(buf));

        /* Write to SHM_REDIR_DIR (primary — where wrappers redirect) */
        int msfd = (int)syscall(SYS_openat, AT_FDCWD, ms_redir,
                                O_CREAT | O_RDWR | O_TRUNC, 0666);
        if (msfd >= 0) {
            /* ftruncate FIRST to set file size, then write content.
             * FEX overlay may not update st_size from write() alone. */
            syscall(SYS_ftruncate, msfd, (long)8192);
            long wr = syscall(SYS_write, msfd, buf, sizeof(buf));
            /* Verify fstat sees the correct size */
            struct stat vst;
            fstat(msfd, &vst);
            debug_str("S32: v114 MS pre-created(redir): ", ms_redir, "\n");
            debug_int("  wrote=", wr);
            debug_int("  fstat_size=", (long)vst.st_size);
            close(msfd);
        } else {
            debug_str("S32: v114 MS FAILED(redir): ", ms_redir, "\n");
            debug_int("  errno=", errno);
        }

        /* Also write to /dev/shm/ (backup — in case client opens directly) */
        msfd = (int)syscall(SYS_openat, AT_FDCWD, ms_devshm,
                            O_CREAT | O_RDWR | O_TRUNC, 0666);
        if (msfd >= 0) {
            syscall(SYS_ftruncate, msfd, (long)8192);
            long wr2 = syscall(SYS_write, msfd, buf, sizeof(buf));
            struct stat vst2;
            fstat(msfd, &vst2);
            debug_str("S32: v114 MS pre-created(devshm): ", ms_devshm, "\n");
            debug_int("  wrote=", wr2);
            debug_int("  fstat_size=", (long)vst2.st_size);
            close(msfd);
        }

        /* v116: Pre-create via BOTH glibc shm_open AND raw syscall.
         * Diagnostic: compare the two paths to find which one Connect() uses.
         * Keep BOTH fds open. Start poller that checks both paths. */
        {
            typedef int (*real_shm_open_fn)(const char *, int, unsigned int);
            real_shm_open_fn rso = (real_shm_open_fn)dlsym(RTLD_NEXT, "shm_open");

            /* Build shm_open name: "/SteamChrome_MasterStream_..." */
            ms_poller_shm_name[0] = '/';
            { const char *s = ms_name; int i = 1; while (*s && i < 128) ms_poller_shm_name[i++] = *s++; ms_poller_shm_name[i] = '\0'; }

            /* Build /dev/shm/ path for raw syscall */
            { int n = 0; const char *d = "/dev/shm/";
              while (*d) ms_poller_devshm_path[n++] = *d++;
              const char *s = ms_name;
              while (*s && n < 250) ms_poller_devshm_path[n++] = *s++;
              ms_poller_devshm_path[n] = '\0'; }

            /* Save buf for poller thread */
            memcpy(ms_poller_buf, buf, 8192);

            /* Path A: glibc's real shm_open */
            if (rso) {
                int sfd = rso(ms_poller_shm_name, O_CREAT | O_RDWR, 0666);
                if (sfd >= 0) {
                    real_ftruncate_ptr = (real_ftruncate_fn)dlsym(RTLD_NEXT, "ftruncate");
                    if (real_ftruncate_ptr) real_ftruncate_ptr(sfd, 8192);
                    lseek(sfd, 0, SEEK_SET);
                    write(sfd, buf, 8192);
                    struct stat vst3;
                    fstat(sfd, &vst3);
                    debug_str("S32: v116 shm_open OK: ", ms_poller_shm_name, "\n");
                    debug_int("  fstat_size=", (long)vst3.st_size);
                    ms_precreate_fd = sfd; /* KEEP OPEN */
                    add_ms_fd(sfd);

                    /* Check where glibc's shm_open put the file via /proc/self/fd */
                    char fdlink[64], target[256];
                    { int n = 0; const char *p = "/proc/self/fd/";
                      while (*p) fdlink[n++] = *p++;
                      char fdbuf[16]; int fn = 0; int v = sfd;
                      if (v == 0) fdbuf[fn++] = '0';
                      else while (v > 0) { fdbuf[fn++] = '0' + (v%10); v /= 10; }
                      for (int i = fn-1; i >= 0; i--) fdlink[n++] = fdbuf[i];
                      fdlink[n] = '\0'; }
                    int tlen = readlink(fdlink, target, sizeof(target)-1);
                    if (tlen > 0) {
                        target[tlen] = '\0';
                        debug_str("S32: v116 shm_open fd→", target, "\n");
                    }
                }
            }

            /* Path B: raw syscall open on /dev/shm/ */
            {
                int rfd = (int)syscall(SYS_openat, AT_FDCWD, ms_poller_devshm_path,
                                       O_CREAT | O_RDWR, 0666);
                if (rfd >= 0) {
                    syscall(SYS_ftruncate, rfd, (long)8192);
                    lseek(rfd, 0, SEEK_SET);
                    syscall(SYS_write, rfd, buf, (long)8192);
                    debug_str("S32: v116 raw_open OK: ", ms_poller_devshm_path, "\n");
                    ms_precreate_raw_fd = rfd; /* KEEP OPEN */

                    /* v127: Test BOTH fstat variants right here in constructor */
                    struct stat64 raw_vst;
                    memset(&raw_vst, 0xCC, sizeof(raw_vst));
                    long fsr = raw32_fstat64(rfd, &raw_vst);
                    debug_int("S32-v129: PATH-B [fstat64/197] ret=", fsr);
                    debug_int("S32-v129: PATH-B [fstat64/197] size=", (long)raw_vst.st_size);

                    unsigned char old_stat_buf[128];
                    memset(old_stat_buf, 0xCC, sizeof(old_stat_buf));
                    long fsr2 = raw32_fstat_old(rfd, old_stat_buf);
                    unsigned long old_sz = *(unsigned long *)(old_stat_buf + 20);
                    debug_int("S32-v129: PATH-B [fstat/108] ret=", fsr2);
                    debug_int("S32-v129: PATH-B [fstat/108] size@20=", (long)old_sz);
                    debug_hexline("S32-v129: PATH-B [108] raw: ", old_stat_buf, 64);

                    /* Check where raw open put the file via /proc/self/fd */
                    char fdlink[64], target[256];
                    { int n = 0; const char *p = "/proc/self/fd/";
                      while (*p) fdlink[n++] = *p++;
                      char fdbuf[16]; int fn = 0; int v = rfd;
                      if (v == 0) fdbuf[fn++] = '0';
                      else while (v > 0) { fdbuf[fn++] = '0' + (v%10); v /= 10; }
                      for (int i = fn-1; i >= 0; i--) fdlink[n++] = fdbuf[i];
                      fdlink[n] = '\0'; }
                    int tlen = readlink(fdlink, target, sizeof(target)-1);
                    if (tlen > 0) {
                        target[tlen] = '\0';
                        debug_str("S32: v116 raw_open fd→", target, "\n");
                    }
                }
            }

            /* Start poller thread */
            ms_poller_active = 1;
            pthread_t ms_tid;
            pthread_create(&ms_tid, NULL, ms_poller_thread, NULL);
            pthread_detach(ms_tid);
            debug_msg("S32: v116 MS poller started\n");
        }
    }

    /* v104: Extract real Android tmpdir from FEX_OUTPUTLOG.
     * FEX_OUTPUTLOG = "/data/user/0/.../cache/tmp/fex-debug.log"
     * We want:       "/data/user/0/.../cache/tmp" */
    const char *fex_log = getenv("FEX_OUTPUTLOG");
    if (fex_log) {
        const char *last_slash = NULL;
        for (const char *p = fex_log; *p; p++)
            if (*p == '/') last_slash = p;
        if (last_slash && (last_slash - fex_log) > 0 &&
            (last_slash - fex_log) < (int)sizeof(real_tmpdir) - 1) {
            int len = (int)(last_slash - fex_log);
            memcpy(real_tmpdir, fex_log, len);
            real_tmpdir[len] = '\0';
            debug_str("S32: real_tmpdir=", real_tmpdir, "\n");
        }
    }

    /* Create the shmem dummy file early so client can find it */
    create_shmem_dummy_file();
}
