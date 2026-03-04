/* v109: 32-bit shim for the steam client binary.
 * - v109: Inject CMsgBrowserReady from client-side mmap (fixes cross-FEX
 *         mmap visibility issue — webhelper patcher writes weren't visible).
 *         Fill Ring A after 50 polls (~5s) if still empty.
 * - v106: bind/connect AF_UNIX → abstract socket conversion
 * - v105: MAP_SHARED on bridge fd (bidirectional IPC)
 * - v104: Detect IPC fd from -child-update-ui-socket cmdline arg.
 *         After CMsgBrowserReady consumed, send ready signal to
 *         Steam main process via IPC fd. Also: enhanced I/O
 *         monitoring on IPC fd (all data, not just sdPC).
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
#include <stdarg.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

/* v104: IPC fd for communication with Steam main process.
 * Detected from -child-update-ui-socket cmdline arg. */
static volatile int s32_ipc_fd = -1;

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

/* Real Android TMPDIR for abstract socket path rewriting.
 * libXlorie binds X11 socket with the full Android path in the abstract
 * socket name, but FEX guests see /tmp/ through the rootfs overlay.
 * We must rewrite X11 abstract connects to match. */
#define REAL_TMPDIR "/data/user/0/com.mediatek.steamlauncher/cache/tmp"
#define REAL_TMPDIR_LEN 49  /* strlen(REAL_TMPDIR) */

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
            /* Already abstract — check if X11 path needs rewriting */
            int namelen = pathlen > 1 ? pathlen - 1 : 0;
            const char *abs_name = &un->sun_path[1];

            /* libXlorie binds X11 with full Android path, FEX uses /tmp/ */
            if (namelen >= 15 && memcmp(abs_name, "/tmp/.X11-unix/", 15) == 0) {
                struct sockaddr_un rw;
                memset(&rw, 0, sizeof(rw));
                rw.sun_family = AF_UNIX;
                rw.sun_path[0] = '\0';
                /* REAL_TMPDIR + suffix after "/tmp" */
                memcpy(&rw.sun_path[1], REAL_TMPDIR, REAL_TMPDIR_LEN);
                int suffix_len = namelen - 4; /* skip "/tmp" */
                if (REAL_TMPDIR_LEN + suffix_len < 106) {
                    memcpy(&rw.sun_path[1 + REAL_TMPDIR_LEN], abs_name + 4, suffix_len);
                    socklen_t rw_len = offsetof(struct sockaddr_un, sun_path) + 1
                                       + REAL_TMPDIR_LEN + suffix_len;
                    int ret = real_connect_ptr(sockfd, (struct sockaddr *)&rw, rw_len);
                    int saved_errno = errno;
                    int n = __sync_fetch_and_add(&connect_abstract_count, 1);
                    if (n < 10) {
                        debug_str("S32: X11 abstract rewrite: ", abs_name, "\n");
                        debug_str("  → ", &rw.sun_path[1], "\n");
                        debug_int("  ret=", ret);
                        if (ret < 0) debug_int("  errno=", saved_errno);
                    }
                    errno = saved_errno;
                    return ret;
                }
            }

            /* Other abstract connects: pass through */
            int ret = real_connect_ptr(sockfd, addr, addrlen);
            int saved_errno = errno;
            int n = __sync_fetch_and_add(&connect_abstract_count, 1);
            /* Always log steam_chrome_shmem connects */
            int is_shmem = 0;
            if (namelen > 20) {
                for (int i = 0; i < namelen - 18; i++) {
                    if (memcmp(abs_name + i, "steam_chrome_shmem", 18) == 0) {
                        is_shmem = 1; break;
                    }
                }
            }
            if (n < 30 || is_shmem) {
                if (is_shmem) debug_msg("S32: *** steam_chrome_shmem CONNECT ***\n");
                debug_hexline("S32: connect ABSTRACT hex: ", abs_name,
                             namelen > 60 ? 60 : namelen);
                if (namelen > 0 && abs_name[0] >= 0x20) {
                    debug_str("S32:   name=", abs_name, "\n");
                }
                debug_int("  fd=", sockfd);
                debug_int("  ret=", ret);
                if (ret < 0) debug_int("  errno=", saved_errno);
            }
            errno = saved_errno;
            return ret;
        }

        /* Filesystem path → convert to abstract (with X11 path rewrite) */
        {
            const char *path = un->sun_path;
            int plen = 0;
            while (path[plen] && plen < 107) plen++;

            /* X11 paths: rewrite /tmp/.X11-unix/ → REAL_TMPDIR/.X11-unix/ */
            if (plen >= 15 && memcmp(path, "/tmp/.X11-unix/", 15) == 0) {
                struct sockaddr_un rw;
                memset(&rw, 0, sizeof(rw));
                rw.sun_family = AF_UNIX;
                rw.sun_path[0] = '\0';
                memcpy(&rw.sun_path[1], REAL_TMPDIR, REAL_TMPDIR_LEN);
                int suffix_len = plen - 4; /* skip "/tmp" */
                if (REAL_TMPDIR_LEN + suffix_len < 106) {
                    memcpy(&rw.sun_path[1 + REAL_TMPDIR_LEN], path + 4, suffix_len);
                    socklen_t rw_len = offsetof(struct sockaddr_un, sun_path) + 1
                                       + REAL_TMPDIR_LEN + suffix_len;
                    int ret = real_connect_ptr(sockfd, (struct sockaddr *)&rw, rw_len);
                    int saved_errno = errno;
                    int n = __sync_fetch_and_add(&connect_count, 1);
                    if (n < 10) {
                        debug_str("S32: X11 path rewrite: ", path, "\n");
                        debug_str("  → ", &rw.sun_path[1], "\n");
                        debug_int("  ret=", ret);
                        if (ret < 0) debug_int("  errno=", saved_errno);
                    }
                    errno = saved_errno;
                    return ret;
                }
            }

            /* Other paths: normal abstract conversion */
            struct sockaddr_un abstract_addr;
            socklen_t abs_len = make_abstract(addr, addrlen, &abstract_addr);
            if (abs_len > 0) {
                int ret = real_connect_ptr(sockfd, (struct sockaddr *)&abstract_addr, abs_len);
                int saved_errno = errno;
                int n = __sync_fetch_and_add(&connect_count, 1);
                /* Always log steam_chrome_shmem connects */
                int is_shmem_f = 0;
                if (plen > 20) {
                    for (int i = 0; i < plen - 18; i++) {
                        if (memcmp(path + i, "steam_chrome_shmem", 18) == 0) {
                            is_shmem_f = 1; break;
                        }
                    }
                }
                if (n < 50 || is_shmem_f) {
                    if (is_shmem_f) debug_msg("S32: *** steam_chrome_shmem FILE CONNECT ***\n");
                    debug_str("S32: connect→abstract: ", un->sun_path, "\n");
                    debug_int("  fd=", sockfd);
                    debug_int("  ret=", ret);
                    if (ret < 0) debug_int("  errno=", saved_errno);
                }
                errno = saved_errno;
                return ret;
            }
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

        /* v104: Try bridge on every poll failure (was: every 10th).
         * v98: SCM_RIGHTS — bridge sends the webhelper's fd via sendmsg.
         * Even with separate FEX fd tables, SCM_RIGHTS passes real fds.
         * The received fd shares the same open file → live MAP_SHARED. */
        if (pn >= 2) {
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
            /* File exists but is regular → fake as socket */
            buf->st_mode = (buf->st_mode & ~S_IFMT) | S_IFSOCK;
        } else if (ret != 0) {
            /* v109: File doesn't exist in 32-bit overlay (dummy was created
             * in webhelper's overlay). Fake a successful stat as S_IFSOCK. */
            memset(buf, 0, sizeof(*buf));
            buf->st_mode = S_IFSOCK | 0777;
            buf->st_nlink = 1;
            buf->st_size = 0;
            ret = 0;
        }
        int n = __sync_fetch_and_add(&stat_fake_count, 1);
        if (n < 20) {
            debug_str("S32: stat→S_IFSOCK: ", path, "\n");
            debug_int("  original_ret=", ret);
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
            /* v109: Fake success for ENOENT */
            memset(buf, 0, sizeof(*buf));
            buf->st_mode = S_IFSOCK | 0777;
            buf->st_nlink = 1;
            buf->st_size = 0;
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

/* access() wrapper: always succeed for shmem paths */
typedef int (*real_access_fn)(const char *, int);
static real_access_fn real_access_ptr = NULL;

int access(const char *path, int mode) {
    if (!real_access_ptr)
        real_access_ptr = (real_access_fn)dlsym(RTLD_NEXT, "access");
    int ret = real_access_ptr(path, mode);
    if (is_shmem_path(path)) {
        if (ret != 0) {
            /* v109: Fake success for ENOENT */
            ret = 0;
        }
        int n = __sync_fetch_and_add(&stat_fake_count, 1);
        if (n < 20) {
            debug_str("S32: access OK (faked): ", path, "\n");
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
    if (ret > 0 && buf) {
        /* v104: Log ANY write on IPC fd */
        if (s32_ipc_fd >= 0 && fd == s32_ipc_fd) {
            int n = __sync_fetch_and_add(&write_ipc_count, 1);
            if (n < 50) {
                debug_int("S32: write IPC fd=", fd);
                debug_int("  count=", (long)count);
                debug_int("  ret=", (long)ret);
                debug_hexline("  data: ", buf, ret > 40 ? 40 : (int)ret);
            }
        }
        /* Also log sdPC writes on any fd */
        else if (count >= 4) {
            const unsigned char *b = (const unsigned char *)buf;
            if (b[0] == 's' && b[1] == 'd' && b[2] == 'P' && b[3] == 'C') {
                int n = __sync_fetch_and_add(&write_ipc_count, 1);
                if (n < 50) {
                    debug_int("S32: write sdPC fd=", fd);
                    debug_int("  count=", (long)count);
                    debug_int("  ret=", (long)ret);
                }
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
    if (ret > 0 && buf) {
        /* v104: Log ANY read on IPC fd */
        if (s32_ipc_fd >= 0 && fd == s32_ipc_fd) {
            int n = __sync_fetch_and_add(&read_ipc_count, 1);
            if (n < 50) {
                debug_int("S32: read IPC fd=", fd);
                debug_int("  count=", (long)count);
                debug_int("  ret=", (long)ret);
                debug_hexline("  data: ", buf, ret > 40 ? 40 : (int)ret);
            }
        }
        /* Also log sdPC reads on any fd */
        else if (ret >= 4) {
            const unsigned char *b = (const unsigned char *)buf;
            if (b[0] == 's' && b[1] == 'd' && b[2] == 'P' && b[3] == 'C') {
                int n = __sync_fetch_and_add(&read_ipc_count, 1);
                if (n < 50) {
                    debug_int("S32: read sdPC fd=", fd);
                    debug_int("  count=", (long)count);
                    debug_int("  ret=", (long)ret);
                    debug_hexline("  data: ", buf, ret > 40 ? 40 : (int)ret);
                }
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

/* Forward declarations */
static void try_send_ipc_ready(void);
static volatile int ipc_saw_data;

/* Singleton anonymous mapping for the bridge Shm_ */
static void * volatile bridge_anon_map = NULL;
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

/* v105: Use REAL file-backed MAP_SHARED on the bridge fd.
 * Previously used MAP_ANONYMOUS which broke bidirectional IPC —
 * client writes to ANON were invisible to webhelper and vice versa.
 * The bridge (64-bit side) already patches CMsgBrowserReady into
 * the real Shm_ file before sending the fd via SCM_RIGHTS.
 * With MAP_SHARED, both sides share the same physical pages. */
static void *get_bridge_mapping(int fd, size_t length) {
    if (!bridge_anon_map) {
        if (!real_mmap_ptr)
            real_mmap_ptr = (real_mmap_fn)dlsym(RTLD_NEXT, "mmap");

        /* Try MAP_SHARED on the real fd first (bidirectional IPC) */
        void *m = real_mmap_ptr(NULL, length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, 0);
        if (m != MAP_FAILED) {
            bridge_anon_map = m;
            bridge_anon_size = length;
            debug_int("S32: mmap SHARED on bridge fd=", fd);
            debug_int("  size=", (long)length);
            debug_hexline("  hdr: ", m, 32);

            /* v108: Do NOT fill CMsgBrowserReady into Ring A.
             * This ring is client→server (commands). Writing CMsgBrowserReady
             * there causes "Invalid command" in webhelper's chrome_ipc_server.
             * Let Chromium init natively and write CMsgBrowserReady itself. */
            volatile unsigned int *hdr = (volatile unsigned int *)m;
            debug_int("S32: ring hdr: get=", hdr[0]);
            debug_int("  put=", hdr[1]);
            debug_int("  cap=", hdr[2]);
            debug_int("  pend=", hdr[3]);
            return m;
        }

        /* Fallback: MAP_ANONYMOUS (one-way, but won't crash) */
        debug_int("S32: MAP_SHARED failed, fallback ANON fd=", fd);
        void *anon = real_mmap_ptr(NULL, length, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (anon == MAP_FAILED) return anon;
        memset(anon, 0, length);
        bridge_anon_map = anon;
        bridge_anon_size = length;
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
            debug_int("S32: mmap bridge fd=", fd);
            debug_int("  count=", n + 1);
            if (bridge_anon_map)
                debug_hexline("  current hdr: ", bridge_anon_map, 16);
        }
        void *m = get_bridge_mapping(fd, length);

        /* v109: Inject CMsgBrowserReady from client side after 50 polls (~5s).
         * Cross-FEX mmap visibility doesn't work — the webhelper-side patcher
         * writes are never visible here. So we fill the ring buffer directly
         * in our own address space. The ring is server→client: webhelper creates
         * the Shm_ file, 32-bit client reads it. */
        if (n == 50 && m && m != MAP_FAILED) {
            volatile unsigned int *hdr = (volatile unsigned int *)m;
            if (hdr[1] == 0 && hdr[3] == 0 && hdr[2] > 0) {
                fill_shm_header(m, bridge_anon_size > 0 ? bridge_anon_size : length);
                ipc_saw_data = 1;
                debug_msg("S32: v109: injected CMsgBrowserReady after 50 polls\n");
                debug_hexline("  hdr now: ", m, 32);
            }
        }

        try_send_ipc_ready();
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
            debug_int("S32: mmap64 bridge fd=", fd);
            if (bridge_anon_map)
                debug_hexline("  current hdr: ", bridge_anon_map, 16);
        }
        void *m = get_bridge_mapping(fd, length);

        /* v109: Same client-side CMsgBrowserReady injection as mmap() */
        if (n == 50 && m && m != MAP_FAILED) {
            volatile unsigned int *hdr = (volatile unsigned int *)m;
            if (hdr[1] == 0 && hdr[3] == 0 && hdr[2] > 0) {
                fill_shm_header(m, bridge_anon_size > 0 ? bridge_anon_size : length);
                ipc_saw_data = 1;
                debug_msg("S32: v109: injected CMsgBrowserReady after 50 polls (mmap64)\n");
            }
        }

        try_send_ipc_ready();
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

/* v104: Auto-detect IPC fd from -child-update-ui-socket cmdline arg.
 * The child-update-ui process gets this arg with the fd number for
 * communicating readiness back to the Steam main process. */
static void detect_s32_ipc_fd(void) {
    char cmdline[4096];
    int fd = syscall(SYS_openat, -100, "/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return;
    ssize_t n = syscall(SYS_read, fd, cmdline, 4095);
    syscall(SYS_close, fd);
    if (n <= 0) return;
    cmdline[n] = '\0';

    /* Scan NUL-separated args for "-child-update-ui-socket" */
    const char *needle = "-child-update-ui-socket";
    int needle_len = 23;
    int pos = 0;
    while (pos < n) {
        int arg_start = pos;
        while (pos < n && cmdline[pos] != '\0') pos++;
        int arg_len = pos - arg_start;
        pos++; /* skip NUL */

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
                    s32_ipc_fd = (int)val;
                    debug_int("S32: detected IPC fd from cmdline: ", val);
                    /* Write fd to shared file so OTHER guest processes can read it.
                     * Under FEX, fork() creates separate address spaces, so static
                     * variables are NOT shared between guest processes. */
                    {
                        char fd_str[16];
                        int sl = 0;
                        long tmp = val;
                        char digs[10]; int nd = 0;
                        if (tmp == 0) digs[nd++] = '0';
                        else while (tmp > 0) { digs[nd++] = '0' + (tmp % 10); tmp /= 10; }
                        for (int i = nd - 1; i >= 0; i--) fd_str[sl++] = digs[i];
                        fd_str[sl++] = '\n';
                        int wfd = syscall(SYS_openat, -100, "/tmp/s32_ipc_fd",
                                          O_WRONLY | O_CREAT | O_TRUNC, 0666);
                        if (wfd >= 0) {
                            syscall(SYS_write, wfd, fd_str, sl);
                            syscall(SYS_close, wfd);
                            debug_msg("S32: wrote IPC fd to /tmp/s32_ipc_fd\n");
                        }
                    }
                    return;
                }
            }
        }
    }
    debug_msg("S32: no -child-update-ui-socket in cmdline (not child-update-ui process)\n");
}

/* v104b: IPC ready signal — triggered INLINE from mmap interceptor.
 * Previous approach (polling thread) failed because FEX kills pthreads
 * created from DT_NEEDED constructors. Instead, we detect CMsgBrowserReady
 * consumption directly in the mmap() wrapper and send the ready signal
 * on the IPC fd synchronously.
 *
 * Flow: client mmaps → gets singleton with put=10 → consumes → munmaps →
 *       re-mmaps → we see put=0 → send ready signal on ipc_fd.
 */
static volatile int ipc_ready_sent = 0;
static volatile int ipc_saw_data = 0;

static void try_send_ipc_ready(void) {
    if (ipc_ready_sent) return;
    int my_fd = s32_ipc_fd;
    if (my_fd < 0) {
        /* FEX fork() creates separate address spaces — read from shared file
         * written by the child-update-ui guest process */
        char fd_buf[16];
        int rfd = syscall(SYS_openat, -100, "/tmp/s32_ipc_fd", O_RDONLY);
        if (rfd >= 0) {
            ssize_t rn = syscall(SYS_read, rfd, fd_buf, 15);
            syscall(SYS_close, rfd);
            if (rn > 0) {
                long val = 0;
                for (int i = 0; i < rn && fd_buf[i] >= '0' && fd_buf[i] <= '9'; i++)
                    val = val * 10 + (fd_buf[i] - '0');
                if (val > 0 && val < 65536) {
                    s32_ipc_fd = (int)val;
                    my_fd = (int)val;
                    debug_int("S32: try_ready: loaded IPC fd from file: ", val);
                }
            }
        }
        if (my_fd < 0) {
            debug_int("S32: try_ready: skip, ipc_fd=", (long)my_fd);
            return;
        }
    }
    if (!bridge_anon_map) {
        debug_msg("S32: try_ready: skip, no bridge_anon_map\n");
        return;
    }

    /* v105b: With MAP_SHARED, client mmaps ONCE and polls in-place —
     * no mmap→munmap→remmap cycle. So we can't wait for consumption
     * detection via repeated mmap calls. Instead, send immediately
     * when ipc_saw_data is set (meaning we filled CMsgBrowserReady). */
    if (ipc_saw_data) {
        if (__sync_bool_compare_and_swap(&ipc_ready_sent, 0, 1)) {
            debug_msg("S32: ipc_ready: CMsgBrowserReady consumed! Sending ready signal.\n");

            if (!real_write32_ptr)
                real_write32_ptr = (real_write32_fn)dlsym(RTLD_NEXT, "write");

            /* Send sdPC ready message (56 bytes) */
            unsigned char ready_msg[56];
            memset(ready_msg, 0, sizeof(ready_msg));
            ready_msg[0] = 's'; ready_msg[1] = 'd';
            ready_msg[2] = 'P'; ready_msg[3] = 'C';
            ready_msg[4] = 0x02; /* version = 2 */
            ready_msg[8] = 0x01; /* type = 1 */

            ssize_t w = real_write32_ptr(my_fd, ready_msg, 56);
            debug_int("S32: ipc_ready: wrote sdPC type=1, ret=", (long)w);

            if (w == 56) {
                char one = '1';
                w = real_write32_ptr(my_fd, &one, 1);
                debug_int("S32: ipc_ready: wrote '1' status, ret=", (long)w);
            }
            debug_msg("S32: ipc_ready: DONE\n");
        }
    }
}

__attribute__((constructor))
static void init(void) {
    mkdir(SHM_REDIR_DIR, 0777);

    /* Detect IPC fd from cmdline */
    detect_s32_ipc_fd();

    debug_int("S32-v109: pid=", (long)getpid());
    debug_int("  ipc_fd=", (long)s32_ipc_fd);

    /* v104b: IPC ready signal is sent inline from mmap interceptor,
     * not from a separate thread (FEX kills pthreads from constructors). */
    if (s32_ipc_fd >= 0) {
        debug_int("S32: IPC ready will trigger from mmap interceptor, fd=", s32_ipc_fd);
    }
}
