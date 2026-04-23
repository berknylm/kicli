/*
 * import.c — kicli import <file.zip> [-l library] [--project dir]
 *
 * Import vendor component ZIP (SnapEDA, Ultra Librarian, CSE) into
 * a KiCad project's local library structure:
 *   libs/symbols/<lib>.kicad_sym
 *   libs/footprints/<lib>.pretty/<component>.kicad_mod
 *   libs/3dmodels/<lib>/<component>.step
 *
 * Also registers the library in sym-lib-table / fp-lib-table.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include "kicli/portable.h"
#include "kicli/import.h"
#include "kicli/sch.h"
#include "kicli/error.h"

#define CLR_RESET  "\x1b[0m"
#define CLR_BOLD   "\x1b[1m"
#define CLR_RED    "\x1b[31m"
#define CLR_GREEN  "\x1b[32m"
#define CLR_CYAN   "\x1b[36m"
#define CLR_DIM    "\x1b[2m"

/* ── Discovered files from zip ─────────────────────────────────────────── */

typedef struct {
    char symbol[1024];      /* .kicad_sym path */
    char legacy_sym[1024];  /* .lib path (needs conversion) */
    char footprint[1024];   /* .kicad_mod path */
    char model3d[1024];     /* .stp/.step path */
    char component[256];    /* component name (from zip stem) */
} discovered_t;

/* ── Helpers ────────────────────────────────────────────────────────────── */

static int ensure_dir(const char *path)
{
    if (kicli_is_dir(path)) return 0;
    if (kicli_mkdir(path) != 0) {
        fprintf(stderr, CLR_RED "error:" CLR_RESET " cannot create '%s': %s\n",
                path, strerror(errno));
        return -1;
    }
    return 0;
}

static void copy_file(const char *src, const char *dst)
{
    FILE *in  = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    if (!in || !out) {
        if (in)  fclose(in);
        if (out) fclose(out);
        return;
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
}

static char *read_file_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Normalize a filename for use as library/component name */
static void normalize_name(char *out, size_t outsz, const char *filename)
{
    /* strip vendor prefixes */
    if (strncmp(filename, "ul_", 3) == 0)  filename += 3;
    if (strncmp(filename, "UL_", 3) == 0)  filename += 3;
    if (strncmp(filename, "LIB_", 4) == 0) filename += 4;

    size_t i = 0;
    for (; *filename && i < outsz - 1; filename++) {
        char c = *filename;
        if (c == ' ' || c == '-') c = '_';
        out[i++] = c;
    }
    out[i] = '\0';
}

/* Find .kicad_pro in dir or ancestors (up to 3 levels) */
static int find_project_dir(const char *start, char *out, size_t outsz)
{
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", start);

    for (int depth = 0; depth < 4; depth++) {
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
        /* go up one level */
        char *sep = strrchr(dir, KICLI_PATH_SEP);
        if (!sep) break;
        *sep = '\0';
    }
    return -1;
}

/* ── Unzip ──────────────────────────────────────────────────────────────── */

static int unzip_file(const char *zip_path, const char *dest_dir)
{
    if (ensure_dir(dest_dir) != 0) return -1;

    char cmd[2048];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd),
        "powershell -NoProfile -Command \"Expand-Archive -Force -Path '%s' -DestinationPath '%s'\"",
        zip_path, dest_dir);
#else
    snprintf(cmd, sizeof(cmd),
        "unzip -o -q \"%s\" -d \"%s\" 2>/dev/null", zip_path, dest_dir);
#endif

    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, CLR_RED "error:" CLR_RESET
                " failed to unzip '%s' (exit %d)\n", zip_path, rc);
        return -1;
    }
    return 0;
}

/* ── Recursive file discovery ───────────────────────────────────────────── */

static int has_suffix(const char *name, const char *suffix)
{
    size_t nl = strlen(name), sl = strlen(suffix);
    if (nl < sl) return 0;
    return strcmp(name + nl - sl, suffix) == 0;
}

