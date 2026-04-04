/*
 * model.c — parse KiCad 10 s-expression schematic into kicli_schematic_t
 *
 * Only implements what M3 commands need:
 *   list, info, nets, tree, stats
 * Write-back (M5), diff, validate are stubs returning KICLI_ERR_NOT_IMPLEMENTED.
 */

#include "kicli/sch.h"
#include "kicli/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ── File reading ─────────────────────────────────────────────────────────── */

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        kicli_set_error("cannot open '%s': %s", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); kicli_set_error("ftell failed"); return NULL; }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); kicli_set_error("out of memory"); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* ── Property helpers ─────────────────────────────────────────────────────── */

/*
 * get_property_value: find (property "Key" "Value" ...) in a symbol node.
 * Returns pointer into tree — do NOT free.
 */
static const char *get_property_value(const sexpr_t *sym, const char *key)
{
    if (!sym) return NULL;
    for (size_t i = 0; i < sym->num_children; i++) {
        sexpr_t *c = sym->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 3) continue;
        /* (property "Key" "Value" ...) */
        sexpr_t *tag = c->children[0];
        if (!tag || tag->type != SEXPR_ATOM) continue;
        if (strcmp(tag->value, "property") != 0) continue;
        sexpr_t *k = c->children[1];
        if (!k || !k->value || strcmp(k->value, key) != 0) continue;
        sexpr_t *v = c->children[2];
        return v ? v->value : NULL;
    }
    return NULL;
}

/* ── Parse a placed symbol ────────────────────────────────────────────────── */

static int parse_symbol(const sexpr_t *node, kicli_symbol_t *out)
{
    memset(out, 0, sizeof(*out));

    /* lib_id */
    const char *lid = sexpr_atom_value(node, "lib_id");
    if (lid) snprintf(out->lib_id, sizeof(out->lib_id), "%s", lid);

    /* unit */
    const char *unit = sexpr_atom_value(node, "unit");
    out->unit = unit ? (unsigned int)atoi(unit) : 1;

    /* in_bom / on_board */
    const char *ibom = sexpr_atom_value(node, "in_bom");
    out->in_bom = ibom && strcmp(ibom, "yes") == 0;
    const char *ob = sexpr_atom_value(node, "on_board");
    out->on_board = ob && strcmp(ob, "yes") == 0;

    /* at: (at X Y angle) */
    sexpr_t *at = sexpr_get(node, "at");
    if (at && at->num_children >= 3) {
        out->at.x     = at->children[1]->value ? atof(at->children[1]->value) : 0;
        out->at.y     = at->children[2]->value ? atof(at->children[2]->value) : 0;
        out->at.angle = at->num_children >= 4 && at->children[3]->value
                        ? atof(at->children[3]->value) : 0;
    }

    /* properties from (property "Key" "Value") */
    const char *ref  = get_property_value(node, "Reference");
    const char *val  = get_property_value(node, "Value");
    const char *fp   = get_property_value(node, "Footprint");
    const char *ds   = get_property_value(node, "Datasheet");

    if (ref) snprintf(out->reference, sizeof(out->reference), "%s", ref);
    out->value      = val ? strdup(val) : strdup("");
    out->footprint  = fp  ? strdup(fp)  : strdup("");
    out->datasheet  = ds  ? strdup(ds)  : strdup("");

    /* collect all properties */
    size_t prop_cap = 8;
    out->properties = (kicli_property_t *)malloc(prop_cap * sizeof(kicli_property_t));
    out->num_properties = 0;

    for (size_t i = 0; i < node->num_children; i++) {
        sexpr_t *c = node->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 3) continue;
        sexpr_t *tag = c->children[0];
        if (!tag || !tag->value || strcmp(tag->value, "property") != 0) continue;

        if (out->num_properties >= prop_cap) {
            prop_cap *= 2;
            kicli_property_t *tmp = (kicli_property_t *)realloc(
                out->properties, prop_cap * sizeof(kicli_property_t));
            if (!tmp) break;
            out->properties = tmp;
        }

        kicli_property_t *p = &out->properties[out->num_properties++];
        memset(p, 0, sizeof(*p));
        if (c->children[1]->value)
            snprintf(p->key, sizeof(p->key), "%s", c->children[1]->value);
        p->value = c->children[2]->value ? strdup(c->children[2]->value) : strdup("");

        /* check (hidden yes) inside property */
        sexpr_t *effects = sexpr_get(c, "effects");
        if (effects) {
            sexpr_t *hide = sexpr_get(effects, "hide");
            p->hidden = (hide != NULL);
            /* also check (hide yes) as atom */
            if (!p->hidden) {
                const char *hv = sexpr_atom_value(effects, "hide");
                p->hidden = hv && strcmp(hv, "yes") == 0;
            }
        }
    }

    /* collect pins: (pin "num" (uuid "...")) */
    size_t pin_cap = 8;
    out->pins = (kicli_pin_ref_t *)malloc(pin_cap * sizeof(kicli_pin_ref_t));
    out->num_pins = 0;

    for (size_t i = 0; i < node->num_children; i++) {
        sexpr_t *c = node->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
        sexpr_t *tag = c->children[0];
        if (!tag || !tag->value || strcmp(tag->value, "pin") != 0) continue;

        if (out->num_pins >= pin_cap) {
            pin_cap *= 2;
            kicli_pin_ref_t *tmp = (kicli_pin_ref_t *)realloc(
                out->pins, pin_cap * sizeof(kicli_pin_ref_t));
            if (!tmp) break;
            out->pins = tmp;
        }

        kicli_pin_ref_t *pr = &out->pins[out->num_pins++];
        memset(pr, 0, sizeof(*pr));
        if (c->children[1]->value)
            snprintf(pr->number, sizeof(pr->number), "%s", c->children[1]->value);
    }

    return 0;
}

