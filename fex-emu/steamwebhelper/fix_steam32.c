/* v93: 32-bit shim for the steam client binary.
 * - bind/connect AF_UNIX → abstract socket conversion
 * - shm_open/shm_unlink redirect to real Android path
 * - open/open64: redirect /dev/shm/ to real Android path
 * - shm_open: force O_CREAT, strip O_EXCL (match 64-bit shim)
 * - v91: enhanced connect logging (single-buffer hex dump, return values)
 *        + log already-abstract connects + log bind paths
 * - v91b: stat/lstat fake S_IFSOCK for steam_chrome_shmem paths
 * - v93: fake SHMemStream — creates Shm_ file + shmem socket from 32-bit side
 *        (64-bit side can't create files visible to 32-bit FEX overlay)
 */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <stddef.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/types.h>
#include <stdarg.h>
#include <errno.h>
/* pthread.h removed — socket thread no longer needed */

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
            /* Already abstract — pass through without conversion, but LOG it */
            int ret = real_connect_ptr(sockfd, addr, addrlen);
            int saved_errno = errno;
            int n = __sync_fetch_and_add(&connect_abstract_count, 1);
            if (n < 30) {
                /* Log the abstract name (skip leading \0, dump remaining bytes) */
                int namelen = pathlen > 1 ? pathlen - 1 : 0;
                debug_hexline("S32: connect ABSTRACT hex: ", &un->sun_path[1],
                             namelen > 60 ? 60 : namelen);
                /* Also try to print as string (abstract name after \0) */
                if (namelen > 0 && un->sun_path[1] >= 0x20) {
                    debug_str("S32:   name=", &un->sun_path[1], "\n");
                }
                debug_int("  fd=", sockfd);
                debug_int("  ret=", ret);
                if (ret < 0) debug_int("  errno=", saved_errno);
            }
            errno = saved_errno;
            return ret;
        }

        /* Filesystem path → convert to abstract */
        struct sockaddr_un abstract_addr;
        socklen_t abs_len = make_abstract(addr, addrlen, &abstract_addr);
        if (abs_len > 0) {
            int ret = real_connect_ptr(sockfd, (struct sockaddr *)&abstract_addr, abs_len);
            int saved_errno = errno;
            int n = __sync_fetch_and_add(&connect_count, 1);
            if (n < 50) {
                /* Single-buffer: prefix + path in one write */
                debug_str("S32: connect→abstract: ", un->sun_path, "\n");
                /* Also hex dump the raw path for debugging */
                debug_hexline("  hex: ", un->sun_path, pathlen > 60 ? 60 : pathlen);
                debug_int("  fd=", sockfd);
                debug_int("  addrlen=", addrlen);
                debug_int("  abs_len=", abs_len);
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



int shm_open(const char *name, int oflag, mode_t mode) {
    char path[256];
    build_shm_path(path, sizeof(path), name);
    if (!real_open_ptr)
        real_open_ptr = (real_open_fn)dlsym(RTLD_NEXT, "open");

    int real_flags;
    if (oflag & O_CREAT) {
        /* Caller wants to create: strip O_EXCL, force O_RDWR */
        real_flags = (oflag & ~O_EXCL) | O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW;
        if (!mode) mode = 0666;
    } else {
        /* Caller is just opening/polling (O_RDONLY): preserve original flags!
         * This is how the steam client polls for webhelper readiness. */
        real_flags = oflag | O_CLOEXEC | O_NOFOLLOW;
        if (!mode) mode = 0666;
    }

    int fd = real_open_ptr(path, real_flags, mode);
    int count = __sync_fetch_and_add(&shm_open_count_32, 1);
    /* Always log polling (O_RDONLY) calls, and first 30 of others */
    if (count < 30 || !(oflag & O_CREAT)) {
        debug_str("S32: shm_open(", name, ")\n");
        debug_int("  flags=", oflag);
        debug_int("  fd=", fd);
    }

    /* Track Shm_ polling failures. After 10 failed O_RDONLY polls (~1s),
     * create the Shm_ file ourselves with a valid SHMemStream header.
     * Must be done from the 32-bit side because 64-bit FEX has a separate
     * overlay filesystem — files created by 64-bit are invisible here. */
    if (fd < 0 && !(oflag & O_CREAT) &&
        strstr(name, "Shm_") != NULL && !shm_poll_written) {
        int pn = __sync_fetch_and_add(&shm_poll_fail_count, 1);
        if (pn == 10) {
            shm_poll_written = 1;
            debug_str("S32: FAKE_SHMEM: creating fake Shm_: ", name, "\n");

            /* Create the Shm_ file via our own shm_open (O_CREAT path) */
            int sfd = shm_open(name, O_CREAT | O_RDWR, 0666);
            debug_int("S32: FAKE_SHMEM: shm_open O_CREAT fd=", sfd);
            if (sfd >= 0) {
                /* Build SHMemStream header (76 bytes) */
                unsigned char hdr[76];
                memset(hdr, 0, sizeof(hdr));
                long spid = (long)getpid(); /* we ARE the steam client */

                hdr[0] = 0x4c;                                /* header_size = 76 */
                hdr[4] = 0x4c;                                /* data_offset = 76 */
                hdr[8] = 0x00; hdr[9] = 0x20;                 /* buffer_size = 8192 */
                hdr[16] = 0x01;                                /* version = 1 */
                hdr[20] = 0x01;                                /* count = 1 */
                hdr[24] = (unsigned char)(spid & 0xff);
                hdr[25] = (unsigned char)((spid >> 8) & 0xff);
                hdr[26] = (unsigned char)((spid >> 16) & 0xff);
                hdr[27] = (unsigned char)((spid >> 24) & 0xff);

                /* Stream name at offset 28 */
                char sname[48];
                memset(sname, 0, sizeof(sname));
                {
                    char *p = sname;
                    const char *pfx = "SteamChrome_MasterStream_";
                    while (*pfx) *p++ = *pfx++;
                    char tmp[12];
                    long v = spid; int nd = 0;
                    if (v == 0) tmp[nd++] = '0';
                    else while (v > 0) { tmp[nd++] = '0' + (v % 10); v /= 10; }
                    for (int i = nd-1; i >= 0; i--) *p++ = tmp[i];
                    *p++ = '_';
                    *p++ = '1'; /* suffix */
                    *p = '\0';
                }
                memcpy(&hdr[28], sname, 47);

                syscall(SYS_write, sfd, hdr, 76);
                ftruncate(sfd, 76 + 8192);
                close(sfd);

                /* Verify it's readable */
                int vfd = shm_open(name, O_RDONLY, 0);
                debug_int("S32: FAKE_SHMEM: verify O_RDONLY fd=", vfd);
                if (vfd >= 0) close(vfd);

                debug_str("S32: FAKE_SHMEM: stream=", sname, "\n");
                /* Socket is handled by the webhelper's own chrome_ipc_server.
                 * We only create the Shm_ file here (32-bit FEX overlay). */
            }
        }
    }

    return fd;
}

int shm_unlink(const char *name) {
    char path[256];
    build_shm_path(path, sizeof(path), name);
    int ret = unlink(path);
    debug_str("S32: shm_unlink(", name, ")\n");
    debug_int("  ret=", ret);
    return ret;
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

    /* Log any open that touches steam_chrome_shmem or shmem paths */
    if (pathname && (strstr(pathname, "steam_chrome") || strstr(pathname, "chrome_shmem"))) {
        debug_str("S32: open(shmem-related): ", pathname, "\n");
        debug_int("  flags=", flags);
    }

    char redir[256];
    if (build_devshm_redir(pathname, redir, sizeof(redir))) {
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
    if (ret == 0 && is_shmem_path(path) && S_ISREG(buf->st_mode)) {
        buf->st_mode = (buf->st_mode & ~S_IFMT) | S_IFSOCK;
        int n = __sync_fetch_and_add(&stat_fake_count, 1);
        if (n < 10) {
            debug_str("S32: stat→S_IFSOCK: ", path, "\n");
        }
    }
    return ret;
}

int __lxstat(int ver, const char *path, struct stat *buf) {
    if (!real_lxstat_ptr)
        real_lxstat_ptr = (real_lxstat_fn)dlsym(RTLD_NEXT, "__lxstat");
    int ret = real_lxstat_ptr(ver, path, buf);
    if (ret == 0 && is_shmem_path(path) && S_ISREG(buf->st_mode)) {
        buf->st_mode = (buf->st_mode & ~S_IFMT) | S_IFSOCK;
        int n = __sync_fetch_and_add(&stat_fake_count, 1);
        if (n < 10) {
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

/* access() wrapper: if the file exists, succeed for shmem paths */
typedef int (*real_access_fn)(const char *, int);
static real_access_fn real_access_ptr = NULL;

int access(const char *path, int mode) {
    if (!real_access_ptr)
        real_access_ptr = (real_access_fn)dlsym(RTLD_NEXT, "access");
    int ret = real_access_ptr(path, mode);
    if (ret == 0 && is_shmem_path(path)) {
        int n = __sync_fetch_and_add(&stat_fake_count, 1);
        if (n < 10) {
            debug_str("S32: access OK: ", path, "\n");
        }
    }
    return ret;
}

/* socketpair wrapper: log when steam client creates IPC sockets */
typedef int (*real_socketpair_fn)(int, int, int, int[2]);
static real_socketpair_fn real_socketpair_ptr = NULL;
static volatile int socketpair_count = 0;

int socketpair(int domain, int type, int protocol, int sv[2]) {
    if (!real_socketpair_ptr)
        real_socketpair_ptr = (real_socketpair_fn)dlsym(RTLD_NEXT, "socketpair");
    int ret = real_socketpair_ptr(domain, type, protocol, sv);
    int n = __sync_fetch_and_add(&socketpair_count, 1);
    if (n < 20) {
        debug_int("S32: socketpair fd0=", sv[0]);
        debug_int("  fd1=", sv[1]);
    }
    return ret;
}

/* write wrapper: monitor writes that look like IPC to webhelper */
typedef ssize_t (*real_write32_fn)(int, const void *, size_t);
static real_write32_fn real_write32_ptr = NULL;
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
typedef ssize_t (*real_read32_fn)(int, void *, size_t);
static real_read32_fn real_read32_ptr = NULL;
static volatile int read_ipc_count = 0;

ssize_t read(int fd, void *buf, size_t count) {
    if (!real_read32_ptr)
        real_read32_ptr = (real_read32_fn)dlsym(RTLD_NEXT, "read");
    ssize_t ret = real_read32_ptr(fd, buf, count);
    /* Log reads that returned sdPC IPC messages */
    if (ret >= 4 && buf) {
        const unsigned char *b = (const unsigned char *)buf;
        if (b[0] == 's' && b[1] == 'd' && b[2] == 'P' && b[3] == 'C') {
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

__attribute__((constructor))
static void init(void) {
    mkdir(SHM_REDIR_DIR, 0777);
    debug_int("S32-v93: pid=", (long)getpid());
    debug_msg("S32-v93: + fake SHMemStream coordination\n");
}
