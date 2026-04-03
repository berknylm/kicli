/*
 * sym_lib.c — find and cache symbols from KiCad's .kicad_sym libraries
 *
 * Usage:
 *   sexpr_t *sym = kicli_sym_lib_find("Device:R");
 *   // returns pointer into cached tree — caller must NOT free
 */

#include "kicli/sch.h"
#include "kicli/error.h"
#include "kicli/kicad_cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  define PATH_SEP '\\'
#else
#  include <sys/stat.h>
#  define PATH_SEP '/'
#endif

/* ── Symbol library directory ─────────────────────────────────────────────── */

static char s_sym_dir[1024] = {0};

/*
 * Derive the symbols directory from the kicad-cli path.
 *
 * macOS: .../KiCad.app/Contents/MacOS/kicad-cli
 *     -> .../KiCad.app/Contents/SharedSupport/symbols/
 * Linux: /usr/bin/kicad-cli
 *     -> /usr/share/kicad/symbols/
 * Windows: C:\Program Files\KiCad\bin\kicad-cli.exe
 *       -> C:\Program Files\KiCad\share\kicad\symbols\
 */
static void derive_sym_dir(const char *cli_path)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", cli_path);

    /* strip filename */
    char *last = strrchr(buf, PATH_SEP);
    if (last) *last = '\0';

#if defined(__APPLE__)
    /* buf is now .../Contents/MacOS */
    last = strrchr(buf, PATH_SEP);
    if (last) *last = '\0'; /* .../Contents */
    snprintf(s_sym_dir, sizeof(s_sym_dir), "%s/SharedSupport/symbols", buf);

#elif defined(_WIN32)
    /* buf is .../KiCad/bin */
    last = strrchr(buf, PATH_SEP);
    if (last) *last = '\0'; /* .../KiCad */
    snprintf(s_sym_dir, sizeof(s_sym_dir),
             "%s\\share\\kicad\\symbols", buf);

#else
    /* Linux: /usr/bin -> /usr/share/kicad/symbols */
    last = strrchr(buf, PATH_SEP);
    if (last) *last = '\0'; /* /usr */
    snprintf(s_sym_dir, sizeof(s_sym_dir),
             "%s/share/kicad/symbols", buf);
#endif
}

static const char *get_sym_dir(void)
{
    if (s_sym_dir[0]) return s_sym_dir;

    char cli[KICAD_CLI_MAX_PATH];
    if (kicad_cli_find(cli) != KICLI_OK) {
        /* fallback platform defaults */
#if defined(__APPLE__)
        snprintf(s_sym_dir, sizeof(s_sym_dir),
                 "/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols");
#elif defined(_WIN32)
        snprintf(s_sym_dir, sizeof(s_sym_dir),
                 "C:\\Program Files\\KiCad\\share\\kicad\\symbols");
#else
        snprintf(s_sym_dir, sizeof(s_sym_dir),
                 "/usr/share/kicad/symbols");
#endif
        return s_sym_dir;
    }

    derive_sym_dir(cli);
    return s_sym_dir;
}

/* ── Library cache entry ──────────────────────────────────────────────────── */

typedef struct {
    char     name[64];   /* e.g. "Device" */
    sexpr_t *tree;       /* parsed .kicad_sym root */
} lib_cache_t;

#define MAX_LIB_CACHE 32
static lib_cache_t s_cache[MAX_LIB_CACHE];
static size_t      s_cache_count = 0;

static sexpr_t *load_lib(const char *lib_name)
{
    /* check cache */
    for (size_t i = 0; i < s_cache_count; i++) {
        if (strcmp(s_cache[i].name, lib_name) == 0)
            return s_cache[i].tree;
    }

    /* build path */
    char path[1536];
    snprintf(path, sizeof(path), "%s/%s.kicad_sym", get_sym_dir(), lib_name);

    /* read file */
    FILE *f = fopen(path, "rb");
    if (!f) {
        kicli_set_error("library not found: %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); kicli_set_error("out of memory"); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';

    char errbuf[256];
    sexpr_t *tree = sexpr_parse(buf, errbuf, sizeof(errbuf));
    free(buf);

    if (!tree) {
        kicli_set_error("parse error in %s: %s", lib_name, errbuf);
        return NULL;
    }

    /* cache it */
    if (s_cache_count < MAX_LIB_CACHE) {
        snprintf(s_cache[s_cache_count].name, 64, "%s", lib_name);
        s_cache[s_cache_count].tree = tree;
        s_cache_count++;
    }

    return tree;
}

/* ── Public: find a symbol ────────────────────────────────────────────────── */

/*
 * kicli_sym_lib_find("Device:R")
 * Returns pointer to the (symbol "R" ...) node inside the library tree.
 * Caller must NOT free — it's owned by the cache.
 * Returns NULL and sets error on failure.
 */
sexpr_t *kicli_sym_lib_find(const char *lib_id)
{
    if (!lib_id) return NULL;

    /* split "Device:R" into lib="Device", sym="R" */
    char lib_name[256];
    const char *colon = strchr(lib_id, ':');
    if (!colon) {
        kicli_set_error("invalid lib_id '%s' (expected LIB:SYM)", lib_id);
        return NULL;
    }
    size_t lib_len = (size_t)(colon - lib_id);
    if (lib_len >= sizeof(lib_name)) {
        kicli_set_error("lib name too long");
        return NULL;
    }
    memcpy(lib_name, lib_id, lib_len);
    lib_name[lib_len] = '\0';
    const char *sym_name = colon + 1;

    sexpr_t *lib_tree = load_lib(lib_name);
    if (!lib_tree) return NULL;

    /* search for (symbol "sym_name" ...) in lib root */
    for (size_t i = 0; i < lib_tree->num_children; i++) {
        sexpr_t *c = lib_tree->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
        sexpr_t *tag = c->children[0];
        if (!tag || !tag->value || strcmp(tag->value, "symbol") != 0) continue;
        sexpr_t *name = c->children[1];
        if (!name || !name->value) continue;
        if (strcmp(name->value, sym_name) == 0) return c;
    }

    kicli_set_error("symbol '%s' not found in library '%s'", sym_name, lib_name);
    return NULL;
}

/*
 * Collect pin numbers from a library symbol definition.
 * Searches recursively through sub-symbols.
 * nums[] must be freed by caller (each entry is strdup'd).
 * Returns count.
 */
static void collect_pins_rec(const sexpr_t *node, char **nums,
                              size_t *count, size_t cap)
{
    if (!node || node->type != SEXPR_LIST || node->num_children == 0) return;
    const char *tag = node->children[0]->value;
    if (!tag) return;

    if (strcmp(tag, "pin") == 0) {
        /* (pin type dir (at ...) (length ...) (name ...) (number "N" ...)) */
        for (size_t i = 1; i < node->num_children; i++) {
            sexpr_t *c = node->children[i];
            if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
            sexpr_t *ptag = c->children[0];
            if (!ptag || !ptag->value || strcmp(ptag->value, "number") != 0) continue;
            if (*count < cap && c->children[1]->value)
                nums[(*count)++] = strdup(c->children[1]->value);
        }
        return;
    }

    /* recurse into sub-symbols and other list children */
    for (size_t i = 1; i < node->num_children; i++)
        collect_pins_rec(node->children[i], nums, count, cap);
}

size_t kicli_sym_lib_get_pins(const sexpr_t *sym_def,
                               char **nums_out, size_t cap)
{
    size_t count = 0;
    collect_pins_rec(sym_def, nums_out, &count, cap);
    return count;
}
