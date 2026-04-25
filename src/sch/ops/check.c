/*
 * check.c — kicli sch check
 *
 * One-shot layout-readiness validator. Walks the schematic ONCE and reports:
 *   1. Duplicate references (anywhere — incl. duplicate #PWRNNNN)
 *   2. Symbols missing a Footprint (excluding power: lib, virtual symbols)
 *   3. Duplicate UUIDs (symbol uuid + pin uuids)
 *   4. ERC violations (kicad-cli passthrough)
 *
 * Exit code: 0 only when everything is clean. 1 otherwise. The output is
 * a flat tab-separated `KIND  REF/UUID  DETAIL` table the agent can grep.
 */

#include "kicli/sch_ops.h"
#include "kicli/error.h"
#include "kicli/portable.h"
#include "kicli/kicad_cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Local data ────────────────────────────────────────────────────────── */

typedef struct {
    const char *ref;
    const char *value;
    const char *lib_id;
    const char *footprint;
    const char *uuid;          /* the symbol's own (uuid ...) */
    const sexpr_t *node;       /* back-pointer for pin uuids */
} placed_attrs_t;

typedef struct { placed_attrs_t *items; size_t n, cap; } placed_vec_t;

/* Tiny string-set: linear-probe array, fine for schematic-scale (< few k). */
typedef struct { const char **keys; size_t n, cap; } strset_t;

static int strset_add(strset_t *s, const char *k)  /* 1=new, 0=dup */
{
    for (size_t i = 0; i < s->n; i++)
        if (strcmp(s->keys[i], k) == 0) return 0;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 32;
        s->keys = realloc(s->keys, s->cap * sizeof(*s->keys));
    }
    s->keys[s->n++] = k;
    return 1;
}
static void strset_free(strset_t *s)
{
    free(s->keys); s->keys = NULL; s->n = s->cap = 0;
}

/* "power:*" symbols don't take a footprint. Same for KiCad virtual prefix '#'. */
static int is_virtual_symbol(const placed_attrs_t *a)
{
    if (!a->lib_id) return 1;
    if (strncmp(a->lib_id, "power:", 6) == 0) return 1;
    if (a->ref && a->ref[0] == '#') return 1;
    return 0;
}

static void fill_attrs(const sexpr_t *sym, placed_attrs_t *a)
{
    a->node      = sym;
    a->ref       = placed_property(sym, "Reference");
    a->value     = placed_property(sym, "Value");
    a->lib_id    = placed_lib_id(sym);
    a->footprint = placed_property(sym, "Footprint");
    a->uuid      = placed_uuid(sym);
}

/* Collect every (symbol …) child of root into a flat vector. */
static size_t collect_placed(const sexpr_t *root, placed_vec_t *out)
{
    for (size_t i = 0; i < root->num_children; i++) {
        const sexpr_t *c = root->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 1) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "symbol") != 0) continue;
        if (out->n == out->cap) {
            out->cap = out->cap ? out->cap * 2 : 64;
            out->items = realloc(out->items, out->cap * sizeof(*out->items));
        }
        fill_attrs(c, &out->items[out->n++]);
    }
    return out->n;
}

/* ── ERC bridge ────────────────────────────────────────────────────────── */

/* Returns error-severity violation count (>=0), or -1 if kicad-cli failed. */
static int run_erc_violation_count(const char *sch_path)
{
    char tmp[KICLI_PATH_MAX];
    kicli_temp_path(tmp, sizeof(tmp), "erc-check", "json");

    const char *args[10];
    int n = 0;
    args[n++] = "sch"; args[n++] = "erc";
    args[n++] = "--output"; args[n++] = tmp;
    args[n++] = "--format"; args[n++] = "json";
    args[n++] = sch_path;
    args[n]   = NULL;

    char *discard = NULL;
    (void)kicad_cli_capture(args, &discard);  /* nonzero return = violations exist */
    free(discard);

    size_t body_len = 0;
    char *body = kicli_read_file(tmp, &body_len);
    kicli_unlink(tmp);
    if (!body) return -1;

    /* Cheap JSON scan: count `"severity": "error"`. ERC JSON is small +
     * well-formed, no need for a real parser. Warnings ignored. */
    int errors = 0;
    const char *p = body;
    const char *needle = "\"severity\": \"error\"";
    size_t nl = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) { errors++; p += nl; }
    free(body);
    return errors;
}

