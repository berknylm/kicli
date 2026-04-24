/*
 * catalog.c — kicli sym / fp search/info/list
 *
 * Scans:
 *   - KiCad-bundled libraries under the SharedSupport/{symbols,footprints}
 *     dir (platform-derived, same pattern as sym_lib.c).
 *   - Project-local libs/symbols and libs/footprints if the caller gave a
 *     --project dir or we detect one by walking ancestors for .kicad_pro.
 *
 * Each command emits tab-separated rows — agent-friendly; no color, no
 * column-padding fluff.
 */

#include "kicli/catalog.h"
#include "kicli/sch.h"
#include "kicli/error.h"
#include "kicli/kicad_cli.h"
#include "kicli/portable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Platform library roots (bundled libs) ───────────────────────────────── */

static void derive_kicad_root(char *out, size_t outsz, const char *cli_path)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", cli_path);
    char *last = strrchr(buf, KICLI_PATH_SEP);
    if (last) *last = '\0';
#if defined(__APPLE__)
    last = strrchr(buf, KICLI_PATH_SEP);
    if (last) *last = '\0';
    snprintf(out, outsz, "%s/SharedSupport", buf);
#elif defined(_WIN32)
    last = strrchr(buf, KICLI_PATH_SEP);
    if (last) *last = '\0';
    snprintf(out, outsz, "%s\\share\\kicad", buf);
#else
    last = strrchr(buf, KICLI_PATH_SEP);
    if (last) *last = '\0';
    snprintf(out, outsz, "%s/share/kicad", buf);
#endif
}

/* Fills `sym_root` and `fp_root` with the bundled library root dirs. Falls
 * back to known platform defaults when kicad-cli isn't resolvable. */
static void resolve_bundled_roots(char *sym_root, size_t srsz,
                                   char *fp_root,  size_t frsz)
{
    char cli[KICAD_CLI_MAX_PATH];
    if (kicad_cli_find(cli) == KICLI_OK) {
        char base[1024];
        derive_kicad_root(base, sizeof(base), cli);
        snprintf(sym_root, srsz, "%s%csymbols",    base, KICLI_PATH_SEP);
        snprintf(fp_root,  frsz, "%s%cfootprints", base, KICLI_PATH_SEP);
        return;
    }
#if defined(__APPLE__)
    snprintf(sym_root, srsz, "/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols");
    snprintf(fp_root,  frsz, "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints");
#elif defined(_WIN32)
    snprintf(sym_root, srsz, "C:\\Program Files\\KiCad\\share\\kicad\\symbols");
    snprintf(fp_root,  frsz, "C:\\Program Files\\KiCad\\share\\kicad\\footprints");
#else
    snprintf(sym_root, srsz, "/usr/share/kicad/symbols");
    snprintf(fp_root,  frsz, "/usr/share/kicad/footprints");
#endif
}

/* Ascend from `start` looking for a *.kicad_pro. Writes the *directory* path
 * to out. Returns 0 if found, -1 otherwise. */
