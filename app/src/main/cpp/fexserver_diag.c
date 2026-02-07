/**
 * FEXServer initialization diagnostic tool.
 *
 * Simulates each step of FEXServer's startup to identify which one fails:
 * 1. Server lock folder creation ($HOME/.fex-emu/Server/)
 * 2. Server lock file creation + flock()
 * 3. Abstract Unix socket creation
 * 4. Filesystem Unix socket creation ($TMPDIR/<uid>.FEXServer.Socket)
 * 5. SquashFS/RootFS config check
 *
 * Run from the app (seccomp context) and from adb (no seccomp) to compare.
 *
 * Environment variables:
 *   HOME   - FEX home dir (e.g., files/fex-home)
 *   TMPDIR - Temp dir for sockets (e.g., cache/tmp)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int uid;

/* Get temp folder using FEXServer's priority order */
static const char* get_temp_folder(void) {
    const char* vars[] = {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP", "TEMPDIR", NULL};
    for (int i = 0; vars[i]; i++) {
        const char* v = getenv(vars[i]);
        if (v && v[0]) return v;
    }
    return "/tmp";
}

/* Get data directory (simplified FEX logic) */
static char data_dir[4096];
static const char* get_data_dir(void) {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";

    /* Check $HOME/.fex-emu/ first (FEX legacy path) */
    snprintf(data_dir, sizeof(data_dir), "%s/.fex-emu", home);
    struct stat st;
    if (stat(data_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        return data_dir;
    }

    /* Fallback: $XDG_DATA_HOME/fex-emu/ or $HOME/.local/share/fex-emu/ */
    const char* xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) {
        snprintf(data_dir, sizeof(data_dir), "%s/fex-emu", xdg);
    } else {
        snprintf(data_dir, sizeof(data_dir), "%s/.local/share/fex-emu", home);
    }
    return data_dir;
}

/* Test 1: Create server lock folder */
static int test_lock_folder(void) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/Server", get_data_dir());
    printf("  Path: %s\n", path);

    struct stat st;
    if (stat(path, &st) == 0) {
        printf("  Already exists (dir=%d)\n", S_ISDIR(st.st_mode));
        return 0;
    }

    if (mkdir(path, 0777) == 0) {
        printf("  Created OK\n");
        return 0;
    }

    /* Try creating parent dirs first */
    char parent[4096];
    snprintf(parent, sizeof(parent), "%s", get_data_dir());
    if (stat(parent, &st) != 0) {
        printf("  Parent dir '%s' doesn't exist, creating...\n", parent);
        if (mkdir(parent, 0777) != 0 && errno != EEXIST) {
            printf("  FAIL: mkdir parent: %s (errno=%d)\n", strerror(errno), errno);
            return -1;
        }
    }

    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        printf("  FAIL: mkdir: %s (errno=%d)\n", strerror(errno), errno);
        return -1;
    }

    printf("  Created OK\n");
    return 0;
}

/* Test 2: Server lock file + flock */
static int test_lock_file(void) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/Server/Server.lock", get_data_dir());
    printf("  Path: %s\n", path);

    /* Check if stale lock exists */
    struct stat st;
    if (stat(path, &st) == 0) {
        printf("  Lock file exists (size=%ld)\n", (long)st.st_size);

        /* Try to open and check lock status */
        int fd = open(path, O_RDWR | O_CLOEXEC, 0777);
        if (fd < 0) {
            printf("  FAIL: open existing: %s (errno=%d)\n", strerror(errno), errno);
            return -1;
        }

        struct flock lk = {
            .l_type = F_WRLCK,
            .l_whence = SEEK_SET,
            .l_start = 0,
            .l_len = 0,
        };

        /* Use F_GETLK to check lock without acquiring */
        struct flock check = lk;
        if (fcntl(fd, F_GETLK, &check) == 0) {
            if (check.l_type == F_UNLCK) {
                printf("  Lock is FREE (stale file, no holder)\n");
            } else {
                printf("  Lock HELD by PID %d (type=%d)\n", check.l_pid,
                       check.l_type);
            }
        }

        /* Try to acquire write lock (non-blocking) */
        int ret = fcntl(fd, F_SETLK, &lk);
        if (ret == 0) {
            printf("  Write lock acquired OK (was stale)\n");

            /* Downgrade to read lock */
            lk.l_type = F_RDLCK;
            ret = fcntl(fd, F_SETLK, &lk);
            if (ret == 0) {
                printf("  Downgraded to read lock OK\n");
            } else {
                printf("  FAIL: downgrade: %s (errno=%d)\n", strerror(errno),
                       errno);
            }
        } else {
            printf("  FAIL: write lock: %s (errno=%d)\n", strerror(errno),
                   errno);
            printf("  Another FEXServer is running!\n");
        }

        close(fd);
        return ret == 0 ? 0 : -1;
    }

    /* Create new lock file */
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_EXCL, 0777);
    if (fd < 0) {
        printf("  FAIL: create: %s (errno=%d)\n", strerror(errno), errno);
        return -1;
    }

    struct flock lk = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };
    int ret = fcntl(fd, F_SETLK, &lk);
    if (ret == 0) {
        printf("  Created + write-locked OK\n");

        /* Downgrade */
        lk.l_type = F_RDLCK;
        ret = fcntl(fd, F_SETLK, &lk);
        printf("  Downgrade: %s\n", ret == 0 ? "OK" : strerror(errno));
    } else {
        printf("  FAIL: write lock on new file: %s (errno=%d)\n",
               strerror(errno), errno);
    }

    close(fd);

    /* Clean up - remove the lock file so FEXServer can create it */
    unlink(path);
    printf("  Cleaned up lock file\n");
    return ret == 0 ? 0 : -1;
}