/* ── Parse title_block ────────────────────────────────────────────────────── */

static void parse_title_block(const sexpr_t *node, kicli_title_block_t *tb)
{
    const char *t  = sexpr_atom_value(node, "title");
    const char *d  = sexpr_atom_value(node, "date");
    const char *r  = sexpr_atom_value(node, "rev");
    const char *co = sexpr_atom_value(node, "company");

    if (t)  snprintf(tb->title,    sizeof(tb->title),    "%s", t);
    if (d)  snprintf(tb->date,     sizeof(tb->date),     "%s", d);
    if (r)  snprintf(tb->revision, sizeof(tb->revision), "%s", r);
    if (co) snprintf(tb->company,  sizeof(tb->company),  "%s", co);
}

/* ── Parse wire ───────────────────────────────────────────────────────────── */

static int parse_wire(const sexpr_t *node, kicli_wire_t *out)
{
    memset(out, 0, sizeof(*out));
    sexpr_t *pts = sexpr_get(node, "pts");
    if (!pts || pts->num_children < 3) return -1;

    /* (pts (xy X Y) (xy X Y)) */
    sexpr_t *xy0 = NULL, *xy1 = NULL;
    for (size_t i = 1; i < pts->num_children; i++) {
        sexpr_t *c = pts->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 3) continue;
        sexpr_t *tag = c->children[0];
        if (!tag || !tag->value || strcmp(tag->value, "xy") != 0) continue;
        if (!xy0) xy0 = c; else if (!xy1) xy1 = c;
    }

    if (xy0) {
        out->start.x = xy0->children[1]->value ? atof(xy0->children[1]->value) : 0;
        out->start.y = xy0->children[2]->value ? atof(xy0->children[2]->value) : 0;
    }
    if (xy1) {
        out->end.x = xy1->children[1]->value ? atof(xy1->children[1]->value) : 0;
        out->end.y = xy1->children[2]->value ? atof(xy1->children[2]->value) : 0;
    }
    return 0;
}

/* ── Parse junction ───────────────────────────────────────────────────────── */

static int parse_junction(const sexpr_t *node, kicli_junction_t *out)
{
    memset(out, 0, sizeof(*out));
    sexpr_t *at = sexpr_get(node, "at");
    if (at && at->num_children >= 3) {
        out->at.x = at->children[1]->value ? atof(at->children[1]->value) : 0;
        out->at.y = at->children[2]->value ? atof(at->children[2]->value) : 0;
    }
    const char *d = sexpr_atom_value(node, "diameter");
    out->diameter = d ? atof(d) : 0;
    return 0;
}

