/* v104: 32-bit shim for the steam client binary.
 * - bind/connect AF_UNIX → abstract socket conversion (steam-only)
 * - shm_open/shm_unlink redirect to real Android path
 * - open/open64: redirect /dev/shm/ to real Android path
 * - shm_open: force O_CREAT, strip O_EXCL (match 64-bit shim)
 * - v91: enhanced connect logging
 * - v91b: stat/lstat fake S_IFSOCK for steam_chrome_shmem paths
 * - v98: SCM_RIGHTS fd passing across FEX overlay boundary
 * - v100: Bridge fd dup (no local file creation)
 * - v101: Intercept mmap for bridge fds → anonymous mapping
 * - v102: CORRECT SHMemStream header format (from local trace):
 *         hdr[0]=get (read cursor), hdr[1]=put (write cursor),
 *         hdr[2]=capacity, hdr[3]=pending bytes.
 *         NOT cubHeader/eState as previously assumed.
 * - v103: FD 11 bridge — fake webhelper handshake from 32-bit side.
 *         The webhelper (64-bit) never inherits fd 11 because
 *         cross-FEX-arch exec loses inherited fds. We dup the child
 *         end of the socketpair and fake the sdPC ready response so
 *         the Steam client thinks the webhelper is ready.
 *         Also: stat/access fake for missing steam_chrome_shmem files,
 *         and create dummy shmem file in 32-bit overlay.
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
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/types.h>
#include <poll.h>
#include <stdarg.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

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
            if (real_tmpdir[0] && strncmp(abs_name, "/tmp/", 5) == 0) {
                /* Rewrite @/tmp/foo → @$real_tmpdir/foo */
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
                if (n < 30) {
                    debug_str("S32: connect ABSTRACT→rewrite: ", abs_name, "\n");
                    debug_str("  → @", &rewritten.sun_path[1], "\n");
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
            if (n < 30) {
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
         * socket name so they connect to the ARM64 X11 server. */
        if (real_tmpdir[0] && strncmp(un->sun_path, "/tmp/", 5) == 0) {
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
            if (n < 50) {
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


int shm_open(const char *name, int oflag, mode_t mode) {
    char path[256];
    build_shm_path(path, sizeof(path), name);

    /* v100: If we already have a bridge fd for this Shm_ name, return dup.
     * v101: Track the dup fd so our mmap interceptor can catch it later. */
    if (!(oflag & O_CREAT) && bridge_shm_fd >= 0 &&
        strstr(name, "Shm_") != NULL && strcmp(name, bridge_shm_name) == 0) {
        int dfd = dup(bridge_shm_fd);
        if (dfd >= 0) add_bridge_dup_fd(dfd);
        int count = __sync_fetch_and_add(&shm_open_count_32, 1);
        if (count < 10 || (count % 100) == 0) {
            debug_int("S32: shm_open→bridge dup fd=", dfd);
        }
        return dfd;
    }

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

    /* v100: No fd tracking needed — we use bridge_shm_fd directly. */

    /* Track Shm_ polling failures. On every failed poll, try to get real
     * data from the 64-bit bridge socket. The bridge sends the webhelper's
     * actual Shm_ file content (with real SHMemStream header + ring buffer).
     * We write it to our local (32-bit) overlay so the steam client can see it.
     *
     * Must be done INLINE here (not in a separate thread) because FEX overlay
     * isolation is per-process — files created by a different PID are invisible. */
    if (fd < 0 && !(oflag & O_CREAT) && strstr(name, "Shm_") != NULL) {
        int pn = __sync_fetch_and_add(&shm_poll_fail_count, 1);

        /* Try bridge every 10 polls (~1s).
         * v98: SCM_RIGHTS — bridge sends the webhelper's fd via sendmsg.
         * Even with separate FEX fd tables, SCM_RIGHTS passes real fds.
         * The received fd shares the same open file → live MAP_SHARED. */
        if (pn >= 5 && (pn % 10) == 0) {
            int bsock = socket(AF_UNIX, SOCK_STREAM, 0);
            if (bsock >= 0) {
                struct sockaddr_un baddr;
                memset(&baddr, 0, sizeof(baddr));
                baddr.sun_family = AF_UNIX;
                memcpy(baddr.sun_path, "\0shm_bridge_64to32", 19);

                if (!real_connect_ptr)
                    real_connect_ptr = (real_connect_fn)dlsym(RTLD_NEXT, "connect");
                int cret = real_connect_ptr(bsock, (struct sockaddr *)&baddr,
                    offsetof(struct sockaddr_un, sun_path) + 19);

                if (cret == 0) {
                    /* v98: receive fds via recvmsg(SCM_RIGHTS).
                     * Each message has: iov = name_len(2) + name, cmsg = fd.
                     * Terminator: name_len=0 via plain recv. */
                    int got = 0;
                    while (1) {
                        /* Receive name + fd via recvmsg */
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

                        got++;
                        if (got <= 10) {
                            debug_str("S32: BRIDGE: SCM_RIGHTS ", bname, "\n");
                            debug_int("  received_fd=", received_fd);
                        }

                        /* If name matches our poll, save as bridge fd */
                        if (strcmp(bname, name) == 0 && received_fd >= 0) {
                            /* v100: Save bridge fd globally for future dup()s.
                             * v101: mmap interception handles the header. */
                            bridge_shm_fd = received_fd;
                            {
                                int i = 0;
                                while (name[i] && i < 63) {
                                    bridge_shm_name[i] = name[i]; i++;
                                }
                                bridge_shm_name[i] = '\0';
                            }
                            debug_int("  MATCH! bridge_shm_fd=", received_fd);

                            fd = dup(received_fd);
                            if (fd >= 0) add_bridge_dup_fd(fd);
                            debug_int("  returning dup fd=", fd);
                        } else if (received_fd >= 0) {
                            /* Not the one we need — close it to avoid leak */
                            close(received_fd);
                        }
                    }
                    if (got > 0) {
                        debug_int("S32: BRIDGE: total fds=", got);
                    }
                } else if (pn < 50 || (pn % 25) == 0) {
                    debug_int("S32: BRIDGE: connect failed pn=", pn);
                    debug_int("  errno=", errno);
                }
                close(bsock);
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

    debug_msg("S32: fd11_fake: sleeping 3s for child-update-ui to finish...\n");
    struct timespec ts = { .tv_sec = 3, .tv_nsec = 0 };
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

    fd11_ready_done = 1;
    debug_msg("S32: fd11_fake: DONE — ready signal sent\n");
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

    /* v103: Save a dup of sv[0] (the child's end) before the parent closes it.
     * We dup to a high fd number (200+) so it won't collide with normal fds.
     * The dup survives even after close(sv[0]) because it's an independent fd
     * referencing the same socket endpoint. */
    if (ret == 0 && ipc_child_fd < 0 && domain == AF_UNIX) {
        int saved = fcntl(sv[0], F_DUPFD, 200);
        if (saved >= 0) {
            /* Clear CLOEXEC so it survives (fcntl F_DUPFD doesn't set it) */
            ipc_child_fd = saved;
            debug_int("S32: fd11: saved child end dup fd=", saved);
            debug_int("S32: fd11: original sv[0]=", sv[0]);
            debug_int("S32: fd11: parent sv[1]=", sv[1]);

            /* Start the fake webhelper handshake thread */
            pthread_t thr;
            pthread_create(&thr, NULL, fd11_fake_ready_func, NULL);
            pthread_detach(thr);
            debug_msg("S32: fd11: started fake ready thread\n");
        } else {
            debug_int("S32: fd11: dup failed errno=", errno);
        }
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

/* v102: CORRECT SHMemStream header format (confirmed via local Steam trace):
 *   hdr[0] = get  (read cursor into ring buffer, 0-based)
 *   hdr[1] = put  (write cursor into ring buffer, 0-based)
 *   hdr[2] = capacity (ring buffer size in bytes)
 *   hdr[3] = pending (bytes available = put - get)
 *   Ring buffer data starts at file/mapping offset 16.
 *
 * We write CMsgBrowserReady (10 bytes) into the ring buffer at offset 0,
 * then set put=10, pending=10 so the client reads it.
 * Only fill ONCE on creation — after client consumes + Clear()s, leave it. */
static void fill_shm_header(void *mapping, size_t length) {
    unsigned int *hdr = (unsigned int *)mapping;

    /* Ring buffer header */
    hdr[0] = 0;      /* get = 0 (read starts at ring offset 0) */
    hdr[1] = 10;     /* put = 10 (10 bytes written to ring) */
    hdr[2] = 8192;   /* capacity */
    hdr[3] = 10;     /* pending = 10 bytes */

    /* CMsgBrowserReady at ring buffer offset 0 (file offset 16)
     * Message format: [4B type][4B size][payload]
     * Type=0 (BrowserReady), size=2, payload=08 01 (handle=1) */
    if (length >= 26) {
        unsigned char *ring = (unsigned char *)mapping + 16;
        unsigned int msg_type = 0;
        unsigned int msg_size = 2;
        memcpy(ring, &msg_type, 4);
        memcpy(ring + 4, &msg_size, 4);
        ring[8] = 0x08;  /* protobuf field 1, varint */
        ring[9] = 0x01;  /* browser_handle = 1 */
    }
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

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if (!real_mmap_ptr)
        real_mmap_ptr = (real_mmap_fn)dlsym(RTLD_NEXT, "mmap");

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

__attribute__((constructor))
static void init(void) {
    mkdir(SHM_REDIR_DIR, 0777);
    mkdir("/tmp", 0777);
    debug_int("S32-v104: pid=", (long)getpid());
    debug_msg("S32-v104: fd11 bridge + shmem stat fake\n");

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