/* Test 3: Abstract Unix socket */
static int test_abstract_socket(void) {
    char name[256];
    snprintf(name, sizeof(name), "%d.FEXServer.Socket.DiagTest", uid);
    printf("  Name: \\0%s\n", name);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        printf("  FAIL: socket(): %s (errno=%d)\n", strerror(errno), errno);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';  /* Abstract namespace */
    size_t name_len = strlen(name);
    if (name_len >= sizeof(addr.sun_path) - 1) name_len = sizeof(addr.sun_path) - 2;
    memcpy(addr.sun_path + 1, name, name_len);

    socklen_t addr_len = offsetof(struct sockaddr_un, sun_path) + 1 + name_len;

    if (bind(fd, (struct sockaddr*)&addr, addr_len) < 0) {
        printf("  FAIL: bind(): %s (errno=%d)\n", strerror(errno), errno);
        close(fd);
        return -1;
    }

    if (listen(fd, 5) < 0) {
        printf("  FAIL: listen(): %s (errno=%d)\n", strerror(errno), errno);
        close(fd);
        return -1;
    }

    printf("  Abstract socket OK\n");
    close(fd);
    return 0;
}

/* Test 4: Filesystem Unix socket */
static int test_fs_socket(void) {
    const char* tmp = get_temp_folder();
    char path[4096];
    snprintf(path, sizeof(path), "%s/%d.FEXServer.Socket.DiagTest", tmp, uid);
    printf("  Path: %s\n", path);

    /* Check if TMPDIR exists */
    struct stat st;
    if (stat(tmp, &st) != 0) {
        printf("  FAIL: TMPDIR '%s' doesn't exist: %s\n", tmp, strerror(errno));
        return -1;
    }
    printf("  TMPDIR exists (dir=%d, writable=%d)\n",
           S_ISDIR(st.st_mode), access(tmp, W_OK) == 0);

    /* Remove any existing socket file */
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        printf("  FAIL: socket(): %s (errno=%d)\n", strerror(errno), errno);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    socklen_t addr_len = offsetof(struct sockaddr_un, sun_path) + strlen(path);

    if (bind(fd, (struct sockaddr*)&addr, addr_len) < 0) {
        printf("  FAIL: bind(): %s (errno=%d)\n", strerror(errno), errno);
        close(fd);
        return -1;
    }

    if (listen(fd, 5) < 0) {
        printf("  FAIL: listen(): %s (errno=%d)\n", strerror(errno), errno);
        close(fd);
        unlink(path);
        return -1;
    }

    printf("  Filesystem socket OK\n");
    close(fd);
    unlink(path);
    return 0;
}

/* Test 5: Check RootFS lock */
static int test_rootfs_lock(void) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/Server/RootFS.lock", get_data_dir());
    printf("  Path: %s\n", path);

    struct stat st;
    if (stat(path, &st) == 0) {
        printf("  RootFS lock exists (size=%ld)\n", (long)st.st_size);

        int fd = open(path, O_RDWR | O_CLOEXEC, 0777);
        if (fd < 0) {
            printf("  FAIL: open: %s (errno=%d)\n", strerror(errno), errno);
            return -1;
        }

        struct flock lk = {
            .l_type = F_WRLCK,
            .l_whence = SEEK_SET,
            .l_start = 0,
            .l_len = 0,
        };
        struct flock check = lk;
        if (fcntl(fd, F_GETLK, &check) == 0) {
            if (check.l_type == F_UNLCK) {
                printf("  Lock is FREE (stale)\n");
            } else {
                printf("  Lock HELD by PID %d\n", check.l_pid);
            }
        }

        /* Read contents to see mount path */
        char buf[1024] = {0};
        lseek(fd, 0, SEEK_SET);
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            printf("  Content: %s\n", buf);
        }

        close(fd);
    } else {
        printf("  No RootFS lock file (OK for extracted rootfs)\n");
    }
    return 0;
}