static void discover_recursive(const char *dir, discovered_t *d, int in_kicad_dir)
{
    kicli_dir_t *dp = kicli_opendir(dir);
    if (!dp) return;

    const char *name;
    while ((name = kicli_readdir(dp)) != NULL) {
        if (name[0] == '.') continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s%c%s", dir, KICLI_PATH_SEP, name);

        if (kicli_is_dir(path)) {
            int is_kicad = (strcmp(name, "KiCad") == 0);
            discover_recursive(path, d, in_kicad_dir || is_kicad);
            continue;
        }

        /* CSE "KiCad/" subdir gets priority */
        int dominated = (!in_kicad_dir && d->symbol[0] && d->footprint[0]);
        if (dominated) continue;

        if (!d->symbol[0] && has_suffix(name, ".kicad_sym"))
            snprintf(d->symbol, sizeof(d->symbol), "%s", path);
        else if (!d->legacy_sym[0] && has_suffix(name, ".lib"))
            snprintf(d->legacy_sym, sizeof(d->legacy_sym), "%s", path);

        if (!d->footprint[0] && has_suffix(name, ".kicad_mod"))
            snprintf(d->footprint, sizeof(d->footprint), "%s", path);

        if (!d->model3d[0] && (has_suffix(name, ".stp") ||
                                has_suffix(name, ".step") ||
                                has_suffix(name, ".STEP") ||
                                has_suffix(name, ".STP")))
            snprintf(d->model3d, sizeof(d->model3d), "%s", path);

        /* if we found items in KiCad dir, overwrite non-KiCad findings */
        if (in_kicad_dir) {
            if (has_suffix(name, ".kicad_sym"))
                snprintf(d->symbol, sizeof(d->symbol), "%s", path);
            if (has_suffix(name, ".kicad_mod"))
                snprintf(d->footprint, sizeof(d->footprint), "%s", path);
        }
    }
    kicli_closedir(dp);
}

/* ── Symbol import ──────────────────────────────────────────────────────── */

/*
 * Update footprint reference inside a .kicad_sym to point at the project
 * library: library_name:component_name
 */
static void fix_footprint_ref(sexpr_t *root, const char *lib_name,
                               const char *comp_name)
{
    if (!root || root->type != SEXPR_LIST) return;

    for (size_t i = 0; i < root->num_children; i++) {
        sexpr_t *c = root->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 3) {
            if (c) fix_footprint_ref(c, lib_name, comp_name);
            continue;
        }

        sexpr_t *tag = c->children[0];
        if (!tag || tag->type != SEXPR_ATOM ||
            strcmp(tag->value, "property") != 0) {
            fix_footprint_ref(c, lib_name, comp_name);
            continue;
        }

        sexpr_t *key = c->children[1];
        if (!key || !key->value || strcmp(key->value, "Footprint") != 0) {
            fix_footprint_ref(c, lib_name, comp_name);
            continue;
        }

        /* Replace footprint value */
        sexpr_t *val = c->children[2];
        if (val && val->value) {
            free(val->value);
            char fp_ref[512];
            snprintf(fp_ref, sizeof(fp_ref), "%s:%s", lib_name, comp_name);
            val->value = strdup(fp_ref);
        }
    }
}

