/*
 * set.c — kicli sch <file|dir> set / set-all
 *
 * set     <ref> <field> <value>          — set field on one component
 * set-all <value_match> <field> <value>  — set field on all components with
 *                                          matching value, across all .kicad_sch
 *                                          files in the given directory
 */

#include "kicli/sch.h"
#include "kicli/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#  include <windows.h>
#  ifndef S_ISDIR
#    define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#  endif
#else
#  include <dirent.h>
#endif

/* ── helpers ─────────────────────────────────────────────────────────────── */

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { kicli_set_error("cannot open '%s': %s", path, strerror(errno)); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); kicli_set_error("out of memory"); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* find a symbol node whose (property "Reference" ...) matches ref */
static sexpr_t *find_symbol_node(sexpr_t *root, const char *ref)
{
    for (size_t i = 0; i < root->num_children; i++) {
        sexpr_t *c = root->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children == 0) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "symbol") != 0) continue;

        /* look for (property "Reference" "<ref>") */
        for (size_t j = 1; j < c->num_children; j++) {
            sexpr_t *p = c->children[j];
            if (!p || p->type != SEXPR_LIST || p->num_children < 3) continue;
            if (!p->children[0]->value || strcmp(p->children[0]->value, "property") != 0) continue;
            if (!p->children[1]->value || strcmp(p->children[1]->value, "Reference") != 0) continue;
            if (!p->children[2]->value || strcmp(p->children[2]->value, ref) != 0) continue;
            return c;
        }
    }
    return NULL;
}

/* find existing (property "field" ...) inside a symbol node */
static sexpr_t *find_property_node(sexpr_t *sym, const char *field)
{
    for (size_t i = 1; i < sym->num_children; i++) {
        sexpr_t *p = sym->children[i];
        if (!p || p->type != SEXPR_LIST || p->num_children < 3) continue;
        if (!p->children[0]->value || strcmp(p->children[0]->value, "property") != 0) continue;
        if (!p->children[1]->value || strcmp(p->children[1]->value, field) != 0) continue;
        return p;
    }
    return NULL;
}

/* build (property "field" "value" (at 0 0 0) (effects (font (size 1.27 1.27)) (hide yes))) */
static sexpr_t *make_property_node(const char *field, const char *value)
{
    sexpr_t *prop = sexpr_make_list();
    sexpr_list_append(prop, sexpr_make_atom("property"));
    sexpr_list_append(prop, sexpr_make_str(field));
    sexpr_list_append(prop, sexpr_make_str(value));

    /* (at 0 0 0) */
    sexpr_t *at = sexpr_make_list();
    sexpr_list_append(at, sexpr_make_atom("at"));
    sexpr_list_append(at, sexpr_make_atom("0"));
    sexpr_list_append(at, sexpr_make_atom("0"));
    sexpr_list_append(at, sexpr_make_atom("0"));
    sexpr_list_append(prop, at);

    /* (effects (font (size 1.27 1.27)) (hide yes)) */
    sexpr_t *size = sexpr_make_list();
    sexpr_list_append(size, sexpr_make_atom("size"));
    sexpr_list_append(size, sexpr_make_atom("1.27"));
    sexpr_list_append(size, sexpr_make_atom("1.27"));

    sexpr_t *font = sexpr_make_list();
    sexpr_list_append(font, sexpr_make_atom("font"));
    sexpr_list_append(font, size);

    sexpr_t *hide = sexpr_make_list();
    sexpr_list_append(hide, sexpr_make_atom("hide"));
    sexpr_list_append(hide, sexpr_make_atom("yes"));

    sexpr_t *effects = sexpr_make_list();
    sexpr_list_append(effects, sexpr_make_atom("effects"));
    sexpr_list_append(effects, font);
    sexpr_list_append(effects, hide);

    sexpr_list_append(prop, effects);
    return prop;
}

/* ── cmd_sch_set ─────────────────────────────────────────────────────────── */

int cmd_sch_set(const char *sch_path, const char *ref, const char *field, const char *value)
{
    char *buf = read_file(sch_path);
    if (!buf) return 1;

    char errbuf[256] = {0};
    sexpr_t *root = sexpr_parse(buf, errbuf, sizeof(errbuf));
    free(buf);

    if (!root) {
        kicli_set_error("parse error: %s", errbuf[0] ? errbuf : "unknown");
        fprintf(stderr, "error: %s\n", kicli_last_error());
        return 1;
    }

    sexpr_t *sym = find_symbol_node(root, ref);
    if (!sym) {
        fprintf(stderr, "error: component '%s' not found\n", ref);
        sexpr_free(root);
        return 1;
    }

    sexpr_t *prop = find_property_node(sym, field);
    if (prop) {
        /* update existing value in place */
        char *newval = strdup(value);
        if (!newval) {
            fprintf(stderr, "error: out of memory\n");
            sexpr_free(root);
            return 1;
        }
        free(prop->children[2]->value);
        prop->children[2]->value = newval;
    } else {
        /* add new property node */
        sexpr_list_append(sym, make_property_node(field, value));
    }

    if (sexpr_write_file(root, sch_path) != 0) {
        fprintf(stderr, "error: cannot write '%s'\n", sch_path);
        sexpr_free(root);
        return 1;
    }

    sexpr_free(root);
    printf("set %s.%s = %s\n", ref, field, value);
    return 0;
}

