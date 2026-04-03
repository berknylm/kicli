/*
 * kicad_cli.c — kicad-cli discovery, execution, capture
 *
 * Discovery priority:
 *   1. $KICAD_CLI_PATH env var
 *   2. Saved path in ~/.config/kicli/config
 *   3. Platform default install paths
 *   4. $PATH search
 *
 * After first successful auto-discovery the path is saved to config
 * so future runs skip the search entirely.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  define popen  _popen
#  define pclose _pclose
#  define F_OK   0
#  define access _access
#else
#  include <unistd.h>
#endif

#include "kicli/kicad_cli.h"
#include "kicli/config.h"
#include "kicli/error.h"

/* ── Platform default paths ─────────────────────────────────────────────── */

static const char *default_paths[] = {
#if defined(__APPLE__)
    "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli",
#elif defined(_WIN32)
    "C:\\Program Files\\KiCad\\10.0\\bin\\kicad-cli.exe",
    "C:\\Program Files\\KiCad\\bin\\kicad-cli.exe",
    "C:\\Program Files (x86)\\KiCad\\10.0\\bin\\kicad-cli.exe",
#else
    "/usr/bin/kicad-cli",
    "/usr/local/bin/kicad-cli",
    "/snap/kicad/current/usr/bin/kicad-cli",
#endif
    NULL
};

/* ── PATH search ─────────────────────────────────────────────────────────── */

static int find_in_path(char *out)
{
    const char *path_env = getenv("PATH");
    if (!path_env) return 0;

    char path_copy[4096];
    snprintf(path_copy, sizeof(path_copy), "%s", path_env);

#ifdef _WIN32
    const char sep = ';';
    const char *exe = "kicad-cli.exe";
#else
    const char sep = ':';
    const char *exe = "kicad-cli";
#endif

    char *dir = path_copy;
    while (dir && *dir) {
        char *next = strchr(dir, sep);
        if (next) *next = '\0';

        char candidate[KICAD_CLI_MAX_PATH];
        snprintf(candidate, sizeof(candidate), "%s/%s", dir, exe);
        if (access(candidate, F_OK) == 0) {
            snprintf(out, KICAD_CLI_MAX_PATH, "%s", candidate);
            return 1;
        }
        dir = next ? next + 1 : NULL;
    }
    return 0;
}

/* ── Cached path ─────────────────────────────────────────────────────────── */

static char s_path[KICAD_CLI_MAX_PATH] = {0};

kicli_err_t kicad_cli_find(char *out)
{
    if (s_path[0]) {
        snprintf(out, KICAD_CLI_MAX_PATH, "%s", s_path);
        return KICLI_OK;
    }

    /* 1. Env var override */
    const char *env = getenv("KICAD_CLI_PATH");
    if (env && access(env, F_OK) == 0) {
        snprintf(s_path, sizeof(s_path), "%s", env);
        snprintf(out, KICAD_CLI_MAX_PATH, "%s", s_path);
        return KICLI_OK;
    }

    /* 2. Saved config */
    kicli_config_t cfg;
    kicli_config_load(&cfg);
    if (cfg.kicad_path[0] && access(cfg.kicad_path, F_OK) == 0) {
        snprintf(s_path, sizeof(s_path), "%s", cfg.kicad_path);
        snprintf(out, KICAD_CLI_MAX_PATH, "%s", s_path);
        return KICLI_OK;
    }

    /* 3. Platform defaults */
    for (int i = 0; default_paths[i]; i++) {
        if (access(default_paths[i], F_OK) == 0) {
            snprintf(s_path, sizeof(s_path), "%s", default_paths[i]);
            snprintf(out, KICAD_CLI_MAX_PATH, "%s", s_path);
            /* save so next run is instant */
            cfg.kicad_path[0] = '\0';
            snprintf(cfg.kicad_path, sizeof(cfg.kicad_path), "%s", s_path);
            kicli_config_save(&cfg);
            return KICLI_OK;
        }
    }

    /* 4. PATH search */
    if (find_in_path(s_path)) {
        snprintf(out, KICAD_CLI_MAX_PATH, "%s", s_path);
        snprintf(cfg.kicad_path, sizeof(cfg.kicad_path), "%s", s_path);
        kicli_config_save(&cfg);
        return KICLI_OK;
    }

    kicli_set_error(
        "kicad-cli not found.\n"
        "Install KiCad 10, or set the path with:\n"
        "  kicli config kicad-path /path/to/kicad-cli"
    );
    return KICLI_ERR_NOT_FOUND;
}

kicli_err_t kicad_cli_set_path(const char *path)
{
    if (!path || !path[0]) {
        kicli_set_error("path cannot be empty");
        return KICLI_ERR_INVALID_ARG;
    }
    if (access(path, F_OK) != 0) {
        kicli_set_error("'%s' does not exist", path);
        return KICLI_ERR_NOT_FOUND;
    }

    snprintf(s_path, sizeof(s_path), "%s", path);

    kicli_config_t cfg;
    kicli_config_load(&cfg);
    snprintf(cfg.kicad_path, sizeof(cfg.kicad_path), "%s", path);
    if (kicli_config_save(&cfg) != 0) {
        kicli_set_error("failed to save config");
        return KICLI_ERR_IO;
    }
    return KICLI_OK;
}