static int find_project_root(const char *start, char *out, size_t outsz)
{
    char dir[KICLI_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", start && start[0] ? start : ".");

    for (int depth = 0; depth < 5; depth++) {
        kicli_dir_t *d = kicli_opendir(dir);
        if (d) {
            const char *name;
            while ((name = kicli_readdir(d)) != NULL) {
                size_t nl = strlen(name);
                if (nl > 10 && strcmp(name + nl - 10, ".kicad_pro") == 0) {
                    kicli_closedir(d);
                    snprintf(out, outsz, "%s", dir);
                    return 0;
                }
            }
            kicli_closedir(d);
        }
        char *sep = strrchr(dir, KICLI_PATH_SEP);
        if (!sep) break;
        *sep = '\0';
    }
    return -1;
}

/* ── Case-insensitive substring match ───────────────────────────────────── */

static int istrstr_(const char *hay, const char *needle)
{
    if (!needle || !*needle) return 1;
    if (!hay) return 0;
    size_t nl = strlen(needle);
    for (const char *h = hay; *h; h++) {
        size_t i;
        for (i = 0; i < nl; i++) {
            if (!h[i]) return 0;
            if (tolower((unsigned char)h[i]) != tolower((unsigned char)needle[i])) break;
        }
        if (i == nl) return 1;
    }
    return 0;
}

/* ── Symbol catalog ──────────────────────────────────────────────────────── */

/* Pull "(extends \"Base\")" value from a (symbol ...) node; NULL if absent. */
static const char *sym_extends_base(const sexpr_t *sym)
{
    for (size_t i = 1; i < sym->num_children; i++) {
        sexpr_t *c = sym->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
        if (!c->children[0]->value) continue;
        if (strcmp(c->children[0]->value, "extends") != 0) continue;
        if (c->children[1]->value) return c->children[1]->value;
    }
    return NULL;
}

/* Find a sibling (symbol "name" ...) under the same library root. */
static const sexpr_t *find_sibling_sym(const sexpr_t *lib_root, const char *name)
{
    if (!lib_root || !name) return NULL;
    for (size_t i = 0; i < lib_root->num_children; i++) {
        const sexpr_t *c = lib_root->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
        if (!c->children[0]->value) continue;
        if (strcmp(c->children[0]->value, "symbol") != 0) continue;
        if (c->children[1]->value && strcmp(c->children[1]->value, name) == 0) return c;
    }
    return NULL;
}

/* Count pins in one library symbol definition. Falls back to the (extends ...)
 * base if the current symbol carries no pins of its own (common for alias
 * chips like LM358 → LM2904). */
static size_t count_pins_in_sym_ex(const sexpr_t *sym, const sexpr_t *lib_root)
{
    char *stash[1024];
    size_t n = kicli_sym_lib_get_pins(sym, stash, 1024);
    for (size_t i = 0; i < n; i++) free(stash[i]);
    if (n == 0 && lib_root) {
        const char *base = sym_extends_base(sym);
        if (base) {
            const sexpr_t *bs = find_sibling_sym(lib_root, base);
            if (bs) {
                n = kicli_sym_lib_get_pins(bs, stash, 1024);
                for (size_t i = 0; i < n; i++) free(stash[i]);
            }
        }
    }
    return n;
}

static size_t count_pins_in_sym(const sexpr_t *sym)
{
    return count_pins_in_sym_ex(sym, NULL);
}

/* Pull the (property "Description" "...") value from a symbol node. */
static const char *sym_description(const sexpr_t *sym)
{
    for (size_t i = 1; i < sym->num_children; i++) {
        sexpr_t *c = sym->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 3) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "property") != 0) continue;
        if (!c->children[1]->value || strcmp(c->children[1]->value, "ki_description") == 0
            || strcmp(c->children[1]->value, "Description") == 0)
            return c->children[2]->value ? c->children[2]->value : "";
    }
    return "";
}

/* Load a .kicad_sym file into an sexpr tree (heap-owned; caller frees). */
static sexpr_t *load_kicad_sym(const char *path)
{
    char *buf = kicli_read_file(path, NULL);
    if (!buf) return NULL;
    char err[256] = {0};
    sexpr_t *root = sexpr_parse(buf, err, sizeof(err));
    free(buf);
    return root;
}

typedef struct {
    const char *query;          /* substring (NULL = match all) */
    int         pins_filter;    /* -1 = no filter */
    const char *lib_filter;     /* NULL = no filter */
    int         max_results;    /* 0 = unlimited */
    int         matches_so_far;
    int         info_hit;       /* for info: set to 1 if found */
    const char *info_lib;       /* for info: lib name to match */
    const char *info_sym;       /* for info: symbol name to match */
    int         info_show_pins;
    int         list_mode;      /* list just emits one row per symbol */
    int         aborted;
} sym_search_ctx_t;