/* ── Parse label ──────────────────────────────────────────────────────────── */

static int parse_label(const sexpr_t *node, kicli_label_t *out)
{
    memset(out, 0, sizeof(*out));
    /* (label "net_name" (at X Y angle) ...) */
    if (node->num_children >= 2 && node->children[1]->value)
        snprintf(out->text, sizeof(out->text), "%s", node->children[1]->value);
    sexpr_t *at = sexpr_get(node, "at");
    if (at && at->num_children >= 3) {
        out->at.x     = at->children[1]->value ? atof(at->children[1]->value) : 0;
        out->at.y     = at->children[2]->value ? atof(at->children[2]->value) : 0;
        out->at.angle = at->num_children >= 4 && at->children[3]->value
                        ? atof(at->children[3]->value) : 0;
    }
    return 0;
}

/* ── Parse global_label ───────────────────────────────────────────────────── */

static int parse_global_label(const sexpr_t *node, kicli_global_label_t *out)
{
    memset(out, 0, sizeof(*out));
    if (node->num_children >= 2 && node->children[1]->value)
        snprintf(out->text, sizeof(out->text), "%s", node->children[1]->value);
    const char *shape = sexpr_atom_value(node, "shape");
    if (shape) snprintf(out->shape, sizeof(out->shape), "%s", shape);
    sexpr_t *at = sexpr_get(node, "at");
    if (at && at->num_children >= 3) {
        out->at.x     = at->children[1]->value ? atof(at->children[1]->value) : 0;
        out->at.y     = at->children[2]->value ? atof(at->children[2]->value) : 0;
        out->at.angle = at->num_children >= 4 && at->children[3]->value
                        ? atof(at->children[3]->value) : 0;
    }
    return 0;
}

/* ── Parse hierarchical label ─────────────────────────────────────────────── */

static int parse_hier_label(const sexpr_t *node, kicli_hier_label_t *out)
{
    memset(out, 0, sizeof(*out));
    /* (hierarchical_label "TEXT" (shape input) (at x y angle) ...) */
    if (node->num_children >= 2 && node->children[1]->value)
        snprintf(out->text, sizeof(out->text), "%s", node->children[1]->value);
    const char *shape = sexpr_atom_value(node, "shape");
    if (shape) snprintf(out->shape, sizeof(out->shape), "%s", shape);
    sexpr_t *at = sexpr_get(node, "at");
    if (at && at->num_children >= 3) {
        out->at.x = at->children[1]->value ? atof(at->children[1]->value) : 0;
        out->at.y = at->children[2]->value ? atof(at->children[2]->value) : 0;
    }
    return 0;
}

/* ── Parse sheet (with hierarchical pins) ────────────────────────────────── */

static int parse_sheet(const sexpr_t *node, kicli_sheet_t *out)
{
    memset(out, 0, sizeof(*out));

    /* (property "Sheetname" "POWER") */
    const char *name = get_property_value(node, "Sheetname");
    if (name) snprintf(out->sheetname, sizeof(out->sheetname), "%s", name);

    /* (property "Sheetfile" "power.kicad_sch") */
    const char *file = get_property_value(node, "Sheetfile");
    if (file) snprintf(out->sheetfile, sizeof(out->sheetfile), "%s", file);

    /* count pins */
    size_t pin_cap = 8;
    out->pins = (kicli_sheet_pin_t *)malloc(pin_cap * sizeof(kicli_sheet_pin_t));
    if (!out->pins) return -1;

    for (size_t i = 0; i < node->num_children; i++) {
        sexpr_t *c = node->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 3) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "pin") != 0) continue;

        /* (pin "CAN_TX" input ...) */
        if (out->num_pins >= pin_cap) {
            pin_cap *= 2;
            kicli_sheet_pin_t *tmp = realloc(out->pins, pin_cap * sizeof(kicli_sheet_pin_t));
            if (!tmp) return -1;
            out->pins = tmp;
        }
        kicli_sheet_pin_t *p = &out->pins[out->num_pins++];
        memset(p, 0, sizeof(*p));
        if (c->children[1]->value)
            snprintf(p->name, sizeof(p->name), "%s", c->children[1]->value);
        if (c->children[2]->value)
            snprintf(p->direction, sizeof(p->direction), "%s", c->children[2]->value);
    }
    return 0;
}

