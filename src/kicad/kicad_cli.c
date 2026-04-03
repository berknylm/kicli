#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  define popen  _popen
#  define pclose _pclose
#  define F_OK 0
#  define access _access
#else
#  include <unistd.h>
#  include <sys/wait.h>
#endif

#include "kicli/kicad_cli.h"
#include "kicli/error.h"

/* ── Platform default paths ─────────────────────────────────────────────── */

static const char *default_paths[] = {
#if defined(__APPLE__)
    "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli",
    /* Homebrew cask installs to same location */
#elif defined(_WIN32)
    "C:\\Program Files\\KiCad\\bin\\kicad-cli.exe",
    "C:\\Program Files (x86)\\KiCad\\bin\\kicad-cli.exe",
    /* Scoop installs */
    /* Users can override with KICAD_CLI_PATH */
#else
    /* Linux */
    "/usr/bin/kicad-cli",
    "/usr/local/bin/kicad-cli",
    "/snap/kicad/current/usr/bin/kicad-cli",
    "/opt/kicad/bin/kicad-cli",
#endif
    NULL
};

/* ── PATH search ─────────────────────────────────────────────────────────── */

static bool find_in_path(char *out) {
    const char *path_env = getenv("PATH");
    if (!path_env) return false;

    /* Work on a copy */
    char path_copy[4096];
    strncpy(path_copy, path_env, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

#ifdef _WIN32
    const char sep = ';';
    const char *exe = "kicad-cli.exe";
#else
    const char sep = ':';
    const char *exe = "kicad-cli";
#endif

    char *dir = path_copy;
    char *next;
    while (dir && *dir) {
        next = strchr(dir, sep);
        if (next) *next = '\0';

        char candidate[KICAD_CLI_MAX_PATH];
        snprintf(candidate, sizeof(candidate), "%s/%s", dir, exe);

        if (access(candidate, F_OK) == 0) {
            strncpy(out, candidate, KICAD_CLI_MAX_PATH - 1);
            out[KICAD_CLI_MAX_PATH - 1] = '\0';
            return true;
        }

        dir = next ? next + 1 : NULL;
    }
    return false;
}

/* ── Cached path ─────────────────────────────────────────────────────────── */

static char _cached_path[KICAD_CLI_MAX_PATH] = {0};

kicli_err_t kicad_cli_find(char *out) {
    /* Return cached result */
    if (_cached_path[0] != '\0') {
        strncpy(out, _cached_path, KICAD_CLI_MAX_PATH - 1);
        return KICLI_OK;
    }

    /* 1. Environment variable override */
    const char *env = getenv("KICAD_CLI_PATH");
    if (env && access(env, F_OK) == 0) {
        strncpy(_cached_path, env, KICAD_CLI_MAX_PATH - 1);
        strncpy(out, _cached_path, KICAD_CLI_MAX_PATH - 1);
        return KICLI_OK;
    }

    /* 2. Platform default paths */
    for (int i = 0; default_paths[i] != NULL; i++) {
        if (access(default_paths[i], F_OK) == 0) {
            strncpy(_cached_path, default_paths[i], KICAD_CLI_MAX_PATH - 1);
            strncpy(out, _cached_path, KICAD_CLI_MAX_PATH - 1);
            return KICLI_OK;
        }
    }

    /* 3. Search PATH */
    if (find_in_path(_cached_path)) {
        strncpy(out, _cached_path, KICAD_CLI_MAX_PATH - 1);
        return KICLI_OK;
    }

    kicli_set_error(
        "kicad-cli not found. Install KiCad 10 or set KICAD_CLI_PATH.\n"
        "  macOS:   /Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli\n"
        "  Windows: C:\\Program Files\\KiCad\\bin\\kicad-cli.exe\n"
        "  Linux:   sudo apt install kicad  (or equivalent)"
    );
    return KICLI_ERR_NOT_FOUND;
}

/* ── Build command string ────────────────────────────────────────────────── */

/*
 * Build a single shell command string from kicad-cli path + args array.
 * Returns heap-allocated string; caller frees.
 */
static char *build_command(const char *cli_path, const char *const *args) {
    /* Calculate total length */
    size_t len = strlen(cli_path) + 4; /* quotes + space */
    for (int i = 0; args[i] != NULL; i++) {
        len += strlen(args[i]) + 3; /* space + quotes */
    }

    char *cmd = malloc(len);
    if (!cmd) return NULL;

    /* Quote the executable path (handles spaces in "Program Files") */
#ifdef _WIN32
    snprintf(cmd, len, "\"%s\"", cli_path);
#else
    snprintf(cmd, len, "\"%s\"", cli_path);
#endif

    for (int i = 0; args[i] != NULL; i++) {
        strncat(cmd, " \"", len - strlen(cmd) - 1);
        strncat(cmd, args[i], len - strlen(cmd) - 1);
        strncat(cmd, "\"", len - strlen(cmd) - 1);
    }

    return cmd;
}

/* ── kicad_cli_run ───────────────────────────────────────────────────────── */

kicli_err_t kicad_cli_run(const char *const *args) {
    char cli_path[KICAD_CLI_MAX_PATH];
    kicli_err_t err = kicad_cli_find(cli_path);
    if (err != KICLI_OK) return err;

    char *cmd = build_command(cli_path, args);
    if (!cmd) {
        kicli_set_error("out of memory");
        return KICLI_ERR_OOM;
    }

    int ret = system(cmd);
    free(cmd);

    if (ret != 0) {
        kicli_set_error("kicad-cli exited with code %d", ret);
        return KICLI_ERR_SUBPROCESS;
    }
    return KICLI_OK;
}

/* ── kicad_cli_capture ───────────────────────────────────────────────────── */

kicli_err_t kicad_cli_capture(const char *const *args, char **output) {
    char cli_path[KICAD_CLI_MAX_PATH];
    kicli_err_t err = kicad_cli_find(cli_path);
    if (err != KICLI_OK) return err;

    char *cmd = build_command(cli_path, args);
    if (!cmd) {
        kicli_set_error("out of memory");
        return KICLI_ERR_OOM;
    }

    /* Redirect stderr to /dev/null so we only capture stdout */
#ifdef _WIN32
    char cmd2[4096];
    snprintf(cmd2, sizeof(cmd2), "%s 2>nul", cmd);
#else
    char cmd2[4096];
    snprintf(cmd2, sizeof(cmd2), "%s 2>/dev/null", cmd);
#endif
    free(cmd);

    FILE *fp = popen(cmd2, "r");
    if (!fp) {
        kicli_set_error("failed to run kicad-cli: %s", cmd2);
        return KICLI_ERR_SUBPROCESS;
    }

    /* Read all output */
    size_t size = 0, cap = 4096;
    char *buf = malloc(cap);
    if (!buf) { pclose(fp); return KICLI_ERR_OOM; }

    size_t n;
    char tmp[1024];
    while ((n = fread(tmp, 1, sizeof(tmp), fp)) > 0) {
        if (size + n + 1 > cap) {
            cap = (size + n + 1) * 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); pclose(fp); return KICLI_ERR_OOM; }
            buf = nb;
        }
        memcpy(buf + size, tmp, n);
        size += n;
    }
    buf[size] = '\0';

    int ret = pclose(fp);
    if (ret != 0) {
        free(buf);
        kicli_set_error("kicad-cli exited with code %d", ret);
        return KICLI_ERR_SUBPROCESS;
    }

    *output = buf;
    return KICLI_OK;
}

/* ── kicad_cli_version ───────────────────────────────────────────────────── */

kicli_err_t kicad_cli_version(char *out) {
    static char _cached_version[32] = {0};

    if (_cached_version[0] != '\0') {
        strncpy(out, _cached_version, 31);
        return KICLI_OK;
    }

    const char *args[] = {"version", "--format", "plain", NULL};
    char *output = NULL;
    kicli_err_t err = kicad_cli_capture(args, &output);
    if (err != KICLI_OK) return err;

    /* Strip trailing newline */
    size_t len = strlen(output);
    while (len > 0 && (output[len-1] == '\n' || output[len-1] == '\r'))
        output[--len] = '\0';

    strncpy(_cached_version, output, sizeof(_cached_version) - 1);
    strncpy(out, _cached_version, 31);
    free(output);
    return KICLI_OK;
}
