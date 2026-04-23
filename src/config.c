/*
 * config.c — simple key=value config at ~/.config/kicli/config
 */

#include "kicli/config.h"
#include "kicli/portable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void kicli_config_path(char *out, size_t size)
{
#ifdef _WIN32
    const char *home = getenv("APPDATA");
    if (!home) home = getenv("USERPROFILE");
    snprintf(out, size, "%s\\kicli\\config", home ? home : ".");
#else
    const char *home = getenv("HOME");
    snprintf(out, size, "%s/.config/kicli/config", home ? home : ".");
#endif
}

void kicli_config_load(kicli_config_t *out)
{
    memset(out, 0, sizeof(*out));

    char path[KICLI_CONFIG_PATH_MAX];
    kicli_config_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[KICLI_CONFIG_PATH_MAX + 64];
    while (fgets(line, sizeof(line), f)) {
        /* strip trailing newline */
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r'))
            line[--n] = '\0';

        /* skip comments and blank lines */
        if (line[0] == '#' || line[0] == '\0') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        if (strcmp(key, "kicad_path") == 0)
            snprintf(out->kicad_path, sizeof(out->kicad_path), "%s", val);
    }
    fclose(f);
}

int kicli_config_save(const kicli_config_t *cfg)
{
    char path[KICLI_CONFIG_PATH_MAX];
    kicli_config_path(path, sizeof(path));

    /* ensure parent dir exists (and all missing ancestors) */
    char dir[KICLI_CONFIG_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *last = strrchr(dir, KICLI_PATH_SEP);
    if (last) {
        *last = '\0';
        kicli_mkdir_p(dir);
    }

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    if (cfg->kicad_path[0])
        fprintf(f, "kicad_path=%s\n", cfg->kicad_path);

    fclose(f);
    return 0;
}