static int import_symbol(const char *sym_path, const char *proj_dir,
                          const char *lib_name, const char *comp_name)
{
    char lib_path[1024];
    snprintf(lib_path, sizeof(lib_path), "%s/libs/symbols/%s.kicad_sym",
             proj_dir, lib_name);

    /* Read the source symbol file */
    char *src_text = read_file_text(sym_path);
    if (!src_text) {
        fprintf(stderr, CLR_RED "error:" CLR_RESET " cannot read '%s'\n", sym_path);
        return -1;
    }

    char errbuf[256];
    sexpr_t *src_root = sexpr_parse(src_text, errbuf, sizeof(errbuf));
    free(src_text);
    if (!src_root) {
        fprintf(stderr, CLR_RED "error:" CLR_RESET " parse '%s': %s\n",
                sym_path, errbuf);
        return -1;
    }

    /* Fix footprint references */
    fix_footprint_ref(src_root, lib_name, comp_name);

    if (!kicli_exists(lib_path)) {
        /* New library — write the source directly */
        if (sexpr_write_file(src_root, lib_path) != 0) {
            fprintf(stderr, CLR_RED "error:" CLR_RESET
                    " cannot write '%s'\n", lib_path);
            sexpr_free(src_root);
            return -1;
        }
        printf(CLR_DIM "  write  %s" CLR_RESET "\n", lib_path);
    } else {
        /* Existing library — append symbol nodes */
        char *dst_text = read_file_text(lib_path);
        if (!dst_text) { sexpr_free(src_root); return -1; }

        sexpr_t *dst_root = sexpr_parse(dst_text, errbuf, sizeof(errbuf));
        free(dst_text);
        if (!dst_root) { sexpr_free(src_root); return -1; }

        /* Check if component already exists */
        size_t sym_count = 0;
        sexpr_t **existing = sexpr_get_all(dst_root, "symbol", &sym_count);
        for (size_t i = 0; i < sym_count; i++) {
            if (existing[i]->num_children >= 2 &&
                existing[i]->children[1]->value &&
                strcmp(existing[i]->children[1]->value, comp_name) == 0) {
                printf(CLR_DIM "  skip   symbol '%s' already in library"
                       CLR_RESET "\n", comp_name);
                free(existing);
                sexpr_free(src_root);
                sexpr_free(dst_root);
                return 0;
            }
        }
        free(existing);

        /* Extract symbol nodes from source and append to dest */
        size_t src_sym_count = 0;
        sexpr_t **src_syms = sexpr_get_all(src_root, "symbol", &src_sym_count);
        for (size_t i = 0; i < src_sym_count; i++) {
            /* Detach from source so it won't be freed with src_root */
            sexpr_t *sym_copy = src_syms[i];
            /* We need to actually add the node; since sexpr_free would
               free it, we set the source pointer to NULL to prevent
               double-free. We do a shallow copy approach. */
            sexpr_list_append(dst_root, sym_copy);

            /* Null out in source to prevent double free */
            for (size_t j = 0; j < src_root->num_children; j++) {
                if (src_root->children[j] == sym_copy)
                    src_root->children[j] = NULL;
            }
        }
        free(src_syms);

        if (sexpr_write_file(dst_root, lib_path) != 0) {
            fprintf(stderr, CLR_RED "error:" CLR_RESET
                    " cannot write '%s'\n", lib_path);
            sexpr_free(src_root);
            sexpr_free(dst_root);
            return -1;
        }
        printf(CLR_DIM "  append %s" CLR_RESET "\n", lib_path);
        sexpr_free(dst_root);
    }

    sexpr_free(src_root);
    return 0;
}

/* ── Footprint import ───────────────────────────────────────────────────── */

static int import_footprint(const char *fp_path, const char *proj_dir,
                             const char *lib_name, const char *comp_name)
{
    char pretty_dir[1024], dest[1024];
    snprintf(pretty_dir, sizeof(pretty_dir), "%s/libs/footprints/%s.pretty",
             proj_dir, lib_name);
    if (ensure_dir(pretty_dir) != 0) return -1;

    snprintf(dest, sizeof(dest), "%s/%s.kicad_mod", pretty_dir, comp_name);
    copy_file(fp_path, dest);
    printf(CLR_DIM "  copy   %s" CLR_RESET "\n", dest);
    return 0;
}

/* ── 3D model import ────────────────────────────────────────────────────── */

static int import_3dmodel(const char *model_path, const char *proj_dir,
                           const char *lib_name, const char *comp_name)
{
    char model_dir[1024], dest[1024];
    snprintf(model_dir, sizeof(model_dir), "%s/libs/3dmodels/%s",
             proj_dir, lib_name);
    if (ensure_dir(model_dir) != 0) return -1;

    /* preserve original extension */
    const char *ext = strrchr(model_path, '.');
    if (!ext) ext = ".step";

    snprintf(dest, sizeof(dest), "%s/%s%s", model_dir, comp_name, ext);
    copy_file(model_path, dest);
    printf(CLR_DIM "  copy   %s" CLR_RESET "\n", dest);
    return 0;
}

/* ── Library table registration ─────────────────────────────────────────── */

