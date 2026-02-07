/*
 * seccomp_test.c - Identifies which syscalls Android's seccomp filter blocks
 *
 * Android app processes inherit a strict seccomp filter from zygote.
 * FEX-Emu dies with SIGSYS (exit code 159) because it makes a syscall
 * that this filter blocks. This tool tests each suspect syscall in a
 * forked child process to safely identify the blocked one(s).
 *
 * Run from app ProcessBuilder: shows which syscalls are blocked by seccomp
 * Run via adb run-as: all syscalls should pass (adb has no seccomp)
 * Compare the two outputs to find the problem.
 *
 * Build: NDK packages this as libseccomp_test.so in nativeLibraryDir
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/personality.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>

/* ---- Syscall number defines (aarch64) ---- */

#ifndef __NR_memfd_create
#define __NR_memfd_create 279
#endif
#ifndef __NR_getrandom
#define __NR_getrandom 278
#endif
#ifndef __NR_rseq
#define __NR_rseq 293
#endif
#ifndef __NR_clone3
#define __NR_clone3 435
#endif
#ifndef __NR_userfaultfd
#define __NR_userfaultfd 282
#endif
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup 425
#endif
#ifndef __NR_set_robust_list
#define __NR_set_robust_list 99
#endif
#ifndef __NR_sched_getaffinity
#define __NR_sched_getaffinity 123
#endif
#ifndef __NR_process_vm_readv
#define __NR_process_vm_readv 270
#endif
#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif

#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 2
#endif

/* FEX-specific prctl constants (kernel patches, not in standard headers) */
#ifndef PR_GET_MEM_MODEL
#define PR_GET_MEM_MODEL 0x6d4d01
#endif
#ifndef PR_SET_MEM_MODEL
#define PR_SET_MEM_MODEL 0x6d4d02
#endif
#ifndef PR_SET_MEM_MODEL_TSO
#define PR_SET_MEM_MODEL_TSO 1
#endif
#ifndef PR_GET_COMPAT_INPUT
#define PR_GET_COMPAT_INPUT 67
#endif
#ifndef PR_SET_COMPAT_INPUT
#define PR_SET_COMPAT_INPUT 68
#endif
#ifndef PR_SET_COMPAT_INPUT_ENABLE
#define PR_SET_COMPAT_INPUT_ENABLE 1
#endif
#ifndef PR_GET_SHADOW_STACK_STATUS
#define PR_GET_SHADOW_STACK_STATUS 74
#endif
#ifndef PR_LOCK_SHADOW_STACK_STATUS
#define PR_LOCK_SHADOW_STACK_STATUS 75
#endif
#ifndef PR_ARM64_SET_UNALIGN_ATOMIC
#define PR_ARM64_SET_UNALIGN_ATOMIC 0x41524d01
#endif

/* ---- Test framework ---- */

static void test_syscall(const char *name, void (*test_fn)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: try the syscall */
        test_fn();
        _exit(0);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code == 0) {
                printf("  %-45s OK\n", name);
            } else {
                printf("  %-45s EPERM/ENOSYS (err=%d)\n", name, code);
            }
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            if (sig == 31) {
                printf("  %-45s ** SECCOMP KILL (SIGSYS) **\n", name);
            } else {
                printf("  %-45s SIGNAL %d (%s)\n", name, sig, strsignal(sig));
            }
        } else {
            printf("  %-45s UNKNOWN status 0x%x\n", name, status);
        }
    } else {
        printf("  %-45s FORK FAILED\n", name);
    }
}

/* ---- Individual syscall tests ---- */

/* Each test function tries one syscall. If the syscall returns an expected
 * error (ENOSYS, EINVAL, EPERM, EFAULT), that's OK — it means the seccomp
 * filter ALLOWED the syscall through, but the kernel rejected the arguments.
 * Only SIGSYS (signal 31) means seccomp blocked it. */