/* ── Build command string ────────────────────────────────────────────────── */

static char *build_cmd(const char *cli, const char *const *args)
{
    size_t len = strlen(cli) + 4;
    for (int i = 0; args[i]; i++) len += strlen(args[i]) + 3;

    char *cmd = malloc(len);
    if (!cmd) return NULL;

    snprintf(cmd, len, "\"%s\"", cli);
    for (int i = 0; args[i]; i++) {
        strncat(cmd, " \"", len - strlen(cmd) - 1);
        strncat(cmd, args[i], len - strlen(cmd) - 1);
        strncat(cmd, "\"",    len - strlen(cmd) - 1);
    }
    return cmd;
}

/* ── kicad_cli_run ───────────────────────────────────────────────────────── */

kicli_err_t kicad_cli_run(const char *const *args)
{
    char cli[KICAD_CLI_MAX_PATH];
    kicli_err_t err = kicad_cli_find(cli);
    if (err != KICLI_OK) return err;

    char *cmd = build_cmd(cli, args);
    if (!cmd) { kicli_set_error("out of memory"); return KICLI_ERR_OOM; }

    int ret = system(cmd);
    free(cmd);

    if (ret != 0) {
        kicli_set_error("kicad-cli exited with code %d", ret);
        return KICLI_ERR_SUBPROCESS;
    }
    return KICLI_OK;
}

/* ── kicad_cli_capture ───────────────────────────────────────────────────── */

kicli_err_t kicad_cli_capture(const char *const *args, char **output)
{
    char cli[KICAD_CLI_MAX_PATH];
    kicli_err_t err = kicad_cli_find(cli);
    if (err != KICLI_OK) return err;

    char *cmd = build_cmd(cli, args);
    if (!cmd) { kicli_set_error("out of memory"); return KICLI_ERR_OOM; }

#ifdef _WIN32
    /* Windows: popen doesn't work well with quoted paths; use temp file */
    char tmp[MAX_PATH + 64];
    char dir[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    snprintf(tmp, sizeof(tmp), "%skicli_cap_%lu.txt",
             dir, (unsigned long)GetCurrentProcessId());

    char cmd2[4096];
    snprintf(cmd2, sizeof(cmd2), "\"%s >\"%s\" 2>nul\"", cmd, tmp);
    free(cmd);

    if (system(cmd2) != 0) {
        DeleteFileA(tmp);
        kicli_set_error("kicad-cli failed");
        return KICLI_ERR_SUBPROCESS;
    }

    FILE *tf = fopen(tmp, "rb");
    if (!tf) { DeleteFileA(tmp); kicli_set_error("cannot read output"); return KICLI_ERR_IO; }
    fseek(tf, 0, SEEK_END);
    long sz = ftell(tf); rewind(tf);
    char *buf = malloc((size_t)(sz < 0 ? 1 : sz + 1));
    if (!buf) { fclose(tf); DeleteFileA(tmp); return KICLI_ERR_OOM; }
    size_t n = (sz > 0) ? fread(buf, 1, (size_t)sz, tf) : 0;
    buf[n] = '\0';
    fclose(tf); DeleteFileA(tmp);
    *output = buf;
    return KICLI_OK;

#else
    char cmd2[4096];
    snprintf(cmd2, sizeof(cmd2), "%s 2>/dev/null", cmd);
    free(cmd);

    FILE *fp = popen(cmd2, "r");
    if (!fp) { kicli_set_error("failed to run kicad-cli"); return KICLI_ERR_SUBPROCESS; }

    size_t size = 0, cap = 4096;
    char *buf = malloc(cap);
    if (!buf) { pclose(fp); return KICLI_ERR_OOM; }

    char tmp2[1024];
    size_t nr;
    while ((nr = fread(tmp2, 1, sizeof(tmp2), fp)) > 0) {
        if (size + nr + 1 > cap) {
            cap = (size + nr + 1) * 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); pclose(fp); return KICLI_ERR_OOM; }
            buf = nb;
        }
        memcpy(buf + size, tmp2, nr);
        size += nr;
    }
    buf[size] = '\0';
    pclose(fp);
    *output = buf;
    return KICLI_OK;
#endif
}

/* ── kicad_cli_version ───────────────────────────────────────────────────── */

kicli_err_t kicad_cli_version(char *out)
{
    static char s_ver[32] = {0};
    if (s_ver[0]) { snprintf(out, 32, "%s", s_ver); return KICLI_OK; }

    const char *args[] = {"version", "--format", "plain", NULL};
    char *raw = NULL;
    kicli_err_t err = kicad_cli_capture(args, &raw);
    if (err != KICLI_OK) return err;

    /* strip trailing newline */
    size_t len = strlen(raw);
    while (len > 0 && (raw[len-1] == '\n' || raw[len-1] == '\r')) raw[--len] = '\0';

    snprintf(s_ver, sizeof(s_ver), "%s", raw);
    snprintf(out, 32, "%s", s_ver);
    free(raw);
    return KICLI_OK;
}