/*
 * Build a (lib ...) s-expression node:
 *   (lib (name "X") (type "KiCad") (uri "...") (options "") (descr "..."))
 */
static sexpr_t *make_lib_entry(const char *name, const char *uri,
                                const char *descr)
{
    sexpr_t *lib = sexpr_make_list();

    sexpr_list_append(lib, sexpr_make_atom("lib"));

    sexpr_t *n = sexpr_make_list();
    sexpr_list_append(n, sexpr_make_atom("name"));
    sexpr_list_append(n, sexpr_make_str(name));
    sexpr_list_append(lib, n);

    sexpr_t *t = sexpr_make_list();
    sexpr_list_append(t, sexpr_make_atom("type"));
    sexpr_list_append(t, sexpr_make_str("KiCad"));
    sexpr_list_append(lib, t);

    sexpr_t *u = sexpr_make_list();
    sexpr_list_append(u, sexpr_make_atom("uri"));
    sexpr_list_append(u, sexpr_make_str(uri));
    sexpr_list_append(lib, u);

    sexpr_t *o = sexpr_make_list();
    sexpr_list_append(o, sexpr_make_atom("options"));
    sexpr_list_append(o, sexpr_make_str(""));
    sexpr_list_append(lib, o);

    sexpr_t *d = sexpr_make_list();
    sexpr_list_append(d, sexpr_make_atom("descr"));
    sexpr_list_append(d, sexpr_make_str(descr));
    sexpr_list_append(lib, d);

    return lib;
}

static int lib_table_has_entry(sexpr_t *root, const char *name)
{
    size_t count = 0;
    sexpr_t **libs = sexpr_get_all(root, "lib", &count);
    for (size_t i = 0; i < count; i++) {
        const char *n = sexpr_atom_value(libs[i], "name");
        if (n && strcmp(n, name) == 0) {
            free(libs);
            return 1;
        }
    }
    free(libs);
    return 0;
}

static int register_lib_table(const char *table_path, const char *lib_name,
                               const char *uri, const char *descr,
                               const char *table_tag)
{
    if (!kicli_exists(table_path)) {
        /* Create new lib table */
        sexpr_t *root = sexpr_make_list();
        sexpr_list_append(root, sexpr_make_atom(table_tag));

        sexpr_t *ver = sexpr_make_list();
        sexpr_list_append(ver, sexpr_make_atom("version"));
        sexpr_list_append(ver, sexpr_make_atom("7"));
        sexpr_list_append(root, ver);

        sexpr_list_append(root, make_lib_entry(lib_name, uri, descr));

        if (sexpr_write_file(root, table_path) != 0) {
            sexpr_free(root);
            return -1;
        }
        sexpr_free(root);
        printf(CLR_DIM "  write  %s" CLR_RESET "\n", table_path);
        return 0;
    }

    /* Parse existing */
    char *text = read_file_text(table_path);
    if (!text) return -1;

    char errbuf[256];
    sexpr_t *root = sexpr_parse(text, errbuf, sizeof(errbuf));
    free(text);
    if (!root) {
        fprintf(stderr, CLR_RED "error:" CLR_RESET " parse '%s': %s\n",
                table_path, errbuf);
        return -1;
    }

    if (lib_table_has_entry(root, lib_name)) {
        printf(CLR_DIM "  skip   '%s' already in %s" CLR_RESET "\n",
               lib_name, table_path);
        sexpr_free(root);
        return 0;
    }

    sexpr_list_append(root, make_lib_entry(lib_name, uri, descr));

    if (sexpr_write_file(root, table_path) != 0) {
        sexpr_free(root);
        return -1;
    }
    sexpr_free(root);
    printf(CLR_DIM "  update %s" CLR_RESET "\n", table_path);
    return 0;
}

/* ── List imported libraries ────────────────────────────────────────────── */