static int sym_match_and_emit_ex(sym_search_ctx_t *ctx, const char *lib_name,
                                  const sexpr_t *sym, const sexpr_t *lib_root)
{
    if (!sym || sym->num_children < 2) return 0;
    const char *sym_name = sym->children[1]->value;
    if (!sym_name) return 0;

    /* info mode: exact lib + sym match */
    if (ctx->info_lib) {
        if (strcmp(lib_name, ctx->info_lib) != 0) return 0;
        if (strcmp(sym_name, ctx->info_sym) != 0) return 0;

        size_t pins = count_pins_in_sym_ex(sym, lib_root);
        printf("lib:         %s\n", lib_name);
        printf("name:        %s\n", sym_name);
        printf("pin_count:   %zu\n", pins);
        const char *desc = sym_description(sym);
        if (desc[0]) printf("description: %s\n", desc);

        if (ctx->info_show_pins) {
            printf("\nPins (NUM\\tNAME\\tTYPE):\n");
            /* Walk sub-symbols for (pin type dir (at ...) (name ...) (number ...)) */
            for (size_t i = 1; i < sym->num_children; i++) {
                sexpr_t *c = sym->children[i];
                if (!c || c->type != SEXPR_LIST || c->num_children == 0) continue;
                if (!c->children[0]->value) continue;
                if (strcmp(c->children[0]->value, "symbol") == 0) {
                    for (size_t j = 1; j < c->num_children; j++) {
                        sexpr_t *p = c->children[j];
                        if (!p || p->type != SEXPR_LIST || p->num_children < 2) continue;
                        if (!p->children[0]->value
                            || strcmp(p->children[0]->value, "pin") != 0) continue;
                        const char *type = p->children[1]->value ? p->children[1]->value : "-";
                        const char *num = "", *nm = "";
                        for (size_t k = 2; k < p->num_children; k++) {
                            sexpr_t *x = p->children[k];
                            if (!x || x->type != SEXPR_LIST || x->num_children < 2) continue;
                            if (!x->children[0]->value) continue;
                            if (strcmp(x->children[0]->value, "number") == 0 && x->children[1]->value)
                                num = x->children[1]->value;
                            else if (strcmp(x->children[0]->value, "name") == 0 && x->children[1]->value)
                                nm = x->children[1]->value;
                        }
                        /* Normalize "~" placeholder to empty. */
                        if (strcmp(nm, "~") == 0) nm = "";
                        printf("  %-8s\t%-24s\t%s\n",
                               num, nm[0] ? nm : "-", type);
                    }
                }
            }
        }
        ctx->info_hit = 1;
        ctx->aborted = 1;
        return 1;
    }

    /* list / search modes */
    if (ctx->lib_filter && strcmp(lib_name, ctx->lib_filter) != 0) return 0;

    size_t pins = count_pins_in_sym_ex(sym, lib_root);
    if (ctx->pins_filter >= 0 && (size_t)ctx->pins_filter != pins) return 0;

    if (!ctx->list_mode && ctx->query) {
        char full[256];
        snprintf(full, sizeof(full), "%s:%s", lib_name, sym_name);
        if (!istrstr_(full, ctx->query)) return 0;
    }

    printf("%s:%s\t%zu\t%s\n", lib_name, sym_name, pins, sym_description(sym));

    ctx->matches_so_far++;
    if (ctx->max_results > 0 && ctx->matches_so_far >= ctx->max_results) {
        ctx->aborted = 1;
        return 1;
    }
    return 0;
}

/* Scan a directory of .kicad_sym files (e.g. bundled symbols/ or project
 * libs/symbols/). Stops early if ctx->aborted becomes 1. */