static void test_personality_query(void) {
    long p = personality(0xffffffff);
    if (p == -1) _exit(errno);
}

static void test_personality_set_same(void) {
    long p = personality(0xffffffff);
    if (p == -1) _exit(errno);
    if (personality(p) == -1) _exit(errno);
}

static void test_personality_read_implies_exec(void) {
    long p = personality(0xffffffff);
    if (p == -1) _exit(errno);
    if (personality(p | READ_IMPLIES_EXEC) == -1) _exit(errno);
}

static void test_personality_addr_no_randomize(void) {
    long p = personality(0xffffffff);
    if (p == -1) _exit(errno);
    if (personality(p | ADDR_NO_RANDOMIZE) == -1) _exit(errno);
}

static void test_prctl_set_name(void) {
    if (prctl(PR_SET_NAME, "test", 0, 0, 0) == -1) _exit(errno);
}

static void test_prctl_set_child_subreaper(void) {
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == -1) _exit(errno);
}

static void test_prctl_set_mm(void) {
    /* PR_SET_MM requires CAP_SYS_RESOURCE. We expect EPERM, not SIGSYS. */
    int r = prctl(PR_SET_MM, PR_SET_MM_MAP, NULL, 0, 0);
    if (r == -1 && (errno == EPERM || errno == EINVAL || errno == EFAULT)) _exit(0);
    if (r == -1) _exit(errno);
}

static void test_prctl_get_mem_model(void) {
    int r = prctl(PR_GET_MEM_MODEL, 0, 0, 0, 0);
    if (r == -1 && (errno == EINVAL || errno == ENOSYS)) _exit(0);
    if (r == -1) _exit(errno);
}

static void test_prctl_set_mem_model_tso(void) {
    int r = prctl(PR_SET_MEM_MODEL, PR_SET_MEM_MODEL_TSO, 0, 0, 0);
    if (r == -1 && (errno == EINVAL || errno == ENOSYS)) _exit(0);
    if (r == -1) _exit(errno);
}

static void test_prctl_get_compat_input(void) {
    int r = prctl(PR_GET_COMPAT_INPUT, 0, 0, 0, 0);
    if (r == -1 && (errno == EINVAL || errno == ENOSYS)) _exit(0);
    if (r == -1) _exit(errno);
}

static void test_prctl_set_compat_input(void) {
    int r = prctl(PR_SET_COMPAT_INPUT, PR_SET_COMPAT_INPUT_ENABLE, 0, 0, 0);
    if (r == -1 && (errno == EINVAL || errno == ENOSYS)) _exit(0);
    if (r == -1) _exit(errno);
}

static void test_prctl_get_shadow_stack(void) {
    unsigned long val = 0;
    int r = prctl(PR_GET_SHADOW_STACK_STATUS, &val, 0, 0, 0);
    if (r == -1 && (errno == EINVAL || errno == ENOSYS)) _exit(0);
    if (r == -1) _exit(errno);
}

static void test_prctl_lock_shadow_stack(void) {
    int r = prctl(PR_LOCK_SHADOW_STACK_STATUS, ~0ULL, 0, 0, 0);
    if (r == -1 && (errno == EINVAL || errno == ENOSYS || errno == EPERM)) _exit(0);
    if (r == -1) _exit(errno);
}

static void test_prctl_arm64_unalign_atomic(void) {
    int r = prctl(PR_ARM64_SET_UNALIGN_ATOMIC, 0, 0, 0, 0);
    if (r == -1 && (errno == EINVAL || errno == ENOSYS)) _exit(0);
    if (r == -1) _exit(errno);
}

static void test_memfd_create(void) {
    int fd = syscall(__NR_memfd_create, "test", MFD_ALLOW_SEALING);
    if (fd == -1) _exit(errno);
    close(fd);
}

static void test_getrandom(void) {
    char buf[8];
    long r = syscall(__NR_getrandom, buf, sizeof(buf), 0);
    if (r == -1) _exit(errno);
}

