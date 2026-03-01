#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    char buf[256];

    write(2, "=== DYNAMIC openat TEST ===\n", 28);

    /* List fds */
    for (int i = 0; i <= 10; i++) {
        char fdpath[32];
        snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", i);
        char linkbuf[256] = {0};
        long lr = syscall(SYS_readlinkat, -100, fdpath, linkbuf, 255);
        if (lr > 0) {
            linkbuf[lr] = 0;
            snprintf(buf, sizeof(buf), "fd %d -> %s\n", i, linkbuf);
            write(2, buf, strlen(buf));
        }
    }

    /* Test openat */
    errno = 0;
    long fd = syscall(SYS_openat, -100, "/etc/passwd", O_RDONLY, 0);
    snprintf(buf, sizeof(buf), "openat /etc/passwd fd=%ld errno=%d\n", fd, errno);
    write(2, buf, strlen(buf));

    if (fd >= 0) {
        struct stat st = {0};
        syscall(SYS_fstat, fd, &st);
        snprintf(buf, sizeof(buf), "mode=%o size=%ld S_ISREG=%d\n",
                 (unsigned)st.st_mode, (long)st.st_size, S_ISREG(st.st_mode));
        write(2, buf, strlen(buf));

        char data[80] = {0};
        errno = 0;
        long nr = syscall(SYS_read, fd, data, 79);
        snprintf(buf, sizeof(buf), "read=%ld errno=%d\n", nr, errno);
        write(2, buf, strlen(buf));
        if (nr > 0) {
            write(2, "DATA: ", 6);
            write(2, data, nr > 60 ? 60 : nr);
            write(2, "\n", 1);
        }
        syscall(SYS_close, fd);
    }

    write(2, "=== DYNAMIC TEST DONE ===\n", 26);
    return 0;
}