static void scan_sym_dir(const char *dir, sym_search_ctx_t *ctx)
{
    kicli_dir_t *d = kicli_opendir(dir);
    if (!d) return;

    const char *name;
    while ((name = kicli_readdir(d)) != NULL && !ctx->aborted) {
        size_t nl = strlen(name);
        if (nl <= 10 || strcmp(name + nl - 10, ".kicad_sym") != 0) continue;

        char lib_name[128];
        size_t stem = nl - 10;  /* drop ".kicad_sym" */
        if (stem >= sizeof(lib_name)) stem = sizeof(lib_name) - 1;
        memcpy(lib_name, name, stem);
        lib_name[stem] = '\0';

        char path[KICLI_PATH_MAX];
        snprintf(path, sizeof(path), "%s%c%s", dir, KICLI_PATH_SEP, name);

        sexpr_t *root = load_kicad_sym(path);
        if (!root) continue;

        for (size_t i = 0; i < root->num_children && !ctx->aborted; i++) {
            sexpr_t *c = root->children[i];
            if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
            if (!c->children[0]->value) continue;
            if (strcmp(c->children[0]->value, "symbol") != 0) continue;
            /* Pass root so extends-base lookups can find sibling definitions. */
            sym_match_and_emit_ex(ctx, lib_name, c, root);
        }

        sexpr_free(root);
    }
    kicli_closedir(d);
}

/* Orchestrate: bundled + (optional) project-local sym dirs. */
static void scan_sym_sources(const char *project_dir, sym_search_ctx_t *ctx)
{
    char sym_root[1024], fp_root[1024];
    resolve_bundled_roots(sym_root, sizeof(sym_root), fp_root, sizeof(fp_root));
    scan_sym_dir(sym_root, ctx);
    if (ctx->aborted) return;

    if (project_dir) {
        char proj_sym[KICLI_PATH_MAX];
        snprintf(proj_sym, sizeof(proj_sym), "%s%clibs%csymbols",
                 project_dir, KICLI_PATH_SEP, KICLI_PATH_SEP);
        scan_sym_dir(proj_sym, ctx);
    }
}

/* ── Footprint catalog ──────────────────────────────────────────────────── */

/* Fast scan of a .kicad_mod header to extract pad count + body/package info.
 * We deliberately do NOT parse the full sexpr — footprint files are large and
 * we only need `(pad ...)` count and a few `(descr)` / `(tags)` strings.
 * Returns pad count; copies description into `out_desc` and tags into
 * `out_tags`. */
static size_t scan_mod_header(const char *path, char *out_desc, size_t desc_sz,
                               char *out_tags, size_t tags_sz)
{
    if (out_desc && desc_sz) out_desc[0] = '\0';
    if (out_tags && tags_sz) out_tags[0] = '\0';

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    size_t pads = 0;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        /* Count distinct `(pad ...)` lines. KiCad's writer puts each pad
         * on its own line; this is a lightweight heuristic — accurate on
         * all bundled and SnapEDA/UL-generated files. */
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "(pad ", 5) == 0) pads++;
        else if (strncmp(p, "(descr ", 7) == 0 && out_desc && !out_desc[0]) {
            const char *start = strchr(p, '"');
            if (start) {
                start++;
                const char *end = strchr(start, '"');
                if (end) {
                    size_t len = (size_t)(end - start);
                    if (len >= desc_sz) len = desc_sz - 1;
                    memcpy(out_desc, start, len);
                    out_desc[len] = '\0';
                }
            }
        } else if (strncmp(p, "(tags ", 6) == 0 && out_tags && !out_tags[0]) {
            const char *start = strchr(p, '"');
            if (start) {
                start++;
                const char *end = strchr(start, '"');
                if (end) {
                    size_t len = (size_t)(end - start);
                    if (len >= tags_sz) len = tags_sz - 1;
                    memcpy(out_tags, start, len);
                    out_tags[len] = '\0';
                }
            }
        }
    }
    fclose(f);
    return pads;
}

