/*
 * nullroute launcher
 *
 * NULLROUTE is written in POSIX sh. This program is the small native entry
 * point used on Windows (and handy on Linux): it locates the game script
 * next to itself and runs it with a POSIX shell.
 *
 *   Linux / macOS : cc -std=c99 -Wall -Wextra -D_POSIX_C_SOURCE=200809L
 *                   execs $NULLROUTE_SH (default /bin/sh)
 *   Windows       : x86_64-w64-mingw32-gcc -std=c99 -Wall -Wextra
 *                   runs $NULLROUTE_SH (default sh.exe on PATH, which Git for
 *                   Windows and MSYS2 both provide)
 *
 * Package layout expected by this launcher:
 *   nullroute[.exe]
 *   game/bin/nullroute
 *   game/lib/ (sh libraries)
 *   game/world/...
 */

#ifndef _WIN32
/* realpath() and friends come from POSIX.1-2008. */
#define _XOPEN_SOURCE 700
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef _WIN32
#include <process.h>
#include <stdint.h>
#include <windows.h>
#define DIR_SEP '\\'
#define DEFAULT_SHELL "sh.exe"
#else
#include <limits.h>
#include <sys/types.h>
#include <unistd.h>
#define DIR_SEP '/'
#define DEFAULT_SHELL "/bin/sh"
#endif

#define VERSION "0.1.0"

/* Returns 1 if the file can be opened for reading. */
static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    fclose(f);
    return 1;
}

/* Removes the last path component in place. Returns 0 if there was none. */
static int strip_last(char *path)
{
    char *last = strrchr(path, DIR_SEP);
#ifdef _WIN32
    char *fwd = strrchr(path, '/');
    if (fwd != NULL && (last == NULL || fwd > last)) {
        last = fwd;
    }
#endif
    if (last == NULL) {
        return 0;
    }
    *last = '\0';
    return 1;
}

#ifndef _WIN32
/* Finds argv0 on PATH when it was invoked without a directory part. */
static int search_path(const char *name, char *out, size_t size)
{
    const char *path = getenv("PATH");
    const char *p;
    size_t len;

    if (path == NULL) {
        return 0;
    }
    p = path;
    while (*p != '\0') {
        const char *end = strchr(p, ':');
        len = (end != NULL) ? (size_t)(end - p) : strlen(p);
        if (len > 0 && len + strlen(name) + 2 < size) {
            memcpy(out, p, len);
            out[len] = '/';
            strcpy(out + len + 1, name);
            if (access(out, X_OK) == 0) {
                return 1;
            }
        }
        if (end == NULL) {
            break;
        }
        p = end + 1;
    }
    return 0;
}
#endif

/* Writes the directory containing this executable into out. */
static int find_self_dir(const char *argv0, char *out, size_t size)
{
#ifdef _WIN32
    char buf[MAX_PATH];
    (void)argv0; /* Windows asks the OS for the executable path */
    DWORD len = GetModuleFileNameA(NULL, buf, sizeof buf);
    if (len == 0 || len >= sizeof buf) {
        return 0;
    }
    if (!strip_last(buf)) {
        return 0;
    }
    if (strlen(buf) + 1 > size) {
        return 0;
    }
    strcpy(out, buf);
    return 1;
#else
    char resolved[PATH_MAX];
    char candidate[PATH_MAX];

    if (strchr(argv0, '/') != NULL) {
        if (realpath(argv0, resolved) == NULL) {
            return 0;
        }
    } else {
        if (!search_path(argv0, candidate, sizeof candidate)) {
            return 0;
        }
        if (realpath(candidate, resolved) == NULL) {
            return 0;
        }
    }
    if (!strip_last(resolved)) {
        return 0;
    }
    if (strlen(resolved) + 1 > size) {
        return 0;
    }
    strcpy(out, resolved);
    return 1;
#endif
}

int main(int argc, char **argv)
{
    char dir[PATH_MAX];
    char script[PATH_MAX + 32];
    const char *shell;
    char **args;
    int i;

    if (argc > 1 && (strcmp(argv[1], "-V") == 0 || strcmp(argv[1], "--version") == 0)) {
        printf("nullroute launcher %s\n", VERSION);
        return 0;
    }

    if (!find_self_dir(argv[0], dir, sizeof dir)) {
        fprintf(stderr, "nullroute: cannot locate the game directory\n");
        return 1;
    }

    /* Packaged layout first (game/ beside the launcher). Inside a source
     * checkout the launcher usually sits in build/, so also try ../game. */
    if (snprintf(script, sizeof script, "%s%cgame%cbin%cnullroute",
                 dir, DIR_SEP, DIR_SEP, DIR_SEP) >= (int)sizeof script) {
        fprintf(stderr, "nullroute: path too long\n");
        return 1;
    }
    if (!file_exists(script)) {
        if (snprintf(script, sizeof script, "%s%c..%cgame%cbin%cnullroute",
                     dir, DIR_SEP, DIR_SEP, DIR_SEP, DIR_SEP) >= (int)sizeof script) {
            fprintf(stderr, "nullroute: path too long\n");
            return 1;
        }
        if (!file_exists(script)) {
            fprintf(stderr, "nullroute: cannot find game/bin/nullroute next to the launcher\n");
            return 1;
        }
    }

    shell = getenv("NULLROUTE_SH");
    if (shell == NULL || *shell == '\0') {
        shell = DEFAULT_SHELL;
    }

    /* shell, script, the user's arguments, NULL */
    args = malloc(sizeof *args * (size_t)(argc + 2));
    if (args == NULL) {
        fprintf(stderr, "nullroute: out of memory\n");
        return 1;
    }
    args[0] = (char *)shell;
    args[1] = script;
    for (i = 1; i < argc; i++) {
        args[i + 1] = argv[i];
    }
    args[argc + 1] = NULL;

#ifdef _WIN32
    {
        intptr_t rc = _spawnvp(_P_WAIT, shell, (const char *const *)args);
        free(args);
        if (rc == -1) {
            fprintf(stderr, "nullroute: cannot start '%s'. Install Git for Windows or MSYS2,\n"
                            "or set NULLROUTE_SH to the full path of a POSIX sh.\n", shell);
            return 1;
        }
        return (int)rc;
    }
#else
    execv(shell, args);
    fprintf(stderr, "nullroute: cannot run '%s'\n", shell);
    free(args);
    return 127;
#endif
}