static void test_rseq(void) {
    /* rseq is called by glibc 2.35+ during _dl_start (before constructors!) */
    long r = syscall(__NR_rseq, NULL, 0, 0, 0);
    if (r == -1 && (errno == EINVAL || errno == EFAULT || errno == ENOSYS || errno == EPERM || errno == EBUSY)) _exit(0);
    if (r == -1) _exit(errno);
}

static void test_clone3(void) {
    /* Use NULL args to get EINVAL without actually creating a process */
    long r = syscall(__NR_clone3, NULL, 0);
    if (r == -1 && (errno == EINVAL || errno == EFAULT)) _exit(0);
    if (r == -1) _exit(errno);
}

static void test_setsid(void) {
    long r = setsid();
    if (r == -1 && errno == EPERM) _exit(0);
    if (r == -1) _exit(errno);
}

static void test_mmap_stack(void) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (p == MAP_FAILED) _exit(errno);
    munmap(p, 4096);
}

static void test_mmap_growsdown(void) {
    void *p = mmap(NULL, 65536, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN | MAP_NORESERVE,
                   -1, 0);
    if (p == MAP_FAILED) _exit(errno);
    munmap(p, 65536);
}

static void test_set_robust_list(void) {
    long r = syscall(__NR_set_robust_list, NULL, 0);
    if (r == -1 && (errno == EINVAL || errno == EFAULT)) _exit(0);
    if (r == -1) _exit(errno);
}

static void test_sched_getaffinity(void) {
    unsigned long mask = 0;
    long r = syscall(__NR_sched_getaffinity, 0, sizeof(mask), &mask);
    if (r == -1) _exit(errno);
}

static void test_userfaultfd(void) {
    int fd = syscall(__NR_userfaultfd, 0);
    if (fd == -1 && (errno == EPERM || errno == ENOSYS)) _exit(0);
    if (fd == -1) _exit(errno);
    close(fd);
}

static void test_io_uring_setup(void) {
    long r = syscall(__NR_io_uring_setup, 0, NULL);
    if (r == -1 && (errno == EINVAL || errno == EFAULT || errno == ENOSYS || errno == EPERM)) _exit(0);
    if (r == -1) _exit(errno);
}

static void test_pidfd_open(void) {
    int fd = syscall(__NR_pidfd_open, getpid(), 0);
    if (fd == -1 && (errno == EINVAL || errno == ENOSYS || errno == EPERM)) _exit(0);
    if (fd == -1) _exit(errno);
    close(fd);
}

static void test_process_vm_readv(void) {
    /* Try to read our own memory — should fail with EPERM or succeed */
    char buf[16];
    struct iovec local = { .iov_base = buf, .iov_len = sizeof(buf) };
    struct iovec remote = { .iov_base = buf, .iov_len = sizeof(buf) };
    long r = syscall(__NR_process_vm_readv, getpid(), &local, 1, &remote, 1, 0);
    if (r == -1 && (errno == EPERM || errno == ENOSYS)) _exit(0);
    if (r == -1) _exit(errno);
}

static void test_sigaction_sigsys(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    if (sigaction(SIGSYS, &sa, NULL) == -1) _exit(errno);
}

/* ---- Also test running glibc ld.so ---- */

static void test_glibc_ldso(void) {
    /* If LD_SO_PATH env is set, try running glibc ld.so --help
     * to see if glibc init itself triggers seccomp */
    const char *ldso = getenv("SECCOMP_TEST_LDSO");
    if (ldso) {
        printf("\n--- glibc ld.so test ---\n");
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            execl(ldso, ldso, "--help", NULL);
            _exit(errno);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status)) {
                printf("  %-45s exit=%d\n", "ld.so --help", WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                if (sig == 31)
                    printf("  %-45s ** SECCOMP KILL (SIGSYS) **\n", "ld.so --help");
                else
                    printf("  %-45s SIGNAL %d (%s)\n", "ld.so --help", sig, strsignal(sig));
            }
        }
    }
}