static int cmd_import_list(const char *proj_dir)
{
    char sym_table[1024];
    snprintf(sym_table, sizeof(sym_table), "%s/sym-lib-table", proj_dir);

    if (!kicli_exists(sym_table)) {
        printf("No libraries imported yet.\n");
        return 0;
    }

    char *text = read_file_text(sym_table);
    if (!text) { printf("No libraries imported yet.\n"); return 0; }

    char errbuf[256];
    sexpr_t *root = sexpr_parse(text, errbuf, sizeof(errbuf));
    free(text);
    if (!root) return 1;

    size_t count = 0;
    sexpr_t **libs = sexpr_get_all(root, "lib", &count);
    if (count == 0) {
        printf("No libraries found.\n");
    } else {
        for (size_t i = 0; i < count; i++) {
            const char *name = sexpr_atom_value(libs[i], "name");
            const char *uri  = sexpr_atom_value(libs[i], "uri");
            printf("%-30s  %s\n", name ? name : "?", uri ? uri : "?");
        }
    }
    free(libs);
    sexpr_free(root);
    return 0;
}

/* ── Entry point ────────────────────────────────────────────────────────── */

int cmd_import(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "-h") == 0) {
        printf("Usage: kicli import <file.zip> [-l <library>] [--project <dir>]\n");
        printf("       kicli import --list [--project <dir>]\n");
        printf("\nImport a vendor component ZIP into KiCad project libraries.\n");
        printf("Supports: SnapEDA, Ultra Librarian, Component Search Engine.\n");
        printf("\nOptions:\n");
        printf("  -l <library>     Library name (default: derived from filename)\n");
        printf("  --project <dir>  Project directory (default: auto-detect .kicad_pro)\n");
        printf("  --list           List imported libraries\n");
        printf("\nExamples:\n");
        printf("  kicli import ~/Downloads/LM358.zip -l op_amps\n");
        printf("  kicli import ~/Downloads/ul_RP2040.zip\n");
        printf("  kicli import --list\n");
        printf("\nThe ZIP is extracted, and symbols/footprints/3D models are placed in:\n");
        printf("  libs/symbols/<lib>.kicad_sym\n");
        printf("  libs/footprints/<lib>.pretty/<component>.kicad_mod\n");
        printf("  libs/3dmodels/<lib>/<component>.step\n");
        printf("sym-lib-table and fp-lib-table are updated automatically.\n");
        return 0;
    }

    /* Parse args */
    const char *zip_path = NULL;
    const char *lib_name_arg = NULL;
    const char *proj_arg = NULL;
    int list_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            list_mode = 1;
        } else if ((strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--library") == 0)
                   && i + 1 < argc) {
            lib_name_arg = argv[++i];
        } else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
            proj_arg = argv[++i];
        } else if (argv[i][0] != '-' && !zip_path) {
            zip_path = argv[i];
        }
    }

    /* Find project directory */
    char proj_dir[1024];
    if (proj_arg) {
        snprintf(proj_dir, sizeof(proj_dir), "%s", proj_arg);
    } else {
        if (find_project_dir(".", proj_dir, sizeof(proj_dir)) != 0) {
            fprintf(stderr, CLR_RED "error:" CLR_RESET
                    " no .kicad_pro found. Use --project <dir>\n");
            return 1;
        }
    }

    if (list_mode)
        return cmd_import_list(proj_dir);

    if (!zip_path) {
        fprintf(stderr, CLR_RED "error:" CLR_RESET " no ZIP file specified\n");
        return 1;
    }

    if (!kicli_exists(zip_path)) {
        fprintf(stderr, CLR_RED "error:" CLR_RESET
                " file not found: '%s'\n", zip_path);
        return 1;
    }

    /* Derive component name from zip filename */
    const char *base = strrchr(zip_path, KICLI_PATH_SEP);
    if (!base) base = strrchr(zip_path, '/');  /* handle mixed separators */
    base = base ? base + 1 : zip_path;

    /* Strip .zip extension */
    char stem[256];
    snprintf(stem, sizeof(stem), "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot && (strcmp(dot, ".zip") == 0 || strcmp(dot, ".ZIP") == 0))
        *dot = '\0';

    char comp_name[256];
    normalize_name(comp_name, sizeof(comp_name), stem);

    /* Library name: explicit or same as component */
    char lib_name[256];
    if (lib_name_arg) {
        snprintf(lib_name, sizeof(lib_name), "%s", lib_name_arg);
    } else {
        snprintf(lib_name, sizeof(lib_name), "%s", comp_name);
    }

    printf(CLR_BOLD "Importing '%s' → library '%s'..." CLR_RESET "\n",
           comp_name, lib_name);

    /* Ensure libs structure */
    char d[1024];
    snprintf(d, sizeof(d), "%s/libs", proj_dir);           ensure_dir(d);
    snprintf(d, sizeof(d), "%s/libs/symbols", proj_dir);    ensure_dir(d);
    snprintf(d, sizeof(d), "%s/libs/footprints", proj_dir); ensure_dir(d);
    snprintf(d, sizeof(d), "%s/libs/3dmodels", proj_dir);   ensure_dir(d);

    /* Unzip to temp */
    char tmp_dir[1024];
    snprintf(tmp_dir, sizeof(tmp_dir), "%s/libs/.tmp/%s", proj_dir, comp_name);
    snprintf(d, sizeof(d), "%s/libs/.tmp", proj_dir);
    ensure_dir(d);

    if (unzip_file(zip_path, tmp_dir) != 0) return 1;

    /* Discover files */
    discovered_t disc = {0};
    snprintf(disc.component, sizeof(disc.component), "%s", comp_name);
    discover_recursive(tmp_dir, &disc, 0);

    int has_sym = disc.symbol[0] != '\0';
    int has_fp  = disc.footprint[0] != '\0';
    int has_3d  = disc.model3d[0] != '\0';

    if (!has_sym && !has_fp && !has_3d) {
        fprintf(stderr, CLR_RED "error:" CLR_RESET
                " no KiCad files found in ZIP\n");
        fprintf(stderr, "  Expected: .kicad_sym, .kicad_mod, .stp/.step\n");
        kicli_rmrf(tmp_dir);
        return 1;
    }

    /* Import each type */
    int errors = 0;

    if (has_sym) {
        if (import_symbol(disc.symbol, proj_dir, lib_name, comp_name) != 0)
            errors++;
    } else if (disc.legacy_sym[0]) {
        fprintf(stderr, CLR_DIM "  warn   legacy .lib found — convert with"
                " 'kicad-cli sym upgrade' first" CLR_RESET "\n");
    }

    if (has_fp) {
        if (import_footprint(disc.footprint, proj_dir, lib_name, comp_name) != 0)
            errors++;
    }

    if (has_3d) {
        if (import_3dmodel(disc.model3d, proj_dir, lib_name, comp_name) != 0)
            errors++;
    }

    /* Register in lib tables */
    if (has_sym) {
        char sym_table[1024], sym_uri[512];
        snprintf(sym_table, sizeof(sym_table), "%s/sym-lib-table", proj_dir);
        snprintf(sym_uri, sizeof(sym_uri),
                 "${KIPRJMOD}/libs/symbols/%s.kicad_sym", lib_name);
        register_lib_table(sym_table, lib_name, sym_uri,
                           "Imported by kicli", "sym_lib_table");
    }

    if (has_fp) {
        char fp_table[1024], fp_uri[512];
        snprintf(fp_table, sizeof(fp_table), "%s/fp-lib-table", proj_dir);
        snprintf(fp_uri, sizeof(fp_uri),
                 "${KIPRJMOD}/libs/footprints/%s.pretty", lib_name);
        register_lib_table(fp_table, lib_name, fp_uri,
                           "Imported by kicli", "fp_lib_table");
    }

    /* Cleanup */
    kicli_rmrf(tmp_dir);

    /* Summary */
    printf("\n" CLR_GREEN CLR_BOLD "Done!" CLR_RESET " %s → %s",
           comp_name, lib_name);
    printf(" (");
    int first = 1;
    if (has_sym)  { printf("symbol");    first = 0; }
    if (has_fp)   { printf("%sfootprint", first ? "" : " + "); first = 0; }
    if (has_3d)   { printf("%s3D model",  first ? "" : " + "); }
    printf(")\n");

    if (errors > 0) {
        fprintf(stderr, CLR_RED "%d error(s) during import" CLR_RESET "\n", errors);
        return 1;
    }

    return 0;
}
