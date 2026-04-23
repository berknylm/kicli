/*
 * portable.h — cross-platform shims used across kicli.
 *
 * Every platform #ifdef for filesystem, process, and string APIs lives in
 * this module. Call sites stay portable C.
 */

#ifndef KICLI_PORTABLE_H
#define KICLI_PORTABLE_H

#include <stddef.h>
#include <stdio.h>

#ifdef _WIN32
#  define KICLI_PATH_SEP     '\\'
#  define KICLI_PATH_SEP_STR "\\"
#else
#  define KICLI_PATH_SEP     '/'
#  define KICLI_PATH_SEP_STR "/"
#endif

#define KICLI_PATH_MAX 1024

/* ── Filesystem ─────────────────────────────────────────────────────────── */

/* Create directory (mode 0755 on POSIX). Treats EEXIST as success.
 * Returns 0 on success, -1 on failure. errno is set on failure. */
int  kicli_mkdir(const char *path);

/* Create directory and all missing parents. Returns 0 on success. */
int  kicli_mkdir_p(const char *path);

/* 1 if path exists (file or dir), 0 otherwise. */
int  kicli_exists(const char *path);

/* 1 if path is a directory, 0 otherwise. */
int  kicli_is_dir(const char *path);

/* Delete a file. Returns 0 on success. */
int  kicli_unlink(const char *path);

/* Recursively delete a directory tree (uses platform shell). */
void kicli_rmrf(const char *path);

/* ── Temp paths ─────────────────────────────────────────────────────────── */

/* Write the system temp dir with trailing separator to out. */
void kicli_tempdir(char *out, size_t sz);

/* Build a unique temp path: <tempdir>kicli_<label>_<pid>.<ext>
 * Pass ext without leading dot ("net") or with it (".net") — both work.
 * Pass NULL or "" for ext to omit the extension. */
void kicli_temp_path(char *out, size_t sz, const char *label, const char *ext);

/* ── Strings ────────────────────────────────────────────────────────────── */

int  kicli_strcasecmp(const char *a, const char *b);

/* ── Directory iteration ────────────────────────────────────────────────── */

typedef struct kicli_dir kicli_dir_t;

/* Open a directory for iteration. Returns NULL if the dir cannot be opened. */
kicli_dir_t *kicli_opendir(const char *path);

/* Return the next entry name, or NULL at end. Skips "." and "..".
 * Pointer is valid until the next kicli_readdir / kicli_closedir call. */
const char  *kicli_readdir(kicli_dir_t *d);

void         kicli_closedir(kicli_dir_t *d);

#endif /* KICLI_PORTABLE_H */