/* ── Dynamic array helpers ────────────────────────────────────────────────── */

#define GROW(arr, count, cap, type) do { \
    if ((count) >= (cap)) { \
        (cap) = (cap) ? (cap) * 2 : 8; \
        type *_tmp = (type *)realloc((arr), (cap) * sizeof(type)); \
        if (!_tmp) { kicli_set_error("out of memory"); goto oom; } \
        (arr) = _tmp; \
    } \
} while (0)

/* ── kicli_sch_read ───────────────────────────────────────────────────────── */

kicli_err_t kicli_sch_read(const char *path, kicli_schematic_t **out)
{
    if (!path || !out) return KICLI_ERR_INVALID_ARG;

    char *buf = read_file(path);
    if (!buf) return KICLI_ERR_IO;

    char errbuf[256] = {0};
    sexpr_t *root = sexpr_parse(buf, errbuf, sizeof(errbuf));
    free(buf);

    if (!root) {
        kicli_set_error("parse error: %s", errbuf[0] ? errbuf : "unknown");
        return KICLI_ERR_PARSE;
    }

    /* root must be (kicad_sch ...) */
    if (root->type != SEXPR_LIST || root->num_children == 0 ||
        !root->children[0]->value ||
        strcmp(root->children[0]->value, "kicad_sch") != 0) {
        sexpr_free(root);
        kicli_set_error("not a valid .kicad_sch file");
        return KICLI_ERR_PARSE;
    }

    kicli_schematic_t *sch = (kicli_schematic_t *)calloc(1, sizeof(kicli_schematic_t));
    if (!sch) { sexpr_free(root); return KICLI_ERR_OOM; }

    /* version */
    const char *ver = sexpr_atom_value(root, "version");
    sch->version = ver ? (unsigned int)atoi(ver) : 0;

    /* generator */
    const char *gen = sexpr_atom_value(root, "generator");
    if (gen) snprintf(sch->generator, sizeof(sch->generator), "%s", gen);

    /* title_block */
    sexpr_t *tb = sexpr_get(root, "title_block");
    if (tb) {
        sch->has_title_block = true;
        parse_title_block(tb, &sch->title_block);
    }

    /* dynamic arrays */
    size_t sym_cap = 16, wire_cap = 16, junc_cap = 8;
    size_t lbl_cap = 8, glbl_cap = 8, hlbl_cap = 8, sht_cap = 8;

    sch->symbols         = (kicli_symbol_t   *)malloc(sym_cap  * sizeof(kicli_symbol_t));
    sch->wires           = (kicli_wire_t     *)malloc(wire_cap * sizeof(kicli_wire_t));
    sch->junctions       = (kicli_junction_t *)malloc(junc_cap * sizeof(kicli_junction_t));
    sch->labels          = (kicli_label_t    *)malloc(lbl_cap  * sizeof(kicli_label_t));
    sch->global_labels   = (kicli_global_label_t *)malloc(glbl_cap * sizeof(kicli_global_label_t));
    sch->hier_labels     = (kicli_hier_label_t *)malloc(hlbl_cap * sizeof(kicli_hier_label_t));
    sch->sheets          = (kicli_sheet_t *)malloc(sht_cap * sizeof(kicli_sheet_t));

    if (!sch->symbols || !sch->wires || !sch->junctions ||
        !sch->labels  || !sch->global_labels || !sch->hier_labels || !sch->sheets)
        goto oom;

    /* Walk root children */
    for (size_t i = 1; i < root->num_children; i++) {
        sexpr_t *c = root->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children == 0) continue;
        const char *tag = c->children[0]->value;
        if (!tag) continue;

        if (strcmp(tag, "symbol") == 0) {
            GROW(sch->symbols, sch->num_symbols, sym_cap, kicli_symbol_t);
            parse_symbol(c, &sch->symbols[sch->num_symbols++]);
        } else if (strcmp(tag, "wire") == 0) {
            GROW(sch->wires, sch->num_wires, wire_cap, kicli_wire_t);
            parse_wire(c, &sch->wires[sch->num_wires++]);
        } else if (strcmp(tag, "junction") == 0) {
            GROW(sch->junctions, sch->num_junctions, junc_cap, kicli_junction_t);
            parse_junction(c, &sch->junctions[sch->num_junctions++]);
        } else if (strcmp(tag, "label") == 0) {
            GROW(sch->labels, sch->num_labels, lbl_cap, kicli_label_t);
            parse_label(c, &sch->labels[sch->num_labels++]);
        } else if (strcmp(tag, "global_label") == 0) {
            GROW(sch->global_labels, sch->num_global_labels, glbl_cap, kicli_global_label_t);
            parse_global_label(c, &sch->global_labels[sch->num_global_labels++]);
        } else if (strcmp(tag, "hierarchical_label") == 0) {
            GROW(sch->hier_labels, sch->num_hier_labels, hlbl_cap, kicli_hier_label_t);
            parse_hier_label(c, &sch->hier_labels[sch->num_hier_labels++]);
        } else if (strcmp(tag, "sheet") == 0) {
            GROW(sch->sheets, sch->num_sheets, sht_cap, kicli_sheet_t);
            parse_sheet(c, &sch->sheets[sch->num_sheets++]);
        }
    }

    sexpr_free(root);
    *out = sch;
    return KICLI_OK;

