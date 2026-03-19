/*
 * Vortek VM-side bridge: Unix socket → TCP to host.
 * Runs inside the QEMU VM. Creates a Unix socket at /tmp/vortek.sock
 * and forwards all traffic to the host's TCP proxy via QEMU slirp.
 *
 * Compile for ARM64 Linux (inside VM or cross-compile):
 *   aarch64-linux-gnu-gcc -static -o vortek_vm_bridge vortek_vm_bridge.c -lpthread
 *   OR with NDK: aarch64-linux-android26-clang -o vortek_vm_bridge vortek_vm_bridge.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>

#define BUFFER_SIZE (64 * 1024)

struct relay_args {
    int from_fd;
    int to_fd;
};

static void *relay_thread(void *arg) {
    struct relay_args *a = (struct relay_args *)arg;
    char buf[BUFFER_SIZE];
    ssize_t n;

    while ((n = read(a->from_fd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(a->to_fd, buf + written, n - written);
            if (w <= 0) goto done;
            written += w;
        }
    }
done:
    shutdown(a->from_fd, SHUT_RD);
    shutdown(a->to_fd, SHUT_WR);
    free(a);
    return NULL;
}

static int connect_tcp(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv) {
    const char *unix_path = "/tmp/vortek.sock";
    const char *tcp_host = "10.0.2.2";  /* QEMU slirp host */
    int tcp_port = 5900;

    if (argc >= 2) unix_path = argv[1];
    if (argc >= 3) tcp_host = argv[2];
    if (argc >= 4) tcp_port = atoi(argv[3]);

    signal(SIGPIPE, SIG_IGN);

    unlink(unix_path);

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, unix_path, sizeof(addr.sun_path) - 1);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 5) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    /* Also create /tmp/.vortek/V0 symlink */
    mkdir("/tmp/.vortek", 0755);
    unlink("/tmp/.vortek/V0");
    symlink(unix_path, "/tmp/.vortek/V0");

    fprintf(stderr, "Vortek VM bridge: %s → %s:%d\n", unix_path, tcp_host, tcp_port);

    while (1) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        int tcp_fd = connect_tcp(tcp_host, tcp_port);
        if (tcp_fd < 0) {
            fprintf(stderr, "Failed to connect to host %s:%d: %s\n",
                    tcp_host, tcp_port, strerror(errno));
            close(client_fd);
            continue;
        }

        fprintf(stderr, "Bridging Unix→TCP connection\n");

        pthread_t t1, t2;
        struct relay_args *a1 = malloc(sizeof(*a1));
        a1->from_fd = client_fd; a1->to_fd = tcp_fd;
        struct relay_args *a2 = malloc(sizeof(*a2));
        a2->from_fd = tcp_fd; a2->to_fd = client_fd;

        pthread_create(&t1, NULL, relay_thread, a1);
        pthread_create(&t2, NULL, relay_thread, a2);
        pthread_join(t1, NULL);
        pthread_join(t2, NULL);

        close(client_fd);
        close(tcp_fd);
    }

    close(listen_fd);
    unlink(unix_path);
    return 0;
}
