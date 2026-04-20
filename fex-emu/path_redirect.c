/*
 * path_redirect.c — LD_PRELOAD shim that rewrites filesystem paths before
 * they hit the kernel. Used to make a binary built with baked-in absolute
 * paths (e.g. Pepelespooder's wineserver compiled with
 * --prefix=/data/data/app.gamenative/files/imagefs/opt/proton-10.0.99-arm64ec)
 * work from a differently named Android package.
 *
 * Config at load time via env vars:
 *   REDIRECT_FROM — absolute path prefix to match (e.g. /data/data/app.gamenative)
 *   REDIRECT_TO   — replacement prefix (e.g. /data/data/com.mediatek.steamlauncher/files/proton11)
 *
 * Single prefix rule. If a path starts with REDIRECT_FROM, its prefix is
 * replaced with REDIRECT_TO before the real syscall is invoked. All other
 * paths pass through untouched.
 *
 * Hooked entrypoints cover what wineserver and wine use at startup to
 * locate l_intl.nls and friends: open/openat, access/faccessat, stat/lstat/
 * fstatat, readlink/readlinkat, fopen, execve. Enough surface to unblock
 * the NLS load path; can grow later if more hits show up in strace.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Support up to N prefix rules. Required for our setup because Pepelespooder's
 * wine has baked-in paths under /data/data/app.gamenative/..., while GameNative
 * shim libs (libevshim.so, libandroid-sysvshm.so) have baked-in paths under
 * /data/data/com.winlator.cmod/... . Both need to resolve to our app's private
 * data dir. REDIRECT_FROM/TO is rule 1; REDIRECT_FROM2/TO2 is rule 2. */
#define MAX_RULES 4
struct rule { const char *from; size_t from_len; const char *to; size_t to_len; };
static struct rule g_rules[MAX_RULES];
static int g_rule_count = 0;
static int g_debug = 0;

static void add_rule(const char *from_var, const char *to_var) {
    if (g_rule_count >= MAX_RULES) return;
    const char *f = getenv(from_var);
    const char *t = getenv(to_var);
    if (!f || !t) return;
    g_rules[g_rule_count].from = f;
    g_rules[g_rule_count].from_len = strlen(f);
    g_rules[g_rule_count].to = t;
    g_rules[g_rule_count].to_len = strlen(t);
    g_rule_count++;
}

__attribute__((constructor))
static void init(void) {
    g_debug = getenv("REDIRECT_DEBUG") != NULL;
    add_rule("REDIRECT_FROM", "REDIRECT_TO");
    add_rule("REDIRECT_FROM2", "REDIRECT_TO2");
    add_rule("REDIRECT_FROM3", "REDIRECT_TO3");
    add_rule("REDIRECT_FROM4", "REDIRECT_TO4");
    if (g_debug) {
        fprintf(stderr, "[redirect] init: %d rules\n", g_rule_count);
        for (int i = 0; i < g_rule_count; i++)
            fprintf(stderr, "  [%d] %s -> %s\n", i, g_rules[i].from, g_rules[i].to);
    }
}

/* Returns a buffer owned by the caller (static TLS) if the path needed
 * rewriting, else the original pointer. First matching rule wins. */
static const char *maybe_rewrite(const char *path) {
    static __thread char buf[4096];
    if (!path) return path;
    for (int i = 0; i < g_rule_count; i++) {
        struct rule *r = &g_rules[i];
        if (strncmp(path, r->from, r->from_len) != 0) continue;
        size_t rest_len = strlen(path + r->from_len);
        if (r->to_len + rest_len + 1 > sizeof(buf)) return path;
        memcpy(buf, r->to, r->to_len);
        memcpy(buf + r->to_len, path + r->from_len, rest_len + 1);
        if (g_debug) fprintf(stderr, "[redirect] %s -> %s\n", path, buf);
        return buf;
    }
    return path;
}

/* -------- open family -------- */

typedef int (*open_fn)(const char *, int, ...);
typedef int (*openat_fn)(int, const char *, int, ...);

int open(const char *pathname, int flags, ...) {
    static open_fn real = NULL;
    if (!real) real = (open_fn)dlsym(RTLD_NEXT, "open");
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return real(maybe_rewrite(pathname), flags, mode);
}

int openat(int dirfd, const char *pathname, int flags, ...) {
    static openat_fn real = NULL;
    if (!real) real = (openat_fn)dlsym(RTLD_NEXT, "openat");
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return real(dirfd, maybe_rewrite(pathname), flags, mode);
}

int open64(const char *pathname, int flags, ...) {
    static open_fn real = NULL;
    if (!real) real = (open_fn)dlsym(RTLD_NEXT, "open64");
    if (!real) real = (open_fn)dlsym(RTLD_NEXT, "open");
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return real(maybe_rewrite(pathname), flags, mode);
}

/* -------- access family -------- */

int access(const char *pathname, int mode) {
    static int (*real)(const char *, int) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "access");
    return real(maybe_rewrite(pathname), mode);
}

