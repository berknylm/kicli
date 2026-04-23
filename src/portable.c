/*
 * portable.c — implementation of kicli/portable.h.
 *
 * All platform #ifdef blocks for filesystem, process and string APIs are
 * centralized here. Other translation units should call the kicli_* wrappers.
 */

#include "kicli/portable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  include <io.h>
#  ifndef S_ISDIR
#    define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#  endif
#else
#  include <unistd.h>
#  include <dirent.h>
#  include <sys/types.h>
#endif

/* ── Filesystem ─────────────────────────────────────────────────────────── */

int kicli_mkdir(const char *path)
{
#ifdef _WIN32
    int rc = _mkdir(path);
#else
    int rc = mkdir(path, 0755);
#endif
    if (rc == 0 || errno == EEXIST) return 0;
    return -1;
}

int kicli_mkdir_p(const char *path)
{
    if (!path || !*path) return -1;

    char buf[KICLI_PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", path);
    size_t n = strlen(buf);

    for (size_t i = 1; i < n; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char saved = buf[i];
            buf[i] = '\0';
            /* skip drive-letter or empty segments */
            if (buf[0] && !(i == 2 && buf[1] == ':')) {
                if (kicli_mkdir(buf) != 0) return -1;
            }
            buf[i] = saved;
        }
    }
    return kicli_mkdir(buf);
}

int kicli_exists(const char *path)
{
#ifdef _WIN32
    return _access(path, 0) == 0;
#else
    return access(path, F_OK) == 0;
#endif
}

int kicli_is_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

int kicli_unlink(const char *path)
{
#ifdef _WIN32
    return _unlink(path);
#else
    return unlink(path);
#endif
}

void kicli_rmrf(const char *path)
{
    if (!path || !*path) return;

    char cmd[KICLI_PATH_MAX + 64];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\" >nul 2>&1", path);
#else
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\" 2>/dev/null", path);
#endif
    (void)system(cmd);
}

int kicli_rename(const char *src, const char *dst)
{
    if (!src || !dst) { errno = EINVAL; return -1; }
#ifdef _WIN32
    /* POSIX rename(2) is not atomic on Windows if dst exists. */
    if (!MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING)) {
        errno = EIO;
        return -1;
    }
    return 0;
#else
    return rename(src, dst);
#endif
}

int kicli_write_file_atomic(const char *path, const void *content, size_t len)
{
    if (!path || !*path) { errno = EINVAL; return -1; }

    /* Write to <path>.kicli.tmp, fsync if we can, then atomically rename.
     * We include the pid so concurrent writers don't step on each other. */
    char tmp[KICLI_PATH_MAX];
#ifdef _WIN32
    unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    int n = snprintf(tmp, sizeof(tmp), "%s.kicli.%lu.tmp", path, pid);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;

    if (len > 0 && content) {
        size_t w = fwrite(content, 1, len, f);
        if (w != len) {
            int saved = errno;
            fclose(f);
            kicli_unlink(tmp);
            errno = saved ? saved : EIO;
            return -1;
        }
    }

    if (fflush(f) != 0) {
        int saved = errno;
        fclose(f);
        kicli_unlink(tmp);
        errno = saved;
        return -1;
    }

#ifndef _WIN32
    /* Best-effort durability on POSIX. If fsync is unsupported (e.g. some
     * network filesystems) we don't treat it as a hard failure. */
    int fd = fileno(f);
    if (fd >= 0) (void)fsync(fd);
#endif

    if (fclose(f) != 0) {
        int saved = errno;
        kicli_unlink(tmp);
        errno = saved;
        return -1;
    }

    if (kicli_rename(tmp, path) != 0) {
        int saved = errno;
        kicli_unlink(tmp);
        errno = saved;
        return -1;
    }
    return 0;
}

char *kicli_read_file(const char *path, size_t *out_len)
{
    if (!path || !*path) { errno = EINVAL; return NULL; }

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        int saved = errno;
        fclose(f);
        errno = saved;
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        int saved = errno;
        fclose(f);
        errno = saved;
        return NULL;
    }
    rewind(f);

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        errno = ENOMEM;
        return NULL;
    }

    size_t got = (sz > 0) ? fread(buf, 1, (size_t)sz, f) : 0;
    int ferr = ferror(f);
    fclose(f);
    if (ferr) {
        free(buf);
        errno = EIO;
        return NULL;
    }

    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

/* ── Temp paths ─────────────────────────────────────────────────────────── */

void kicli_tempdir(char *out, size_t sz)
{
    if (!out || sz == 0) return;

#ifdef _WIN32
    DWORD n = GetTempPathA((DWORD)sz, out);
    if (n == 0 || n >= sz) snprintf(out, sz, ".%s", KICLI_PATH_SEP_STR);
#else
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    snprintf(out, sz, "%s/", t);
#endif
}

void kicli_temp_path(char *out, size_t sz,
                     const char *label, const char *ext)
{
    if (!out || sz == 0) return;

    char dir[KICLI_PATH_MAX];
    kicli_tempdir(dir, sizeof(dir));

#ifdef _WIN32
    unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif

    const char *dot = "";
    if (ext && *ext && *ext != '.') dot = ".";
    if (!ext) ext = "";

    snprintf(out, sz, "%skicli_%s_%lu%s%s",
             dir, label ? label : "tmp", pid, dot, ext);
}

/* ── Strings ────────────────────────────────────────────────────────────── */

int kicli_strcasecmp(const char *a, const char *b)
{
#ifdef _WIN32
    return _stricmp(a, b);
#else
    return strcasecmp(a, b);
#endif
}

/* ── Directory iteration ────────────────────────────────────────────────── */

struct kicli_dir {
#ifdef _WIN32
    HANDLE            h;
    WIN32_FIND_DATAA  fd;
    int               first;
    int               valid;
#else
    DIR              *d;
#endif
};

kicli_dir_t *kicli_opendir(const char *path)
{
    if (!path || !*path) return NULL;

    kicli_dir_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;

#ifdef _WIN32
    char pattern[KICLI_PATH_MAX];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    d->h = FindFirstFileA(pattern, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->first = 1;
    d->valid = 1;
#else
    d->d = opendir(path);
    if (!d->d) { free(d); return NULL; }
#endif
    return d;
}

const char *kicli_readdir(kicli_dir_t *d)
{
    if (!d) return NULL;

#ifdef _WIN32
    for (;;) {
        if (!d->valid) return NULL;
        if (d->first) {
            d->first = 0;
        } else if (!FindNextFileA(d->h, &d->fd)) {
            d->valid = 0;
            return NULL;
        }
        const char *n = d->fd.cFileName;
        if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0')))
            continue;
        return n;
    }
#else
    for (;;) {
        struct dirent *ent = readdir(d->d);
        if (!ent) return NULL;
        const char *n = ent->d_name;
        if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0')))
            continue;
        return n;
    }
#endif
}

void kicli_closedir(kicli_dir_t *d)
{
    if (!d) return;
#ifdef _WIN32
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
#else
    if (d->d) closedir(d->d);
#endif
    free(d);
}