typedef struct {
    const char *query;
    int         pads_filter;
    const char *lib_filter;
    int         max_results;
    int         matches_so_far;
    int         info_hit;
    const char *info_lib;
    const char *info_fp;
    int         list_mode;
    int         aborted;
} fp_search_ctx_t;

static void fp_match_and_emit(fp_search_ctx_t *ctx, const char *lib_name,
                               const char *fp_name, const char *path)
{
    if (ctx->lib_filter && strcmp(lib_name, ctx->lib_filter) != 0) return;

    if (ctx->info_lib) {
        if (strcmp(lib_name, ctx->info_lib) != 0) return;
        if (strcmp(fp_name,  ctx->info_fp)  != 0) return;

        char desc[512], tags[256];
        size_t pads = scan_mod_header(path, desc, sizeof(desc), tags, sizeof(tags));
        printf("lib:       %s\n", lib_name);
        printf("name:      %s\n", fp_name);
        printf("pad_count: %zu\n", pads);
        if (tags[0]) printf("tags:      %s\n", tags);
        if (desc[0]) printf("description: %s\n", desc);
        printf("path:      %s\n", path);
        ctx->info_hit = 1;
        ctx->aborted  = 1;
        return;
    }

    char full[256];
    snprintf(full, sizeof(full), "%s:%s", lib_name, fp_name);
    if (!ctx->list_mode && ctx->query && !istrstr_(full, ctx->query)) return;

    /* Peek pad count; only open the file when necessary (filters demand it
     * or we're listing). */
    char desc[512] = "", tags[256] = "";
    size_t pads = scan_mod_header(path, desc, sizeof(desc), tags, sizeof(tags));
    if (ctx->pads_filter >= 0 && (size_t)ctx->pads_filter != pads) return;

    printf("%s:%s\t%zu\t%s\n", lib_name, fp_name, pads, tags);

    ctx->matches_so_far++;
    if (ctx->max_results > 0 && ctx->matches_so_far >= ctx->max_results) {
        ctx->aborted = 1;
    }
}

/* Scan a `.pretty` folder. Library name = folder basename minus ".pretty". */
static void scan_pretty_dir(const char *pretty_path, const char *lib_name,
                             fp_search_ctx_t *ctx)
{
    kicli_dir_t *d = kicli_opendir(pretty_path);
    if (!d) return;

    const char *name;
    while ((name = kicli_readdir(d)) != NULL && !ctx->aborted) {
        size_t nl = strlen(name);
        if (nl <= 10 || strcmp(name + nl - 10, ".kicad_mod") != 0) continue;

        char fp_name[192];
        size_t stem = nl - 10;
        if (stem >= sizeof(fp_name)) stem = sizeof(fp_name) - 1;
        memcpy(fp_name, name, stem);
        fp_name[stem] = '\0';

        char path[KICLI_PATH_MAX];
        snprintf(path, sizeof(path), "%s%c%s", pretty_path, KICLI_PATH_SEP, name);

        fp_match_and_emit(ctx, lib_name, fp_name, path);
    }
    kicli_closedir(d);
}

/* Scan every `.pretty` dir inside `root`. */
static void scan_fp_root(const char *root, fp_search_ctx_t *ctx)
{
    kicli_dir_t *d = kicli_opendir(root);
    if (!d) return;
    const char *name;
    while ((name = kicli_readdir(d)) != NULL && !ctx->aborted) {
        size_t nl = strlen(name);
        if (nl <= 7 || strcmp(name + nl - 7, ".pretty") != 0) continue;

        char lib_name[128];
        size_t stem = nl - 7;
        if (stem >= sizeof(lib_name)) stem = sizeof(lib_name) - 1;
        memcpy(lib_name, name, stem);
        lib_name[stem] = '\0';

        char pretty[KICLI_PATH_MAX];
        snprintf(pretty, sizeof(pretty), "%s%c%s", root, KICLI_PATH_SEP, name);
        scan_pretty_dir(pretty, lib_name, ctx);
    }
    kicli_closedir(d);
}

