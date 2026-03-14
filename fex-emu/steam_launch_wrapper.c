/*
 * Replacement steam-launch-wrapper for Android/FEX
 * Compiled as static x86-64 ELF — no shebang, no interpreter needed.
 *
 * Parses: steam-launch-wrapper -- reaper SteamLaunch AppId=X -- <actual command...>
 * Skips past two "--" separators, then exec's the remaining args.
 *
 * Build:
 *   x86_64-linux-gnu-gcc -static -O2 -o steam-launch-wrapper steam_launch_wrapper.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

int main(int argc, char *argv[], char *envp[]) {
    FILE *log = fopen("/tmp/slw_debug.log", "a");
    if (log) {
        time_t now = time(NULL);
        fprintf(log, "=== SLW ELF pid=%d ppid=%d time=%ld ===\n", getpid(), getppid(), now);
        fprintf(log, "argc=%d\n", argc);
        for (int i = 0; i < argc; i++)
            fprintf(log, "  argv[%d]=%s\n", i, argv[i]);
    }

    /* Skip past first "--" separator */
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        i++;
    }

    /* Skip past second "--" separator (past reaper args) */
    while (i < argc) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        i++;
    }

    if (i >= argc) {
        if (log) {
            fprintf(log, "ERROR: no command after parsing (i=%d argc=%d)\n", i, argc);
            fclose(log);
        }
        return 1;
    }

    if (log) {
        fprintf(log, "EXEC:");
        for (int j = i; j < argc; j++)
            fprintf(log, " %s", argv[j]);
        fprintf(log, "\n");
        fclose(log);
    }

    /* Build new argv starting from the command */
    execvp(argv[i], &argv[i]);

    /* If exec fails, log it */
    log = fopen("/tmp/slw_debug.log", "a");
    if (log) {
        fprintf(log, "EXEC FAILED: %s (errno=%d)\n", strerror(errno), errno);
        fclose(log);
    }
    return 127;
}
