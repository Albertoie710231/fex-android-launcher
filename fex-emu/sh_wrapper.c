/*
 * Replacement /bin/sh for Android/FEX
 *
 * Problem: dash exits immediately (code 0) when Steam calls
 *   sh -c "steam-launch-wrapper -- reaper ... -- proton waitforexitandrun game.exe"
 * The SLW binary never executes. Root cause unknown (dash exec optimization? FEX issue?).
 *
 * Solution: For "sh -c" invocations containing steam-launch-wrapper, parse the
 * command string and exec the SLW directly, bypassing dash. For everything else,
 * exec real dash normally.
 *
 * Build:
 *   x86_64-linux-gnu-gcc -static -O2 -o sh_wrapper sh_wrapper.c
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>

#define MAX_ARGS 64
#define LOG_PATH "/tmp/sh_wrapper.log"

static void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_msg(const char *fmt, ...) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

/*
 * Parse a shell command string into argv, handling single quotes.
 * Only handles the subset of shell syntax Steam actually uses:
 * - Single-quoted strings (no escapes inside)
 * - Unquoted words separated by whitespace
 * Returns argc.
 */
static int parse_sh_command(char *cmd, char **argv, int max_args) {
    int argc = 0;
    char *p = cmd;

    while (*p && argc < max_args - 1) {
        /* Skip whitespace */
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        char *start = p;
        char *out = p; /* write pointer for in-place unquoting */

        while (*p && !isspace((unsigned char)*p)) {
            if (*p == '\'') {
                /* Single-quoted string — copy contents literally */
                p++; /* skip opening quote */
                while (*p && *p != '\'') {
                    *out++ = *p++;
                }
                if (*p == '\'') p++; /* skip closing quote */
            } else {
                *out++ = *p++;
            }
        }
        if (*p) {
            *out = '\0';
            p++;
        } else {
            *out = '\0';
        }
        argv[argc++] = start;
    }
    argv[argc] = NULL;
    return argc;
}

int main(int argc, char *argv[], char *envp[]) {
    /* Log all invocations */
    log_msg("=== sh pid=%d ppid=%d ===\n", getpid(), getppid());
    for (int i = 0; i < argc; i++)
        log_msg("  [%d] %s\n", i, argv[i]);
    log_msg("---\n");

    /*
     * Detect: sh -c "...steam-launch-wrapper..."
     * If found, parse the command string ourselves and exec SLW directly.
     */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0 &&
        strstr(argv[2], "steam-launch-wrapper") != NULL) {

        log_msg("SH_WRAPPER: intercepting SLW command, parsing directly\n");

        /* Make a mutable copy of the command string */
        char *cmd = strdup(argv[2]);
        if (!cmd) {
            log_msg("SH_WRAPPER: strdup failed\n");
            _exit(1);
        }

        char *parsed_argv[MAX_ARGS];
        int parsed_argc = parse_sh_command(cmd, parsed_argv, MAX_ARGS);

        log_msg("SH_WRAPPER: parsed %d args:\n", parsed_argc);
        for (int i = 0; i < parsed_argc; i++)
            log_msg("  [%d] %s\n", i, parsed_argv[i]);

        if (parsed_argc > 0) {
            log_msg("SH_WRAPPER: exec %s\n", parsed_argv[0]);
            execv(parsed_argv[0], parsed_argv);
            log_msg("SH_WRAPPER: exec FAILED: %s (errno=%d)\n",
                    strerror(errno), errno);
        }
        _exit(127);
    }

    /* Default: exec real dash */
    argv[0] = "/bin/dash";
    execv("/bin/dash", argv);
    log_msg("SH_WRAPPER: dash exec failed: %s\n", strerror(errno));
    return 127;
}
