#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    syscall(SYS_write, 2, s, len);
}

static void write_int(const char *prefix, long val) {
    char buf[64];
    char *p = buf;
    const char *s = prefix;
    while (*s) *p++ = *s++;
    if (val < 0) { *p++ = '-'; val = -val; }
    char digits[20];
    int n = 0;
    if (val == 0) digits[n++] = '0';
    else while (val > 0) { digits[n++] = '0' + (val % 10); val /= 10; }
    for (int i = n - 1; i >= 0; i--) *p++ = digits[i];
    *p++ = '\n';
    syscall(SYS_write, 2, buf, p - buf);
}

int main(void) {
    write_str("=== STANDALONE openat TEST ===\n");

    /* List open fds */
    for (int i = 0; i <= 10; i++) {
        char fdpath[32] = "/proc/self/fd/";
        int pos = 14;
        if (i >= 10) { fdpath[pos++] = '1'; fdpath[pos++] = '0'; }
        else fdpath[pos++] = '0' + i;
        fdpath[pos] = 0;
        char linkbuf[256] = {0};
        long lr = syscall(SYS_readlinkat, -100, fdpath, linkbuf, 255);
        if (lr > 0) {
            linkbuf[lr] = 0;
            write_int("fd ", (long)i);
            write_str(" -> ");
            write_str(linkbuf);
            write_str("\n");
        }
    }

    /* Test openat */
    errno = 0;
    long fd = syscall(SYS_openat, -100, "/etc/passwd", O_RDONLY, 0);
    write_int("openat /etc/passwd fd=", fd);
    write_int("errno=", (long)errno);

    if (fd >= 0) {
        struct stat st = {0};
        syscall(SYS_fstat, fd, &st);
        write_int("st_mode=", (long)st.st_mode);
        write_int("st_size=", (long)st.st_size);
        write_int("S_ISREG=", (long)S_ISREG(st.st_mode));

        char buf[80] = {0};
        errno = 0;
        long nr = syscall(SYS_read, fd, buf, 79);
        write_int("read ret=", nr);
        write_int("read errno=", (long)errno);
        if (nr > 0) {
            write_str("DATA: ");
            write_str(buf);
            write_str("\n");
        }
        syscall(SYS_close, fd);
    }

    write_str("=== TEST DONE ===\n");
    return 0;
}