/* ── cmd_sch_set_all ─────────────────────────────────────────────────────── */

/* simple glob: only supports * as wildcard */
static int glob_match(const char *pattern, const char *str)
{
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;
            for (; *str; str++)
                if (glob_match(pattern, str)) return 1;
            return 0;
        }
        if (*pattern != *str) return 0;
        pattern++; str++;
    }
    return *str == '\0';
}

/* apply set-all to a single file: match by value (+optional footprint), set field=new_val */
static int set_all_in_file(const char *sch_path,
                           const char *val_match, const char *fp_match,
                           const char *field, const char *new_val,
                           int dry_run)
{
    char *buf = read_file(sch_path);
    if (!buf) return 0; /* skip unreadable files */

    char errbuf[256] = {0};
    sexpr_t *root = sexpr_parse(buf, errbuf, sizeof(errbuf));
    free(buf);
    if (!root) return 0;

    int changed = 0;

    for (size_t i = 0; i < root->num_children; i++) {
        sexpr_t *c = root->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children == 0) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "symbol") != 0) continue;

        /* find Reference, Value, Footprint properties */
        const char *sym_ref = NULL, *sym_val = NULL, *sym_fp = NULL;
        for (size_t j = 1; j < c->num_children; j++) {
            sexpr_t *p = c->children[j];
            if (!p || p->type != SEXPR_LIST || p->num_children < 3) continue;
            if (!p->children[0]->value || strcmp(p->children[0]->value, "property") != 0) continue;
            if (!p->children[1]->value || !p->children[2]->value) continue;
            if (strcmp(p->children[1]->value, "Reference") == 0) sym_ref = p->children[2]->value;
            if (strcmp(p->children[1]->value, "Value")     == 0) sym_val = p->children[2]->value;
            if (strcmp(p->children[1]->value, "Footprint") == 0) sym_fp  = p->children[2]->value;
        }

        if (!sym_val || strcmp(sym_val, val_match) != 0) continue;
        if (!sym_ref || sym_ref[0] == '#') continue; /* skip power */
        if (fp_match && !glob_match(fp_match, sym_fp ? sym_fp : "")) continue;

        if (dry_run) {
            printf("  %s  %s.%s = %s (dry-run)\n", sch_path, sym_ref, field, new_val);
            changed++;
            continue;
        }

        sexpr_t *prop = find_property_node(c, field);
        if (prop) {
            char *newval = strdup(new_val);
            if (!newval) { sexpr_free(root); return 0; }
            free(prop->children[2]->value);
            prop->children[2]->value = newval;
        } else {
            sexpr_list_append(c, make_property_node(field, new_val));
        }
        printf("  %s  %s.%s = %s\n", sch_path, sym_ref, field, new_val);
        changed++;
    }

    if (changed && !dry_run) sexpr_write_file(root, sch_path);
    sexpr_free(root);
    return changed;
}

int cmd_sch_set_all(const char *path, int argc, char **argv)
{
    /* parse: <VALUE_MATCH> <FIELD> <NEW_VALUE> [--footprint PAT] [--dry-run] */
    if (argc < 3) {
        fprintf(stderr, "Usage: kicli sch <file|dir> set-all <VALUE> <FIELD> <NEW_VALUE>\n");
        fprintf(stderr, "  --footprint <glob>   Only match components with this footprint\n");
        fprintf(stderr, "  --dry-run            Preview changes without writing\n");
        return 1;
    }

    const char *val_match = argv[0];
    const char *field     = argv[1];
    const char *new_val   = argv[2];
    const char *fp_match  = NULL;
    int dry_run = 0;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--footprint") == 0 && i + 1 < argc)
            fp_match = argv[++i];
        else if (strcmp(argv[i], "--dry-run") == 0)
            dry_run = 1;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "error: cannot stat '%s': %s\n", path, strerror(errno));
        return 1;
    }

    int total = 0;

    if (S_ISDIR(st.st_mode)) {
        /* scan directory for *.kicad_sch */
#ifdef _WIN32
        char pattern[1024];
        snprintf(pattern, sizeof(pattern), "%s\\*.kicad_sch", path);
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(pattern, &fd);
        if (hFind == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "error: cannot open dir '%s'\n", path);
            return 1;
        }
        do {
            char fpath[1024];
            snprintf(fpath, sizeof(fpath), "%s\\%s", path, fd.cFileName);
            total += set_all_in_file(fpath, val_match, fp_match, field, new_val, dry_run);
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
#else
        DIR *dir = opendir(path);
        if (!dir) { fprintf(stderr, "error: cannot open dir '%s'\n", path); return 1; }
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            size_t nl = strlen(ent->d_name);
            if (nl < 10 || strcmp(ent->d_name + nl - 10, ".kicad_sch") != 0) continue;
            char fpath[1024];
            snprintf(fpath, sizeof(fpath), "%s/%s", path, ent->d_name);
            total += set_all_in_file(fpath, val_match, fp_match, field, new_val, dry_run);
        }
        closedir(dir);
#endif
    } else {
        total = set_all_in_file(path, val_match, fp_match, field, new_val, dry_run);
    }

    if (total == 0)
        printf("no components with value '%s' found\n", val_match);
    else if (dry_run)
        printf("%d component(s) would be updated\n", total);
    else
        printf("%d component(s) updated\n", total);
    return 0;
}
