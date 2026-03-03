/* Trace shim v3: LD_PRELOAD into steam/steamwebhelper to capture IPC protocol.
 * Safe version: only intercepts shm_open, write, read, socket ops.
 * Monitors /dev/shm headers via a background thread (no mmap/close interception).
 *
 * Compile 64-bit: gcc -shared -fPIC -O2 -w -o trace_steam_ipc64.so trace_steam_ipc.c -ldl -lrt -lpthread
 * Compile 32-bit: gcc -m32 -shared -fPIC -O2 -w -o trace_steam_ipc32.so trace_steam_ipc.c -ldl -lrt -lpthread
 */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <string.h>
#include <stddef.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <pthread.h>
#include <dirent.h>

static FILE *logf = NULL;
static pid_t my_pid = 0;
static int is_32bit = 0;

/* --- Track shm names opened by this process --- */
#define MAX_SHM_NAMES 64
static char shm_names[MAX_SHM_NAMES][128];
static int shm_name_count = 0;

static void track_shm_name(const char *name) {
    if (shm_name_count < MAX_SHM_NAMES) {
        strncpy(shm_names[shm_name_count], name, 127);
        shm_names[shm_name_count][127] = '\0';
        shm_name_count++;
    }
}

static void ensure_log(void) {
    if (logf) return;
    my_pid = getpid();
    is_32bit = (sizeof(void*) == 4);
    char path[256];
    snprintf(path, sizeof(path), "/tmp/steam_ipc_trace_%s_%d.log",
             is_32bit ? "32" : "64", my_pid);
    logf = fopen(path, "w");
    if (!logf) logf = stderr;
    setvbuf(logf, NULL, _IOLBF, 0);
}

static double elapsed(void) {
    static struct timespec start = {0};
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (start.tv_sec == 0) start = now;
    return (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
}

static void log_hex(const char *prefix, const void *data, int len) {
    ensure_log();
    fprintf(logf, "[%7.3f] %s[%d] %s", elapsed(), is_32bit ? "S32" : "S64", my_pid, prefix);
    const unsigned char *d = (const unsigned char *)data;
    int show = len > 64 ? 64 : len;
    for (int i = 0; i < show; i++) fprintf(logf, "%02x ", d[i]);
    if (len > 64) fprintf(logf, "...");
    fprintf(logf, "\n");
}

static void log_msg(const char *fmt, ...) {
    ensure_log();
    va_list ap;
    va_start(ap, fmt);
    fprintf(logf, "[%7.3f] %s[%d] ", elapsed(), is_32bit ? "S32" : "S64", my_pid);
    vfprintf(logf, fmt, ap);
    va_end(ap);
}

/* --- /dev/shm monitor thread --- */
/* Reads Shm_ files from /dev/shm directly, no mmap interception needed */
static void dump_shm_file(const char *path, const char *name) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    unsigned char buf[80];
    ssize_t n = pread(fd, buf, sizeof(buf), 0);
    close(fd);
    if (n >= 16) {
        unsigned int *h = (unsigned int *)buf;
        log_msg("SHM_HDR[%s]: hdr[0]=%u hdr[1]=%u hdr[2]=%u hdr[3]=%u\n",
                name, h[0], h[1], h[2], h[3]);
        if (h[0] != 0 || h[1] != 0 || h[2] != 0 || h[3] != 0) {
            log_hex("  raw: ", buf, n > 64 ? 64 : (int)n);
        }
    }
}

static void *shm_monitor_thread(void *arg) {
    (void)arg;
    while (1) {
        sleep(2);
        /* Scan /dev/shm for Shm_ files */
        DIR *d = opendir("/dev/shm");
        if (!d) continue;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strstr(ent->d_name, "Shm_") || strstr(ent->d_name, "ValveIPC")) {
                char path[512];
                snprintf(path, sizeof(path), "/dev/shm/%s", ent->d_name);
                dump_shm_file(path, ent->d_name);
            }
        }
        closedir(d);
    }
    return NULL;
}

/* ---- write() ---- */
typedef ssize_t (*real_write_fn)(int, const void *, size_t);
static real_write_fn real_write_ptr = NULL;

ssize_t write(int fd, const void *buf, size_t count) {
    if (!real_write_ptr)
        real_write_ptr = (real_write_fn)dlsym(RTLD_NEXT, "write");
    ssize_t ret = real_write_ptr(fd, buf, count);
    if (fd >= 3 && ret > 0 && buf) {
        const unsigned char *b = (const unsigned char *)buf;
        if (count >= 4 && b[0]=='s' && b[1]=='d' && b[2]=='P' && b[3]=='C') {
            log_msg("WRITE sdPC fd=%d count=%zd ret=%zd\n", fd, count, ret);
            log_hex("  data: ", buf, (int)(ret > 64 ? 64 : ret));
        }
        else if (count == 1 && fd < 256) {
            struct stat st;
            if (fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode)) {
                log_msg("WRITE 1-byte fd=%d val=0x%02x '%c' ret=%zd\n",
                        fd, b[0], (b[0] >= 0x20 && b[0] < 0x7f) ? b[0] : '.', ret);
            }
        }
    }
    return ret;
}