static void scan_fp_sources(const char *project_dir, fp_search_ctx_t *ctx)
{
    char sym_root[1024], fp_root[1024];
    resolve_bundled_roots(sym_root, sizeof(sym_root), fp_root, sizeof(fp_root));
    scan_fp_root(fp_root, ctx);
    if (ctx->aborted) return;

    if (project_dir) {
        char proj_fp[KICLI_PATH_MAX];
        snprintf(proj_fp, sizeof(proj_fp), "%s%clibs%cfootprints",
                 project_dir, KICLI_PATH_SEP, KICLI_PATH_SEP);
        scan_fp_root(proj_fp, ctx);
    }
}

/* ── Dispatchers ─────────────────────────────────────────────────────────── */

static void sym_help(void)
{
    printf("Usage: kicli sym <command> [args]\n\n");
    printf("Commands:\n");
    printf("  search <query> [--pins N] [--lib L] [-n N] [--project D]\n");
    printf("                      Search bundled + project symbol libraries.\n");
    printf("                      Output (tab-sep): lib:name\\tpins\\tdescription\n");
    printf("  info <lib:name> [--pins] [--project D]\n");
    printf("                      Show one symbol's metadata. --pins also dumps\n");
    printf("                      NUM\\tNAME\\tTYPE for every pin.\n");
    printf("  list [lib] [--project D]\n");
    printf("                      List every symbol (optionally within one lib).\n");
}

static void fp_help(void)
{
    printf("Usage: kicli fp <command> [args]\n\n");
    printf("Commands:\n");
    printf("  search <query> [--pads N] [--lib L] [-n N] [--project D]\n");
    printf("                      Search bundled + project footprint libraries.\n");
    printf("                      Output (tab-sep): lib:name\\tpads\\ttags\n");
    printf("  info <lib:name> [--project D]\n");
    printf("                      Show one footprint's metadata + path.\n");
    printf("  list [lib] [--project D]\n");
    printf("                      List every footprint (optionally in one lib).\n");
}

/* Parse --project <dir>; returns a pointer into argv. */
static const char *extract_project_arg(int argc, char **argv)
{
    for (int i = 0; i < argc - 1; i++) {
        if (strcmp(argv[i], "--project") == 0) return argv[i + 1];
    }
    return NULL;
}

/* Resolve effective project root: --project wins, else walk from cwd. */
static const char *resolve_project_root(const char *arg, char *buf, size_t bufsz)
{
    if (arg) return arg;
    if (find_project_root(".", buf, bufsz) == 0) return buf;
    return NULL;
}

