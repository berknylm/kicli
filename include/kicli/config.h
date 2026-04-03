#pragma once
#ifndef KICLI_CONFIG_H
#define KICLI_CONFIG_H

#include <stddef.h>

/*
 * kicli config — simple key=value file at ~/.config/kicli/config
 *
 * Supported keys:
 *   kicad_path = /path/to/kicad-cli
 */

#define KICLI_CONFIG_PATH_MAX 512

typedef struct {
    char kicad_path[KICLI_CONFIG_PATH_MAX]; /* empty = auto-discover */
} kicli_config_t;

/* Load config from ~/.config/kicli/config into *out. Missing file is fine. */
void kicli_config_load(kicli_config_t *out);

/* Save config back to ~/.config/kicli/config. Creates dirs as needed. */
int  kicli_config_save(const kicli_config_t *cfg);

/* Path of the config file (platform-specific). */
void kicli_config_path(char *out, size_t size);

#endif /* KICLI_CONFIG_H */
