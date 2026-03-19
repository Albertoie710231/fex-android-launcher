/*
 * Vortek TCP-to-Unix socket proxy for QEMU VM GPU passthrough.
 *
 * Runs on the Android host. Listens on a TCP port and relays all traffic
 * bidirectionally to the Vortek Unix socket.
 *
 * VM (FEX/Vortek client) → TCP → QEMU hostfwd → this proxy → Unix socket → VortekRenderer → Mali GPU
 *
 * Compile for Android ARM64:
 *   $NDK/bin/aarch64-linux-android26-clang -static -o vortek_proxy vortek_tcp_proxy.c android_tls_fix.o
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
#include <poll.h>

#define BUFFER_SIZE (64 * 1024)  /* 64KB buffer for Vulkan commands */

static volatile int running = 1;

struct relay_args {
    int from_fd;
    int to_fd;
    const char *label;
};

static void *relay_thread(void *arg) {
    struct relay_args *a = (struct relay_args *)arg;
    char buf[BUFFER_SIZE];
    ssize_t n;

    while (running) {
        n = read(a->from_fd, buf, sizeof(buf));
        if (n <= 0) break;

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

static int connect_unix(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void handle_client(int client_fd, const char *unix_path) {
    int unix_fd = connect_unix(unix_path);
    if (unix_fd < 0) {
        fprintf(stderr, "Failed to connect to Vortek socket %s: %s\n",
                unix_path, strerror(errno));
        close(client_fd);
        return;
    }

    fprintf(stderr, "Client connected, bridging TCP↔Unix\n");

    /* Start two relay threads: TCP→Unix and Unix→TCP */
    pthread_t t1, t2;

    struct relay_args *a1 = malloc(sizeof(*a1));
    a1->from_fd = client_fd;
    a1->to_fd = unix_fd;
    a1->label = "TCP→Unix";

    struct relay_args *a2 = malloc(sizeof(*a2));
    a2->from_fd = unix_fd;
    a2->to_fd = client_fd;
    a2->label = "Unix→TCP";

    pthread_create(&t1, NULL, relay_thread, a1);
    pthread_create(&t2, NULL, relay_thread, a2);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    close(client_fd);
    close(unix_fd);
    fprintf(stderr, "Client disconnected\n");
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <tcp_port> <vortek_unix_socket_path>\n", argv[0]);
        fprintf(stderr, "Example: %s 5900 /data/local/tmp/vortek.sock\n", argv[0]);
        return 1;
    }

    int tcp_port = atoi(argv[1]);
    const char *unix_path = argv[2];

    signal(SIGPIPE, SIG_IGN);

    /* Create TCP listen socket */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(tcp_port);

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

    fprintf(stderr, "Vortek TCP proxy listening on port %d → %s\n", tcp_port, unix_path);

    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        /* Handle each client in a new thread */
        pthread_t t;
        int *cfd = malloc(sizeof(int));
        *cfd = client_fd;

        /* For simplicity, handle one client at a time (Vortek is single-connection) */
        handle_client(client_fd, unix_path);
    }

    close(listen_fd);
    return 0;
}
