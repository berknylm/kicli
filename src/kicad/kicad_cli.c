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

#include "kicli/portable.h"
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
        if (kicli_exists(candidate)) {
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
    if (env && kicli_exists(env)) {
        snprintf(s_path, sizeof(s_path), "%s", env);
        snprintf(out, KICAD_CLI_MAX_PATH, "%s", s_path);
        return KICLI_OK;
    }

    /* 2. Saved config */
    kicli_config_t cfg;
    kicli_config_load(&cfg);
    if (cfg.kicad_path[0] && kicli_exists(cfg.kicad_path)) {
        snprintf(s_path, sizeof(s_path), "%s", cfg.kicad_path);
        snprintf(out, KICAD_CLI_MAX_PATH, "%s", s_path);
        return KICLI_OK;
    }

    /* 3. Platform defaults */
    for (int i = 0; default_paths[i]; i++) {
        if (kicli_exists(default_paths[i])) {
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
    if (!kicli_exists(path)) {
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

/* Reject characters that would break our naive double-quoted shell quoting.
 * kicli builds a system() command string: "cli" "arg1" "arg2" — a double-quote
 * or backtick or $ inside a path lets the shell re-interpret it. Instead of
 * writing a full POSIX shell escaper, we refuse paths with these glyphs —
 * they're essentially never present in real KiCad paths, and better to fail
 * loudly than run unsafe. */
static int arg_is_shell_safe(const char *s)
{
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '`' || c == '$' || c == '\\' || c == '\n' || c == '\r')
            return 0;
    }
    return 1;
}

static char *build_cmd(const char *cli, const char *const *args)
{
    if (!arg_is_shell_safe(cli)) {
        kicli_set_error("kicad-cli path contains unsafe shell characters: %s", cli);
        return NULL;
    }
    for (int i = 0; args[i]; i++) {
        if (!arg_is_shell_safe(args[i])) {
            kicli_set_error("argument contains unsafe shell characters: %s", args[i]);
            return NULL;
        }
    }

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

    /* Capture stderr so we can filter out Fontconfig noise without losing
     * legitimate error messages. */
    char err_tmp[KICLI_PATH_MAX];
    kicli_temp_path(err_tmp, sizeof(err_tmp), "err", "txt");

    /* Size the full command string dynamically to avoid truncating on long
     * paths (our old 8192-byte cmd2 could silently drop the redirect suffix,
     * which would leak Fontconfig noise AND on Windows break quoting). */
    size_t cmd2_len = strlen(cmd) + strlen(err_tmp) + 16;
    char *cmd2 = malloc(cmd2_len);
    if (!cmd2) { free(cmd); kicli_set_error("out of memory"); return KICLI_ERR_OOM; }
    int w = snprintf(cmd2, cmd2_len, "%s 2> \"%s\"", cmd, err_tmp);
    free(cmd);
    if (w < 0 || (size_t)w >= cmd2_len) {
        free(cmd2);
        kicli_set_error("command string too long");
        return KICLI_ERR_SUBPROCESS;
    }

    int ret = system(cmd2);
    free(cmd2);

    FILE *ef = fopen(err_tmp, "r");
    if (ef) {
        char line[4096];
        while (fgets(line, sizeof(line), ef)) {
            if (strstr(line, "Fontconfig")) continue;
            fputs(line, stderr);
        }
        fclose(ef);
    }
    kicli_unlink(err_tmp);

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
    char tmp[KICLI_PATH_MAX];
    kicli_temp_path(tmp, sizeof(tmp), "cap", "txt");

    size_t cmd2_len = strlen(cmd) + strlen(tmp) + 32;
    char *cmd2 = malloc(cmd2_len);
    if (!cmd2) { free(cmd); kicli_set_error("out of memory"); return KICLI_ERR_OOM; }
    int w = snprintf(cmd2, cmd2_len, "\"%s >\"%s\" 2>nul\"", cmd, tmp);
    free(cmd);
    if (w < 0 || (size_t)w >= cmd2_len) {
        free(cmd2); kicli_set_error("command string too long"); return KICLI_ERR_SUBPROCESS;
    }
    int rc = system(cmd2);
    free(cmd2);
    if (rc != 0) {
        kicli_unlink(tmp);
        kicli_set_error("kicad-cli failed");
        return KICLI_ERR_SUBPROCESS;
    }

    /* Use the shared atomic-safe reader so we get proper ftell handling. */
    char *buf = kicli_read_file(tmp, NULL);
    kicli_unlink(tmp);
    if (!buf) { kicli_set_error("cannot read kicad-cli output"); return KICLI_ERR_IO; }
    *output = buf;
    return KICLI_OK;

#else
    size_t cmd2_len = strlen(cmd) + 24;
    char *cmd2 = malloc(cmd2_len);
    if (!cmd2) { free(cmd); kicli_set_error("out of memory"); return KICLI_ERR_OOM; }
    int w = snprintf(cmd2, cmd2_len, "%s 2>/dev/null", cmd);
    free(cmd);
    if (w < 0 || (size_t)w >= cmd2_len) {
        free(cmd2); kicli_set_error("command string too long"); return KICLI_ERR_SUBPROCESS;
    }

    FILE *fp = popen(cmd2, "r");
    free(cmd2);
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