/* ---- Main ---- */

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL); /* unbuffered output */

    printf("=== Android Seccomp Syscall Test ===\n");
    printf("PID=%d  UID=%d  PPID=%d\n", getpid(), getuid(), getppid());
    printf("Compare: run via app (seccomp) vs adb run-as (no seccomp)\n\n");

    printf("--- personality() [FEX: ELFCodeLoader.h:414] ---\n");
    test_syscall("personality(QUERY)", test_personality_query);
    test_syscall("personality(SET_SAME)", test_personality_set_same);
    test_syscall("personality(READ_IMPLIES_EXEC)", test_personality_read_implies_exec);
    test_syscall("personality(ADDR_NO_RANDOMIZE)", test_personality_addr_no_randomize);

    printf("\n--- prctl() [FEX: various] ---\n");
    test_syscall("prctl(PR_SET_NAME)", test_prctl_set_name);
    test_syscall("prctl(PR_SET_CHILD_SUBREAPER)", test_prctl_set_child_subreaper);
    test_syscall("prctl(PR_SET_MM) [ELFCodeLoader:807]", test_prctl_set_mm);
    test_syscall("prctl(PR_GET_MEM_MODEL) [FEXInterp:283]", test_prctl_get_mem_model);
    test_syscall("prctl(PR_SET_MEM_MODEL,TSO) [FEXInterp:295]", test_prctl_set_mem_model_tso);
    test_syscall("prctl(PR_GET_COMPAT_INPUT) [FEXInterp:311]", test_prctl_get_compat_input);
    test_syscall("prctl(PR_SET_COMPAT_INPUT) [FEXInterp:316]", test_prctl_set_compat_input);
    test_syscall("prctl(PR_GET_SHADOW_STACK) [FEXInterp:328]", test_prctl_get_shadow_stack);
    test_syscall("prctl(PR_LOCK_SHADOW_STACK) [FEXInterp:333]", test_prctl_lock_shadow_stack);
    test_syscall("prctl(PR_ARM64_UNALIGN_ATOMIC) [FEXInterp:356]", test_prctl_arm64_unalign_atomic);

    printf("\n--- Memory/FD syscalls ---\n");
    test_syscall("memfd_create() [SeccompEmulator:216]", test_memfd_create);
    test_syscall("mmap(MAP_STACK)", test_mmap_stack);
    test_syscall("mmap(MAP_GROWSDOWN) [ELFCodeLoader:478]", test_mmap_growsdown);
    test_syscall("getrandom() [ELFCodeLoader:218]", test_getrandom);

    printf("\n--- Process/thread syscalls ---\n");
    test_syscall("clone3() [Syscalls.cpp:604]", test_clone3);
    test_syscall("setsid() [FEXServer:226]", test_setsid);
    test_syscall("sched_getaffinity()", test_sched_getaffinity);
    test_syscall("set_robust_list()", test_set_robust_list);

    printf("\n--- glibc init / newer syscalls ---\n");
    test_syscall("rseq() [glibc 2.38 _dl_start]", test_rseq);
    test_syscall("userfaultfd()", test_userfaultfd);
    test_syscall("io_uring_setup()", test_io_uring_setup);
    test_syscall("pidfd_open()", test_pidfd_open);
    test_syscall("process_vm_readv()", test_process_vm_readv);
    test_syscall("sigaction(SIGSYS)", test_sigaction_sigsys);

    /* Optional: test glibc ld.so itself */
    test_glibc_ldso();

    printf("\n=== Legend ===\n");
    printf("  OK              = syscall allowed by seccomp\n");
    printf("  EPERM/ENOSYS    = syscall allowed but kernel rejected args\n");
    printf("  SECCOMP KILL    = blocked by seccomp filter (this kills FEX!)\n");

    return 0;
}