int faccessat(int dirfd, const char *pathname, int mode, int flags) {
    static int (*real)(int, const char *, int, int) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "faccessat");
    return real(dirfd, maybe_rewrite(pathname), mode, flags);
}

/* -------- stat family -------- */

int stat(const char *pathname, struct stat *statbuf) {
    static int (*real)(const char *, struct stat *) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "stat");
    return real(maybe_rewrite(pathname), statbuf);
}

int lstat(const char *pathname, struct stat *statbuf) {
    static int (*real)(const char *, struct stat *) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "lstat");
    return real(maybe_rewrite(pathname), statbuf);
}

int fstatat(int dirfd, const char *pathname, struct stat *statbuf, int flags) {
    static int (*real)(int, const char *, struct stat *, int) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "fstatat");
    return real(dirfd, maybe_rewrite(pathname), statbuf, flags);
}

int stat64(const char *pathname, struct stat64 *statbuf) {
    static int (*real)(const char *, struct stat64 *) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "stat64");
    return real(maybe_rewrite(pathname), statbuf);
}

int lstat64(const char *pathname, struct stat64 *statbuf) {
    static int (*real)(const char *, struct stat64 *) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "lstat64");
    return real(maybe_rewrite(pathname), statbuf);
}

/* -------- readlink family -------- */

ssize_t readlink(const char *pathname, char *buf, size_t bufsiz) {
    static ssize_t (*real)(const char *, char *, size_t) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "readlink");
    return real(maybe_rewrite(pathname), buf, bufsiz);
}

ssize_t readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz) {
    static ssize_t (*real)(int, const char *, char *, size_t) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "readlinkat");
    return real(dirfd, maybe_rewrite(pathname), buf, bufsiz);
}

/* -------- fopen -------- */

FILE *fopen(const char *pathname, const char *mode) {
    static FILE *(*real)(const char *, const char *) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "fopen");
    return real(maybe_rewrite(pathname), mode);
}

/* -------- execve -------- */

int execve(const char *pathname, char *const argv[], char *const envp[]) {
    static int (*real)(const char *, char *const[], char *const[]) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "execve");
    return real(maybe_rewrite(pathname), argv, envp);
}

/* -------- opendir -------- */
#include <dirent.h>
DIR *opendir(const char *name) {
    static DIR *(*real)(const char *) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "opendir");
    return real(maybe_rewrite(name));
}

/* NOTE: attempted to hook dlsym() to redirect libevshim's
 * dlsym(libc_handle, "open") to our open. But returning NULL for
 * non-intercepted symbols (when __loader_dlsym wasn't available)
 * caused an infinite loop — libevshim retries on NULL. Reverted.
 *
 * TODO: fix this via a different mechanism — either use Android's
 * __libc_dlsym, bind directly to libdl's dlsym entry, or preload a
 * custom libc stub that wraps open itself. For now, leave dlsym
 * unhooked; libevshim will keep failing to open gamepad.mem but the
 * process won't hang. */

/* -------- mmap: force PE DLL/EXE file-backed mmaps to fail so Wine takes its
 * pread-into-anon fallback. Otherwise mmap(PROT_READ|WRITE) on a .dll in
 * app_data_file context succeeds but the later mprotect(PROT_EXEC) is denied
 * by SELinux's `execmod` rule. Making wine read into anon memory instead
 * sidesteps this — mprotect(PROT_EXEC) on anon memory uses `execmem` which
 * is allowed for untrusted_app. */

/* Detect PE files by the MZ magic (0x4D 0x5A) at offset 0. Matches .dll,
 * .exe, .drv, .cpl, .ocx, and anything else wine loads as a PE image,
 * regardless of filename. pread() doesn't disturb the fd's file position. */
static int fd_is_pe(int fd) {
    if (fd < 0) return 0;
    unsigned char magic[2];
    if (pread(fd, magic, 2, 0) != 2) return 0;
    return magic[0] == 'M' && magic[1] == 'Z';
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    static void *(*real)(void *, size_t, int, int, int, off_t) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "mmap");
    if (fd >= 0 && (flags & MAP_PRIVATE) && !(flags & MAP_ANONYMOUS)) {
        if (fd_is_pe(fd)) {
            if (g_debug) fprintf(stderr, "[redirect] mmap fd=%d on PE file, forcing EPERM\n", fd);
            errno = EPERM;
            return MAP_FAILED;
        }
    }
    return real(addr, length, prot, flags, fd, offset);
}

void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    static void *(*real)(void *, size_t, int, int, int, off_t) = NULL;
    if (!real) real = dlsym(RTLD_NEXT, "mmap64");
    if (!real) real = dlsym(RTLD_NEXT, "mmap");
    if (fd >= 0 && (flags & MAP_PRIVATE) && !(flags & MAP_ANONYMOUS)) {
        if (fd_is_pe(fd)) {
            if (g_debug) fprintf(stderr, "[redirect] mmap64 fd=%d on PE file, forcing EPERM\n", fd);
            errno = EPERM;
            return MAP_FAILED;
        }
    }
    return real(addr, length, prot, flags, fd, offset);
}