oom:
    sexpr_free(root);
    kicli_sch_free(sch);
    return KICLI_ERR_OOM;
}

/* ── kicli_sch_free ───────────────────────────────────────────────────────── */

void kicli_sch_free(kicli_schematic_t *sch)
{
    if (!sch) return;

    for (size_t i = 0; i < sch->num_symbols; i++) {
        kicli_symbol_t *s = &sch->symbols[i];
        free(s->value);
        free(s->footprint);
        free(s->datasheet);
        for (size_t j = 0; j < s->num_properties; j++)
            free(s->properties[j].value);
        free(s->properties);
        free(s->pins);
    }
    free(sch->symbols);
    for (size_t i = 0; i < sch->num_power_symbols; i++) {
        kicli_symbol_t *s = &sch->power_symbols[i];
        free(s->value);
        free(s->footprint);
        free(s->datasheet);
        for (size_t j = 0; j < s->num_properties; j++)
            free(s->properties[j].value);
        free(s->properties);
        free(s->pins);
    }
    free(sch->power_symbols);
    free(sch->lib_symbols);
    free(sch->wires);
    free(sch->junctions);
    free(sch->labels);
    free(sch->global_labels);
    free(sch->hier_labels);
    for (size_t i = 0; i < sch->num_sheets; i++)
        free(sch->sheets[i].pins);
    free(sch->sheets);
    free(sch);
}

/* ── Lookup helpers ───────────────────────────────────────────────────────── */

kicli_symbol_t *kicli_sch_symbol_by_ref(kicli_schematic_t *sch, const char *ref)
{
    if (!sch || !ref) return NULL;
    for (size_t i = 0; i < sch->num_symbols; i++) {
        if (strcmp(sch->symbols[i].reference, ref) == 0)
            return &sch->symbols[i];
    }
    return NULL;
}

kicli_lib_symbol_t *kicli_sch_find_lib_symbol(kicli_schematic_t *sch, const char *lib_id)
{
    if (!sch || !lib_id) return NULL;
    for (size_t i = 0; i < sch->num_lib_symbols; i++) {
        if (strcmp(sch->lib_symbols[i].name, lib_id) == 0)
            return &sch->lib_symbols[i];
    }
    return NULL;
}

bool kicli_sch_pin_position(const kicli_schematic_t *sch, const char *ref,
                             const char *pin_number, kicli_pt_t *out)
{
    (void)sch; (void)ref; (void)pin_number; (void)out;
    return false;
}

