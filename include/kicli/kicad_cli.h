#pragma once
#ifndef KICLI_KICAD_CLI_H
#define KICLI_KICAD_CLI_H

/*
 * kicad_cli.h — cross-platform kicad-cli discovery and execution
 *
 * Priority order for finding kicad-cli:
 *   1. $KICAD_CLI_PATH environment variable (user override)
 *   2. Platform default install paths
 *   3. $PATH lookup
 */

#include <stdbool.h>
#include "kicli/error.h"

/* Maximum path length for kicad-cli executable */
#define KICAD_CLI_MAX_PATH 512

/*
 * Find kicad-cli and store its full path in `out` (size >= KICAD_CLI_MAX_PATH).
 * Returns KICLI_OK if found, KICLI_ERR_NOT_FOUND if not found anywhere.
 */
kicli_err_t kicad_cli_find(char *out);

/*
 * Run kicad-cli with the given arguments.
 * `args` is a NULL-terminated array of strings (not including argv[0]).
 * Output is streamed directly to stdout/stderr.
 * Returns KICLI_OK on success, KICLI_ERR_SUBPROCESS on non-zero exit,
 * KICLI_ERR_NOT_FOUND if kicad-cli is not installed.
 */
kicli_err_t kicad_cli_run(const char *const *args);

/*
 * Same as kicad_cli_run but captures stdout into a heap-allocated string.
 * Caller must free *output.
 */
kicli_err_t kicad_cli_capture(const char *const *args, char **output);

/*
 * Return the KiCad version string (e.g. "10.0.0").
 * Writes into `out` (size >= 32). Cached after first call.
 */
kicli_err_t kicad_cli_version(char *out);

#endif /* KICLI_KICAD_CLI_H */