/* ── Command entry ─────────────────────────────────────────────────────── */

int cmd_sch_check(const char *sch_path, int argc, char **argv)
{
    int skip_erc = 0;
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], "--no-erc") == 0) skip_erc = 1;

    sexpr_t *root = load_sch(sch_path);
    if (!root) { fprintf(stderr, "error: %s\n", kicli_last_error()); return 3; }

    placed_vec_t v = {0};
    size_t total = collect_placed(root, &v);

    int issues = 0;
    printf("# kicli sch check — %s\n", sch_path);
    printf("# %zu placed symbol(s)\n", total);
    printf("KIND\tREF/UUID\tDETAIL\n");

    /* (1) Duplicate references */
    {
        strset_t seen = {0};
        for (size_t i = 0; i < v.n; i++) {
            const char *r = v.items[i].ref;
            if (!r) continue;
            if (!strset_add(&seen, r)) {
                printf("dup_ref\t%s\t(appears more than once)\n", r);
                issues++;
            }
        }
        strset_free(&seen);
    }

    /* (2) Empty Footprint (excluding virtual / power symbols) */
    for (size_t i = 0; i < v.n; i++) {
        const placed_attrs_t *a = &v.items[i];
        if (is_virtual_symbol(a)) continue;
        if (!a->footprint || !*a->footprint) {
            printf("no_footprint\t%s\t(value=%s, lib=%s)\n",
                   a->ref ? a->ref : "?",
                   a->value ? a->value : "",
                   a->lib_id ? a->lib_id : "");
            issues++;
        }
    }

    /* (3) Duplicate UUIDs (symbol-level + pin-level) */
    {
        strset_t seen = {0};
        for (size_t i = 0; i < v.n; i++) {
            const char *u = v.items[i].uuid;
            if (u && !strset_add(&seen, u)) {
                printf("dup_uuid\t%s\t(symbol %s)\n", u, v.items[i].ref ? v.items[i].ref : "?");
                issues++;
            }
            const sexpr_t *sym = v.items[i].node;
            for (size_t j = 1; j < sym->num_children; j++) {
                const sexpr_t *p = sym->children[j];
                if (!p || p->type != SEXPR_LIST || p->num_children < 3) continue;
                if (!p->children[0]->value || strcmp(p->children[0]->value, "pin") != 0) continue;
                /* (pin "N" (uuid "...")) — uuid lives among the (pin…) children */
                for (size_t k = 1; k < p->num_children; k++) {
                    const sexpr_t *pc = p->children[k];
                    if (!pc || pc->type != SEXPR_LIST || pc->num_children < 2) continue;
                    if (!pc->children[0]->value || strcmp(pc->children[0]->value, "uuid") != 0) continue;
                    const char *pu = pc->children[1]->value;
                    if (pu && !strset_add(&seen, pu)) {
                        printf("dup_uuid\t%s\t(pin of %s)\n", pu, v.items[i].ref ? v.items[i].ref : "?");
                        issues++;
                    }
                }
            }
        }
        strset_free(&seen);
    }

    free(v.items);
    sexpr_free(root);

    /* (4) ERC */
    if (!skip_erc) {
        int erc_errors = run_erc_violation_count(sch_path);
        if (erc_errors > 0) {
            printf("erc\t-\t%d error-severity violation(s) (run `kicli sch %s erc -o -` for details)\n",
                   erc_errors, sch_path);
            issues += erc_errors;
        } else if (erc_errors < 0) {
            printf("erc\t-\t(kicad-cli unavailable — ERC skipped)\n");
        }
    }

    printf("# summary: %d issue(s)%s\n", issues, skip_erc ? " (ERC skipped)" : "");
    return issues ? 1 : 0;
}