/* Test 6: Check FEX config */
static int test_fex_config(void) {
    char path[4096];
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(path, sizeof(path), "%s/.fex-emu/Config.json", home);
    printf("  Path: %s\n", path);

    struct stat st;
    if (stat(path, &st) != 0) {
        printf("  FAIL: Config.json not found: %s\n", strerror(errno));
        return -1;
    }

    printf("  Exists (size=%ld)\n", (long)st.st_size);

    /* Read and print Config.json */
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        char buf[2048] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            printf("  Content: %s\n", buf);
        }
        close(fd);
    }
    return 0;
}

/* Test 7: Check actual FEXServer socket path */
static int test_real_socket_path(void) {
    const char* tmp = get_temp_folder();
    char path[4096];
    snprintf(path, sizeof(path), "%s/%d.FEXServer.Socket", tmp, uid);
    printf("  Expected: %s\n", path);

    struct stat st;
    if (stat(path, &st) == 0) {
        printf("  EXISTS (socket=%d)\n", S_ISSOCK(st.st_mode));
    } else {
        printf("  Not found (expected if FEXServer isn't running)\n");
    }
    return 0;
}

/* Test 8: Check epoll_create (used by fasio reactor) */
static int test_epoll(void) {
    int fd = epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0) {
        printf("  FAIL: epoll_create1: %s (errno=%d)\n", strerror(errno), errno);
        return -1;
    }
    printf("  epoll_create1 OK (fd=%d)\n", fd);
    close(fd);
    return 0;
}

/* Test 9: Check eventfd (used by fasio for async stop) */
#include <sys/eventfd.h>
static int test_eventfd(void) {
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0) {
        printf("  FAIL: eventfd: %s (errno=%d)\n", strerror(errno), errno);
        return -1;
    }
    printf("  eventfd OK (fd=%d)\n", fd);
    close(fd);
    return 0;
}

/* Test 10: Check setsid (FEXServer calls this) */
static int test_setsid(void) {
    /* Fork so we can test setsid without affecting ourselves */
    pid_t pid = fork();
    if (pid == 0) {
        pid_t sid = setsid();
        if (sid < 0) {
            printf("  FAIL: setsid: %s (errno=%d)\n", strerror(errno), errno);
            _exit(1);
        }
        printf("  setsid OK (sid=%d)\n", (int)sid);
        _exit(0);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            return 0;
        }
        /* Child printed the error */
        return -1;
    } else {
        printf("  FAIL: fork: %s\n", strerror(errno));
        return -1;
    }
}

int main(void) {
    uid = getuid();

    printf("=== FEXServer Initialization Diagnostic ===\n\n");
    printf("PID=%d UID=%d\n", getpid(), uid);
    printf("HOME=%s\n", getenv("HOME") ?: "(not set)");
    printf("TMPDIR=%s\n", getenv("TMPDIR") ?: "(not set)");
    printf("XDG_RUNTIME_DIR=%s\n", getenv("XDG_RUNTIME_DIR") ?: "(not set)");
    printf("CWD=%s\n", getcwd(NULL, 0) ?: "(error)");
    printf("DataDir=%s\n", get_data_dir());
    printf("TempFolder=%s\n\n", get_temp_folder());

    struct {
        const char* name;
        int (*test)(void);
    } tests[] = {
        {"Lock folder creation", test_lock_folder},
        {"Lock file + flock", test_lock_file},
        {"Abstract Unix socket", test_abstract_socket},
        {"Filesystem Unix socket", test_fs_socket},
        {"RootFS lock check", test_rootfs_lock},
        {"FEX Config.json", test_fex_config},
        {"Real FEXServer socket", test_real_socket_path},
        {"epoll_create1", test_epoll},
        {"eventfd", test_eventfd},
        {"setsid", test_setsid},
    };

    int num_tests = sizeof(tests) / sizeof(tests[0]);
    int passed = 0, failed = 0;

    for (int i = 0; i < num_tests; i++) {
        printf("[Test %d] %s\n", i + 1, tests[i].name);
        int result = tests[i].test();
        if (result == 0) {
            printf("  => PASS\n\n");
            passed++;
        } else {
            printf("  => FAIL\n\n");
            failed++;
        }
    }

    printf("=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
