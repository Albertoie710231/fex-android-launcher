/*
 * Minimal VM init for AVF smoke test.
 * Compiled as static ARM64 binary via Android NDK.
 * No busybox/shell needed — does everything in C.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/utsname.h>
#include <linux/reboot.h>

static void read_file(const char *path, char *buf, size_t len) {
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(buf, len, f) == NULL)
            buf[0] = '\0';
        /* strip newline */
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';
        fclose(f);
    } else {
        snprintf(buf, len, "(not found)");
    }
}

static void write_file(const char *path, const char *value) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(value, f);
        fclose(f);
    }
}

int main(void) {
    char buf[256];
    struct utsname uts;

    /* Mount essential filesystems */
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sys", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);

    printf("\n");
    printf("=========================================\n");
    printf("  AVF Smoke Test - VM Booted Successfully\n");
    printf("=========================================\n\n");

    if (uname(&uts) == 0) {
        printf("Kernel:  %s %s\n", uts.release, uts.machine);
    }

    /* Memory info */
    read_file("/proc/meminfo", buf, sizeof(buf));
    printf("Memory:  %s\n", buf);

    /* CPU count */
    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    int cpus = 0;
    if (cpuinfo) {
        char line[256];
        while (fgets(line, sizeof(line), cpuinfo))
            if (strncmp(line, "processor", 9) == 0)
                cpus++;
        fclose(cpuinfo);
    }
    printf("CPUs:    %d\n\n", cpus);

    /* THE KEY TEST: vm.max_map_count */
    read_file("/proc/sys/vm/max_map_count", buf, sizeof(buf));
    printf("vm.max_map_count (before): %s\n", buf);

    write_file("/proc/sys/vm/max_map_count", "1048576\n");

    read_file("/proc/sys/vm/max_map_count", buf, sizeof(buf));
    printf("vm.max_map_count (after):  %s\n", buf);

    if (strcmp(buf, "1048576") == 0) {
        printf("\n*** SUCCESS: vm.max_map_count = 1048576 ***\n");
        printf("*** Webhelper will survive in this VM! ***\n");
    } else {
        printf("\n*** FAILED: Could not set vm.max_map_count ***\n");
    }

    printf("\nSmoke test complete. Shutting down.\n");
    fflush(stdout);

    sleep(2);

    /* Power off */
    reboot(LINUX_REBOOT_CMD_POWER_OFF);

    return 0;
}