int cmd_sym(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        sym_help();
        return 0;
    }

    const char *sub = argv[1];

    char proj_buf[KICLI_PATH_MAX];
    const char *proj = resolve_project_root(extract_project_arg(argc - 2, argv + 2),
                                             proj_buf, sizeof(proj_buf));

    if (strcmp(sub, "list") == 0) {
        sym_search_ctx_t ctx = { .pins_filter = -1, .list_mode = 1 };
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) { i++; continue; }
            if (argv[i][0] != '-' && !ctx.lib_filter) ctx.lib_filter = argv[i];
        }
        scan_sym_sources(proj, &ctx);
        return 0;
    }

    if (strcmp(sub, "search") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: kicli sym search <query> [--pins N] [--lib L] [-n N]\n"); return 1; }
        sym_search_ctx_t ctx = { .query = argv[2], .pins_filter = -1, .max_results = 0 };
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) { i++; continue; }
            else if (strcmp(argv[i], "--pins") == 0 && i + 1 < argc) ctx.pins_filter = atoi(argv[++i]);
            else if (strcmp(argv[i], "--lib")  == 0 && i + 1 < argc) ctx.lib_filter = argv[++i];
            else if (strcmp(argv[i], "-n")     == 0 && i + 1 < argc) ctx.max_results = atoi(argv[++i]);
        }
        scan_sym_sources(proj, &ctx);
        if (ctx.matches_so_far == 0) { fprintf(stderr, "no matches\n"); return 2; }
        return 0;
    }

    if (strcmp(sub, "info") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: kicli sym info <lib:name> [--pins]\n"); return 1; }
        const char *lib_id = argv[2];
        const char *colon = strchr(lib_id, ':');
        if (!colon) { fprintf(stderr, "error: expected LIB:NAME, got '%s'\n", lib_id); return 1; }
        char lib_name[128];
        size_t ln = (size_t)(colon - lib_id);
        if (ln >= sizeof(lib_name)) ln = sizeof(lib_name) - 1;
        memcpy(lib_name, lib_id, ln); lib_name[ln] = '\0';

        sym_search_ctx_t ctx = { .info_lib = lib_name, .info_sym = colon + 1,
                                  .pins_filter = -1, .info_show_pins = 0 };
        for (int i = 3; i < argc; i++)
            if (strcmp(argv[i], "--pins") == 0) ctx.info_show_pins = 1;

        scan_sym_sources(proj, &ctx);
        if (!ctx.info_hit) {
            fprintf(stderr, "error: %s not found\n", lib_id);
            return 2;
        }
        return 0;
    }

    fprintf(stderr, "error: unknown sym command '%s'\n", sub);
    return 1;
}

int cmd_fp(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        fp_help();
        return 0;
    }

    const char *sub = argv[1];

    char proj_buf[KICLI_PATH_MAX];
    const char *proj = resolve_project_root(extract_project_arg(argc - 2, argv + 2),
                                             proj_buf, sizeof(proj_buf));

    if (strcmp(sub, "list") == 0) {
        fp_search_ctx_t ctx = { .pads_filter = -1, .list_mode = 1 };
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) { i++; continue; }
            if (argv[i][0] != '-' && !ctx.lib_filter) ctx.lib_filter = argv[i];
        }
        scan_fp_sources(proj, &ctx);
        return 0;
    }

    if (strcmp(sub, "search") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: kicli fp search <query> [--pads N] [--lib L] [-n N]\n"); return 1; }
        fp_search_ctx_t ctx = { .query = argv[2], .pads_filter = -1 };
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) { i++; continue; }
            else if (strcmp(argv[i], "--pads") == 0 && i + 1 < argc) ctx.pads_filter = atoi(argv[++i]);
            else if (strcmp(argv[i], "--lib")  == 0 && i + 1 < argc) ctx.lib_filter = argv[++i];
            else if (strcmp(argv[i], "-n")     == 0 && i + 1 < argc) ctx.max_results = atoi(argv[++i]);
        }
        scan_fp_sources(proj, &ctx);
        if (ctx.matches_so_far == 0) { fprintf(stderr, "no matches\n"); return 2; }
        return 0;
    }

    if (strcmp(sub, "info") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: kicli fp info <lib:name>\n"); return 1; }
        const char *lib_id = argv[2];
        const char *colon = strchr(lib_id, ':');
        if (!colon) { fprintf(stderr, "error: expected LIB:NAME, got '%s'\n", lib_id); return 1; }
        char lib_name[128];
        size_t ln = (size_t)(colon - lib_id);
        if (ln >= sizeof(lib_name)) ln = sizeof(lib_name) - 1;
        memcpy(lib_name, lib_id, ln); lib_name[ln] = '\0';

        fp_search_ctx_t ctx = { .info_lib = lib_name, .info_fp = colon + 1,
                                 .pads_filter = -1 };
        scan_fp_sources(proj, &ctx);
        if (!ctx.info_hit) {
            fprintf(stderr, "error: %s not found\n", lib_id);
            return 2;
        }
        return 0;
    }

    fprintf(stderr, "error: unknown fp command '%s'\n", sub);
    return 1;
}