/* ---- read() ---- */
typedef ssize_t (*real_read_fn)(int, void *, size_t);
static real_read_fn real_read_ptr = NULL;

ssize_t read(int fd, void *buf, size_t count) {
    if (!real_read_ptr)
        real_read_ptr = (real_read_fn)dlsym(RTLD_NEXT, "read");
    ssize_t ret = real_read_ptr(fd, buf, count);
    if (fd >= 3 && ret > 0 && buf) {
        const unsigned char *b = (const unsigned char *)buf;
        if (ret >= 4 && b[0]=='s' && b[1]=='d' && b[2]=='P' && b[3]=='C') {
            log_msg("READ sdPC fd=%d count=%zd ret=%zd\n", fd, count, ret);
            log_hex("  data: ", buf, (int)(ret > 64 ? 64 : ret));
        }
        else if (ret == 1 && count <= 4 && fd < 256) {
            struct stat st;
            if (fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode)) {
                log_msg("READ 1-byte fd=%d val=0x%02x '%c'\n",
                        fd, b[0], (b[0] >= 0x20 && b[0] < 0x7f) ? b[0] : '.');
            }
        }
    }
    return ret;
}

/* ---- socketpair() ---- */
typedef int (*real_socketpair_fn)(int, int, int, int[2]);
static real_socketpair_fn real_socketpair_ptr = NULL;

int socketpair(int domain, int type, int protocol, int sv[2]) {
    if (!real_socketpair_ptr)
        real_socketpair_ptr = (real_socketpair_fn)dlsym(RTLD_NEXT, "socketpair");
    int ret = real_socketpair_ptr(domain, type, protocol, sv);
    if (ret == 0) {
        log_msg("socketpair(%d,%d,%d) => fd[%d, %d]\n", domain, type, protocol, sv[0], sv[1]);
    }
    return ret;
}

/* ---- bind() ---- */
typedef int (*real_bind_fn)(int, const struct sockaddr *, socklen_t);
static real_bind_fn real_bind_ptr = NULL;

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (!real_bind_ptr)
        real_bind_ptr = (real_bind_fn)dlsym(RTLD_NEXT, "bind");
    int ret = real_bind_ptr(sockfd, addr, addrlen);
    if (addr && addr->sa_family == AF_UNIX) {
        const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
        if (un->sun_path[0] == '\0') {
            log_msg("bind ABSTRACT fd=%d name=%s ret=%d\n", sockfd, un->sun_path+1, ret);
        } else {
            log_msg("bind fd=%d path=%s ret=%d\n", sockfd, un->sun_path, ret);
        }
    }
    return ret;
}

/* ---- connect() ---- */
typedef int (*real_connect_fn)(int, const struct sockaddr *, socklen_t);
static real_connect_fn real_connect_ptr = NULL;

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (!real_connect_ptr)
        real_connect_ptr = (real_connect_fn)dlsym(RTLD_NEXT, "connect");
    int ret = real_connect_ptr(sockfd, addr, addrlen);
    if (addr && addr->sa_family == AF_UNIX) {
        const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
        int saved = errno;
        if (un->sun_path[0] == '\0') {
            log_msg("connect ABSTRACT fd=%d name=%s ret=%d errno=%d\n",
                    sockfd, un->sun_path+1, ret, saved);
        } else if (strstr(un->sun_path, "steam") || strstr(un->sun_path, "shmem") ||
                   strstr(un->sun_path, "chrome")) {
            log_msg("connect fd=%d path=%s ret=%d errno=%d\n",
                    sockfd, un->sun_path, ret, saved);
        }
        errno = saved;
    }
    return ret;
}

/* ---- accept() / accept4() ---- */
typedef int (*real_accept_fn)(int, struct sockaddr *, socklen_t *);
static real_accept_fn real_accept_ptr = NULL;

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    if (!real_accept_ptr)
        real_accept_ptr = (real_accept_fn)dlsym(RTLD_NEXT, "accept");
    int ret = real_accept_ptr(sockfd, addr, addrlen);
    if (ret >= 0) {
        log_msg("accept fd=%d => new_fd=%d\n", sockfd, ret);
    }
    return ret;
}

typedef int (*real_accept4_fn)(int, struct sockaddr *, socklen_t *, int);
static real_accept4_fn real_accept4_ptr = NULL;