/* ── Print helpers ────────────────────────────────────────────────────────── */

void kicli_sch_print_tree(const kicli_schematic_t *sch)
{
    if (!sch) return;
    printf("schematic (v%u)\n", sch->version);
    if (sch->has_title_block && sch->title_block.title[0])
        printf("  title: %s\n", sch->title_block.title);
    printf("  symbols:   %zu\n", sch->num_symbols);
    printf("  wires:     %zu\n", sch->num_wires);
    printf("  labels:    %zu\n", sch->num_labels);
    printf("  globals:   %zu\n", sch->num_global_labels);
}

/* ── Stub: write back ─────────────────────────────────────────────────────── */

kicli_err_t kicli_sch_write(const kicli_schematic_t *sch, const char *path, bool backup)
{
    (void)sch; (void)path; (void)backup;
    kicli_set_error("sch write not yet implemented");
    return KICLI_ERR_NOT_IMPLEMENTED;
}

/* ── Stubs: ops ───────────────────────────────────────────────────────────── */

kicli_err_t kicli_sch_add_symbol(kicli_schematic_t *s, const char *lib_id,
                                  const char *ref, const char *value, kicli_pos_t at)
{ (void)s;(void)lib_id;(void)ref;(void)value;(void)at;
  kicli_set_error("not yet implemented"); return KICLI_ERR_NOT_IMPLEMENTED; }

kicli_err_t kicli_sch_remove_symbol(kicli_schematic_t *s, const char *ref)
{ (void)s;(void)ref;
  kicli_set_error("not yet implemented"); return KICLI_ERR_NOT_IMPLEMENTED; }

kicli_err_t kicli_sch_move_symbol(kicli_schematic_t *s, const char *ref, double x, double y)
{ (void)s;(void)ref;(void)x;(void)y;
  kicli_set_error("not yet implemented"); return KICLI_ERR_NOT_IMPLEMENTED; }

kicli_err_t kicli_sch_connect(kicli_schematic_t *s, const char *a, const char *b)
{ (void)s;(void)a;(void)b;
  kicli_set_error("not yet implemented"); return KICLI_ERR_NOT_IMPLEMENTED; }

kicli_err_t kicli_sch_disconnect(kicli_schematic_t *s, const char *a, const char *b)
{ (void)s;(void)a;(void)b;
  kicli_set_error("not yet implemented"); return KICLI_ERR_NOT_IMPLEMENTED; }

kicli_err_t kicli_sch_rename(kicli_schematic_t *s, const char *old, const char *newref)
{ (void)s;(void)old;(void)newref;
  kicli_set_error("not yet implemented"); return KICLI_ERR_NOT_IMPLEMENTED; }

kicli_err_t kicli_sch_set_field(kicli_schematic_t *s, const char *ref,
                                  const char *field, const char *value)
{ (void)s;(void)ref;(void)field;(void)value;
  kicli_set_error("not yet implemented"); return KICLI_ERR_NOT_IMPLEMENTED; }

/* ── Stubs: diff / validate / export ──────────────────────────────────────── */

kicli_sch_diff_t *kicli_sch_diff(const kicli_schematic_t *a, const kicli_schematic_t *b)
{ (void)a;(void)b; return NULL; }

void kicli_sch_diff_print(const kicli_sch_diff_t *d) { (void)d; }
void kicli_sch_diff_free(kicli_sch_diff_t *d) { free(d); }

kicli_validation_t *kicli_sch_validate(const kicli_schematic_t *sch)
{ (void)sch; return NULL; }

void kicli_validation_print(const kicli_validation_t *v) { (void)v; }
void kicli_validation_free(kicli_validation_t *v) { free(v); }

kicli_err_t kicli_sch_export(const char *sch_path, const char *format,
                               const char *output_path)
{ (void)sch_path;(void)format;(void)output_path;
  kicli_set_error("use 'kicli sch <file> export <fmt>' to delegate to kicad-cli");
  return KICLI_ERR_NOT_IMPLEMENTED; }