int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
    if (!real_accept4_ptr)
        real_accept4_ptr = (real_accept4_fn)dlsym(RTLD_NEXT, "accept4");
    int ret = real_accept4_ptr(sockfd, addr, addrlen, flags);
    if (ret >= 0) {
        log_msg("accept4 fd=%d => new_fd=%d\n", sockfd, ret);
    }
    return ret;
}

/* ---- listen() ---- */
typedef int (*real_listen_fn)(int, int);
static real_listen_fn real_listen_ptr = NULL;

int listen(int sockfd, int backlog) {
    if (!real_listen_ptr)
        real_listen_ptr = (real_listen_fn)dlsym(RTLD_NEXT, "listen");
    int ret = real_listen_ptr(sockfd, backlog);
    log_msg("listen fd=%d backlog=%d ret=%d\n", sockfd, backlog, ret);
    return ret;
}

/* ---- shm_open() ---- */
typedef int (*real_shm_open_fn)(const char *, int, unsigned int);
static real_shm_open_fn real_shm_open_ptr = NULL;

int shm_open(const char *name, int oflag, mode_t mode) {
    if (!real_shm_open_ptr)
        real_shm_open_ptr = (real_shm_open_fn)dlsym(RTLD_NEXT, "shm_open");
    int fd = real_shm_open_ptr(name, oflag, mode);
    log_msg("shm_open(%s, 0x%x, 0%o) => fd=%d\n", name, oflag, mode, fd);
    if (fd >= 0) {
        track_shm_name(name);
        struct stat st;
        if (fstat(fd, &st) == 0)
            log_msg("  shm size=%ld\n", (long)st.st_size);
        /* Dump header right after open */
        unsigned char buf[64];
        ssize_t n = pread(fd, buf, sizeof(buf), 0);
        if (n >= 16) {
            unsigned int *h = (unsigned int *)buf;
            log_msg("  hdr: [0]=%u [1]=%u [2]=%u [3]=%u\n", h[0], h[1], h[2], h[3]);
            if (h[0] != 0 || h[1] != 0 || h[3] != 0)
                log_hex("  raw: ", buf, n > 64 ? 64 : (int)n);
        }
    }
    return fd;
}

/* ---- shm_unlink() ---- */
typedef int (*real_shm_unlink_fn)(const char *);
static real_shm_unlink_fn real_shm_unlink_ptr = NULL;

int shm_unlink(const char *name) {
    if (!real_shm_unlink_ptr)
        real_shm_unlink_ptr = (real_shm_unlink_fn)dlsym(RTLD_NEXT, "shm_unlink");
    log_msg("shm_unlink(%s)\n", name);
    return real_shm_unlink_ptr(name);
}

/* ---- ftruncate() ---- */
typedef int (*real_ftruncate_fn)(int, off_t);
static real_ftruncate_fn real_ftruncate_ptr = NULL;

int ftruncate(int fd, off_t length) {
    if (!real_ftruncate_ptr)
        real_ftruncate_ptr = (real_ftruncate_fn)dlsym(RTLD_NEXT, "ftruncate");
    int ret = real_ftruncate_ptr(fd, length);
    /* Only log if it looks like a shm fd (small fd number or known) */
    if (length > 0 && length < 100000000) {
        char fdlink[64], fdpath[256];
        snprintf(fdlink, sizeof(fdlink), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(fdlink, fdpath, sizeof(fdpath)-1);
        if (n > 0) {
            fdpath[n] = '\0';
            if (strstr(fdpath, "shm") || strstr(fdpath, "Shm_") || strstr(fdpath, "Valve")) {
                log_msg("FTRUNCATE fd=%d path=%s length=%ld ret=%d\n", fd, fdpath, (long)length, ret);
            }
        }
    }
    return ret;
}

__attribute__((constructor))
static void init(void) {
    ensure_log();
    log_msg("=== TRACE v3 pid=%d ppid=%d bits=%d ===\n", getpid(), getppid(), is_32bit ? 32 : 64);

    struct stat st;
    if (fstat(11, &st) == 0) {
        log_msg("FD 11: mode=0x%x %s\n", st.st_mode,
                S_ISSOCK(st.st_mode) ? "SOCKET" :
                S_ISFIFO(st.st_mode) ? "PIPE" : "OTHER");
    } else {
        log_msg("FD 11: not open\n");
    }

    for (int fd = 3; fd < 64; fd++) {
        if (fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode))
            log_msg("  socket fd=%d\n", fd);
    }

    /* Start /dev/shm monitor thread */
    pthread_t tid;
    pthread_create(&tid, NULL, shm_monitor_thread, NULL);
    pthread_detach(tid);
}
