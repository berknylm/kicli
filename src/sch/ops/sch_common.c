/*
 * sch_common.c — shared helpers for src/sch/ops files
 *
 * Everything in here was previously static inside draw.c. Splitting it out
 * lets the place / net / nc / sheet / check commands live in their own
 * focused files without duplicating the s-expression plumbing.
 */

#include "kicli/sch_ops.h"
#include "kicli/error.h"
#include "kicli/portable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>

/* ── Tiny sexpr atom builders ──────────────────────────────────────────── */

sexpr_t *make_atom(const char *v) { return sexpr_make_atom(v); }
sexpr_t *make_str (const char *v) { return sexpr_make_str (v); }

/* Double → compact string "123.45" with up to 6 fractional digits, no
 * trailing zeros. KiCad itself uses the same cleanup on save. */
void fmt_num(char *out, size_t sz, double v)
{
    snprintf(out, sz, "%.6f", v);
    char *dot = strchr(out, '.');
    if (!dot) return;
    char *end = out + strlen(out) - 1;
    while (end > dot && *end == '0') *end-- = '\0';
    if (end == dot) *end = '\0';
}

sexpr_t *mk_at(double x, double y, double a)
{
    sexpr_t *l = sexpr_make_list();
    char sx[32], sy[32], sa[32];
    fmt_num(sx, sizeof(sx), x);
    fmt_num(sy, sizeof(sy), y);
    fmt_num(sa, sizeof(sa), a);
    sexpr_list_append(l, make_atom("at"));
    sexpr_list_append(l, make_atom(sx));
    sexpr_list_append(l, make_atom(sy));
    sexpr_list_append(l, make_atom(sa));
    return l;
}

sexpr_t *mk_named_atom(const char *tag, const char *val)
{
    sexpr_t *l = sexpr_make_list();
    sexpr_list_append(l, make_atom(tag));
    sexpr_list_append(l, make_atom(val));
    return l;
}

sexpr_t *mk_named_str(const char *tag, const char *val)
{
    sexpr_t *l = sexpr_make_list();
    sexpr_list_append(l, make_atom(tag));
    sexpr_list_append(l, make_str(val));
    return l;
}

double snap_grid(double v, double step)
{
    if (step <= 0) return v;
    return round(v / step) * step;
}

/* ── Schematic traversal ───────────────────────────────────────────────── */

const char *get_root_uuid(const sexpr_t *root)
{
    return sexpr_atom_value(root, "uuid");
}

void project_name_for(const char *sch_path, char *out, size_t sz)
{
    const char *slash = strrchr(sch_path, '/');
#ifdef _WIN32
    const char *bs = strrchr(sch_path, '\\');
    if (!slash || (bs && bs > slash)) slash = bs;
#endif
    const char *base = slash ? slash + 1 : sch_path;
    snprintf(out, sz, "%s", base);
    char *dot = strstr(out, ".kicad_sch");
    if (dot) *dot = '\0';
}

sexpr_t *get_or_create_lib_symbols(sexpr_t *root)
{
    sexpr_t *ls = sexpr_get(root, "lib_symbols");
    if (ls) return ls;
    ls = sexpr_make_list();
    sexpr_list_append(ls, make_atom("lib_symbols"));
    sexpr_list_append(root, ls);
    return ls;
}

int lib_symbols_has(const sexpr_t *ls, const char *lib_id)
{
    for (size_t i = 1; i < ls->num_children; i++) {
        sexpr_t *c = ls->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "symbol") != 0) continue;
        if (c->children[1]->value && strcmp(c->children[1]->value, lib_id) == 0) return 1;
    }
    return 0;
}

int ensure_lib_symbol_in_sheet(sexpr_t *root, const char *lib_id)
{
    sexpr_t *ls = get_or_create_lib_symbols(root);
    if (!ls) return -2;
    if (lib_symbols_has(ls, lib_id)) return 0;

    sexpr_t *src = kicli_sym_lib_find(lib_id);
    if (!src) return -1;

    sexpr_t *copy = sexpr_clone(src);
    if (!copy) return -2;

    /* The cached library stores (symbol "LOCAL_NAME" ...) but the sheet
     * wants (symbol "Library:LOCAL_NAME" ...). Rewrite children[1]. */
    if (copy->type == SEXPR_LIST && copy->num_children >= 2 &&
        copy->children[1]) {
        free(copy->children[1]->value);
        copy->children[1]->value = strdup(lib_id);
        copy->children[1]->type  = SEXPR_STR;
    }
    sexpr_list_append(ls, copy);

    /* Resolve (extends "Base") aliases (e.g. LM358 → LM2904). */
    for (size_t i = 1; i < copy->num_children; i++) {
        sexpr_t *n = copy->children[i];
        if (!n || n->type != SEXPR_LIST || n->num_children < 2) continue;
        if (!n->children[0]->value
            || strcmp(n->children[0]->value, "extends") != 0) continue;
        const char *base_name = n->children[1]->value;
        if (!base_name) break;
        const char *colon = strchr(lib_id, ':');
        if (!colon) break;
        char base_lib_id[256];
        snprintf(base_lib_id, sizeof(base_lib_id), "%.*s:%s",
                 (int)(colon - lib_id), lib_id, base_name);
        if (!lib_symbols_has(ls, base_lib_id))
            (void)ensure_lib_symbol_in_sheet(root, base_lib_id);
        break;
    }
    return 0;
}

sexpr_t *find_lib_symbol_node(const sexpr_t *root, const char *lib_id)
{
    const sexpr_t *ls = sexpr_get(root, "lib_symbols");
    if (!ls) return NULL;
    for (size_t i = 1; i < ls->num_children; i++) {
        sexpr_t *c = ls->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "symbol") != 0) continue;
        if (c->children[1]->value && strcmp(c->children[1]->value, lib_id) == 0) return c;
    }
    return NULL;
}

size_t count_placed_symbols(const sexpr_t *root)
{
    size_t n = 0;
    for (size_t i = 0; i < root->num_children; i++) {
        sexpr_t *c = root->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 1) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "symbol") != 0) continue;
        n++;
    }
    return n;
}

void auto_slot(size_t idx, double *x, double *y)
{
    size_t col = idx % 10;
    size_t row = idx / 10;
    *x = 50.0 + col * 25.0;
    *y = 50.0 + row * 20.0;
}

sexpr_t *find_placed_by_ref(const sexpr_t *root, const char *ref)
{
    for (size_t i = 0; i < root->num_children; i++) {
        sexpr_t *c = root->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "symbol") != 0) continue;
        for (size_t j = 1; j < c->num_children; j++) {
            sexpr_t *p = c->children[j];
            if (!p || p->type != SEXPR_LIST || p->num_children < 3) continue;
            if (!p->children[0]->value || strcmp(p->children[0]->value, "property") != 0) continue;
            if (p->children[1]->value && strcmp(p->children[1]->value, "Reference") == 0 &&
                p->children[2]->value && strcmp(p->children[2]->value, ref) == 0)
                return c;
        }
    }
    return NULL;
}

const char *placed_lib_id(const sexpr_t *sym)
{
    for (size_t i = 1; i < sym->num_children; i++) {
        sexpr_t *c = sym->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "lib_id") != 0) continue;
        return c->children[1]->value;
    }
    return NULL;
}

int placed_at(const sexpr_t *sym, double *x, double *y, double *a)
{
    *x = *y = *a = 0;
    for (size_t i = 1; i < sym->num_children; i++) {
        sexpr_t *c = sym->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 3) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "at") != 0) continue;
        if (c->children[1]->value) *x = atof(c->children[1]->value);
        if (c->children[2]->value) *y = atof(c->children[2]->value);
        if (c->num_children > 3 && c->children[3]->value) *a = atof(c->children[3]->value);
        return 0;
    }
    return -1;
}

int placed_mirror_x(const sexpr_t *sym)
{
    for (size_t i = 1; i < sym->num_children; i++) {
        sexpr_t *c = sym->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "mirror") != 0) continue;
        if (c->children[1]->value && strcmp(c->children[1]->value, "x") == 0) return 1;
    }
    return 0;
}

const char *placed_property(const sexpr_t *sym, const char *field)
{
    for (size_t i = 1; i < sym->num_children; i++) {
        const sexpr_t *p = sym->children[i];
        if (!p || p->type != SEXPR_LIST || p->num_children < 3) continue;
        if (!p->children[0]->value || strcmp(p->children[0]->value, "property") != 0) continue;
        if (p->children[1]->value && strcmp(p->children[1]->value, field) == 0)
            return p->children[2]->value;
    }
    return NULL;
}

const char *placed_uuid(const sexpr_t *sym)
{
    for (size_t i = 1; i < sym->num_children; i++) {
        const sexpr_t *c = sym->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "uuid") != 0) continue;
        return c->children[1]->value;
    }
    return NULL;
}

/* Walk a library-symbol definition (incl. sub-units) and find the pin whose
 * (number "N") matches `num`. Writes its (at X Y ANGLE) into outs. Local
 * helper — used only by world_pin_pos. */
static int find_lib_pin_recursive(const sexpr_t *node, const char *num,
                                   double *x, double *y, double *a)
{
    if (!node || node->type != SEXPR_LIST || node->num_children == 0) return -1;
    const char *tag = node->children[0]->value;
    if (!tag) return -1;

    if (strcmp(tag, "pin") == 0) {
        const char *pin_num = NULL;
        double px = 0, py = 0, pa = 0;
        int got_at = 0;
        for (size_t i = 1; i < node->num_children; i++) {
            sexpr_t *c = node->children[i];
            if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
            const char *t = c->children[0]->value;
            if (!t) continue;
            if (strcmp(t, "at") == 0 && c->num_children >= 3) {
                px = c->children[1]->value ? atof(c->children[1]->value) : 0;
                py = c->children[2]->value ? atof(c->children[2]->value) : 0;
                pa = (c->num_children > 3 && c->children[3]->value)
                     ? atof(c->children[3]->value) : 0;
                got_at = 1;
            } else if (strcmp(t, "number") == 0 && c->children[1]->value) {
                pin_num = c->children[1]->value;
            }
        }
        if (got_at && pin_num && strcmp(pin_num, num) == 0) {
            *x = px; *y = py; *a = pa;
            return 0;
        }
        return -1;
    }

    for (size_t i = 1; i < node->num_children; i++)
        if (find_lib_pin_recursive(node->children[i], num, x, y, a) == 0)
            return 0;
    return -1;
}

int world_pin_pos(sexpr_t *root, const char *ref, const char *pin_num,
                  double *wx, double *wy, double *wangle)
{
    sexpr_t *sym = find_placed_by_ref(root, ref);
    if (!sym) {
        kicli_set_error("placed symbol '%s' not found", ref);
        return -1;
    }
    const char *lib_id = placed_lib_id(sym);
    if (!lib_id) {
        kicli_set_error("placed symbol '%s' has no lib_id", ref);
        return -1;
    }
    sexpr_t *lib_def = find_lib_symbol_node(root, lib_id);
    if (!lib_def) {
        kicli_set_error("lib_symbols is missing '%s' — did you `place` it?", lib_id);
        return -1;
    }

    double sym_x, sym_y, sym_a;
    placed_at(sym, &sym_x, &sym_y, &sym_a);
    int mirror_x = placed_mirror_x(sym);

    double lx, ly, langle;
    int found = (find_lib_pin_recursive(lib_def, pin_num, &lx, &ly, &langle) == 0);

    /* Aliases like LM358 store their pins on the (extends "LM2904") base. */
    if (!found) {
        const char *base_name = NULL;
        for (size_t i = 1; i < lib_def->num_children; i++) {
            sexpr_t *c = lib_def->children[i];
            if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
            if (!c->children[0]->value
                || strcmp(c->children[0]->value, "extends") != 0) continue;
            base_name = c->children[1]->value;
            break;
        }
        if (base_name) {
            const char *colon = strchr(lib_id, ':');
            if (colon) {
                char base_lib_id[256];
                snprintf(base_lib_id, sizeof(base_lib_id), "%.*s:%s",
                         (int)(colon - lib_id), lib_id, base_name);
                sexpr_t *base = find_lib_symbol_node(root, base_lib_id);
                if (base && find_lib_pin_recursive(base, pin_num, &lx, &ly, &langle) == 0)
                    found = 1;
            }
        }
    }

    if (!found) {
        kicli_set_error("pin '%s' not on symbol '%s' (lib %s)",
                        pin_num, ref, lib_id);
        return -1;
    }

    /* dump.c convention (KiCad schematic y-down): */
    double rad = sym_a * (3.14159265358979323846 / 180.0);
    double ca = cos(rad), sa = sin(rad);
    if (mirror_x) lx = -lx;
    *wx = sym_x + lx * ca - ly * sa;
    *wy = sym_y - lx * sa - ly * ca;

    double out = langle - sym_a;
    if (mirror_x) out = 180.0 - out;
    while (out < 0)    out += 360.0;
    while (out >= 360) out -= 360.0;
    *wangle = out;
    return 0;
}

/* ── Power-rail detection ──────────────────────────────────────────────── */

static int str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

int detect_power(const char *net_name, char *canon_out, size_t sz)
{
    if (!net_name || !*net_name) return 0;

    const char *exact[] = { "GND", "VCC", "VDD", "VSS", "VEE",
                            "GNDA", "GNDD", "PGND", "AGND",
                            "+BATT", "-BATT", "VBATT",
                            "EARTH", NULL };
    for (int i = 0; exact[i]; i++) {
        if (str_ieq(net_name, exact[i])) {
            snprintf(canon_out, sz, "%s", exact[i]);
            return 1;
        }
    }

    /* Voltage-looking patterns: optional sign, digits, optional 'V'. */
    const char *s = net_name;
    char sign = 0;
    if (*s == '+' || *s == '-') { sign = *s; s++; }
    const char *num_start = s;
    int seen_digit = 0, seen_v = 0;
    while (*s) {
        if (isdigit((unsigned char)*s)) { seen_digit = 1; s++; }
        else if (*s == '.')              { s++; }
        else if (*s == 'V' || *s == 'v') { seen_v = 1; s++; }
        else return 0;
    }
    if (!seen_digit || !seen_v) return 0;

    if (sign == 0) snprintf(canon_out, sz, "+%s", num_start);
    else           snprintf(canon_out, sz, "%c%s", sign, num_start);
    return 1;
}

/* ── Property / effects / uuid / placed-symbol builders ────────────────── */

sexpr_t *mk_effects_default(void)
{
    sexpr_t *size = sexpr_make_list();
    sexpr_list_append(size, make_atom("size"));
    sexpr_list_append(size, make_atom("1.27"));
    sexpr_list_append(size, make_atom("1.27"));

    sexpr_t *font = sexpr_make_list();
    sexpr_list_append(font, make_atom("font"));
    sexpr_list_append(font, size);

    sexpr_t *effects = sexpr_make_list();
    sexpr_list_append(effects, make_atom("effects"));
    sexpr_list_append(effects, font);
    return effects;
}

sexpr_t *mk_uuid_node(void)
{
    char u[40];
    kicli_uuid4(u, sizeof(u));
    sexpr_t *n = sexpr_make_list();
    sexpr_list_append(n, make_atom("uuid"));
    sexpr_list_append(n, make_str(u));
    return n;
}

sexpr_t *mk_property(const char *key, const char *value,
                     double x, double y, double angle, int hide)
{
    sexpr_t *prop = sexpr_make_list();
    sexpr_list_append(prop, make_atom("property"));
    sexpr_list_append(prop, make_str(key));
    sexpr_list_append(prop, make_str(value ? value : ""));
    sexpr_list_append(prop, mk_at(x, y, angle));
    sexpr_t *effects = mk_effects_default();
    if (hide) {
        sexpr_t *h = sexpr_make_list();
        sexpr_list_append(h, make_atom("hide"));
        sexpr_list_append(h, make_atom("yes"));
        sexpr_list_append(effects, h);
    }
    sexpr_list_append(prop, effects);
    return prop;
}

const char *lib_default_property(const sexpr_t *lib_def, const sexpr_t *root,
                                 const char *lib_id, const char *key)
{
    if (!lib_def) return NULL;
    for (size_t i = 1; i < lib_def->num_children; i++) {
        sexpr_t *c = lib_def->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 3) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "property") != 0) continue;
        if (c->children[1]->value && strcmp(c->children[1]->value, key) == 0)
            return c->children[2]->value;
    }
    /* Try extends base — common for aliases (LM358 → LM2904). */
    for (size_t i = 1; i < lib_def->num_children; i++) {
        sexpr_t *c = lib_def->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "extends") != 0) continue;
        const char *base = c->children[1]->value;
        if (!base) break;
        const char *colon = strchr(lib_id, ':');
        if (!colon) break;
        char base_id[256];
        snprintf(base_id, sizeof(base_id), "%.*s:%s", (int)(colon - lib_id), lib_id, base);
        sexpr_t *base_def = find_lib_symbol_node(root, base_id);
        if (base_def) return lib_default_property(base_def, root, base_id, key);
        break;
    }
    return NULL;
}

sexpr_t *mk_placed_symbol(const char *lib_id, const char *ref, const char *value,
                          double x, double y, double angle,
                          const sexpr_t *lib_def, const sexpr_t *root,
                          const char *root_uuid, const char *project_name)
{
    sexpr_t *sym = sexpr_make_list();
    sexpr_list_append(sym, make_atom("symbol"));
    sexpr_list_append(sym, mk_named_str("lib_id", lib_id));
    sexpr_list_append(sym, mk_at(x, y, angle));
    sexpr_list_append(sym, mk_named_atom("unit", "1"));
    sexpr_list_append(sym, mk_named_atom("exclude_from_sim", "no"));
    sexpr_list_append(sym, mk_named_atom("in_bom",   "yes"));
    sexpr_list_append(sym, mk_named_atom("on_board", "yes"));
    sexpr_list_append(sym, mk_named_atom("dnp",      "no"));
    sexpr_list_append(sym, mk_uuid_node());

    /* Inherit defaults from the library symbol. `--footprint` overrides
     * by patching the property after the builder returns. */
    const char *def_fp   = lib_default_property(lib_def, root, lib_id, "Footprint");
    const char *def_ds   = lib_default_property(lib_def, root, lib_id, "Datasheet");
    const char *def_desc = lib_default_property(lib_def, root, lib_id, "Description");

    sexpr_list_append(sym, mk_property("Reference", ref,   x + 2.54, y - 2.54, 0, 0));
    sexpr_list_append(sym, mk_property("Value",     value, x + 2.54, y + 2.54, 0, 0));
    sexpr_list_append(sym, mk_property("Footprint", def_fp ? def_fp : "",  x, y, 0, 1));
    sexpr_list_append(sym, mk_property("Datasheet", def_ds ? def_ds : "~", x, y, 0, 1));
    sexpr_list_append(sym, mk_property("Description", def_desc ? def_desc : "", x, y, 0, 1));

    /* (pin "N" (uuid "...")) per pin in the lib def. */
    for (size_t i = 1; lib_def && i < lib_def->num_children; i++) {
        sexpr_t *c = lib_def->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children == 0) continue;
        if (!c->children[0]->value) continue;
        if (strcmp(c->children[0]->value, "symbol") != 0) continue;
        for (size_t j = 1; j < c->num_children; j++) {
            sexpr_t *p = c->children[j];
            if (!p || p->type != SEXPR_LIST || p->num_children == 0) continue;
            if (!p->children[0]->value
                || strcmp(p->children[0]->value, "pin") != 0) continue;
            const char *num = NULL;
            for (size_t k = 1; k < p->num_children; k++) {
                sexpr_t *x1 = p->children[k];
                if (!x1 || x1->type != SEXPR_LIST || x1->num_children < 2) continue;
                if (!x1->children[0]->value
                    || strcmp(x1->children[0]->value, "number") != 0) continue;
                num = x1->children[1]->value;
                break;
            }
            if (!num) continue;
            sexpr_t *pin = sexpr_make_list();
            sexpr_list_append(pin, make_atom("pin"));
            sexpr_list_append(pin, make_str(num));
            sexpr_list_append(pin, mk_uuid_node());
            sexpr_list_append(sym, pin);
        }
    }

    /* (instances (project "<proj>" (path "/<root_uuid>" (reference "<REF>") (unit 1)))) */
    char path_val[128];
    snprintf(path_val, sizeof(path_val), "/%s", root_uuid ? root_uuid : "");
    sexpr_t *path = sexpr_make_list();
    sexpr_list_append(path, make_atom("path"));
    sexpr_list_append(path, make_str(path_val));
    sexpr_list_append(path, mk_named_str("reference", ref));
    sexpr_list_append(path, mk_named_atom("unit", "1"));

    sexpr_t *proj = sexpr_make_list();
    sexpr_list_append(proj, make_atom("project"));
    sexpr_list_append(proj, make_str(project_name ? project_name : ""));
    sexpr_list_append(proj, path);

    sexpr_t *inst = sexpr_make_list();
    sexpr_list_append(inst, make_atom("instances"));
    sexpr_list_append(inst, proj);

    sexpr_list_append(sym, inst);
    return sym;
}

sexpr_t *mk_label(const char *text, double x, double y, double angle)
{
    sexpr_t *lbl = sexpr_make_list();
    sexpr_list_append(lbl, make_atom("label"));
    sexpr_list_append(lbl, make_str(text));
    sexpr_list_append(lbl, mk_at(x, y, angle));
    sexpr_list_append(lbl, mk_effects_default());
    sexpr_list_append(lbl, mk_uuid_node());
    return lbl;
}

sexpr_t *mk_global_label(const char *text, const char *shape,
                         double x, double y, double angle)
{
    sexpr_t *lbl = sexpr_make_list();
    sexpr_list_append(lbl, make_atom("global_label"));
    sexpr_list_append(lbl, make_str(text));
    sexpr_list_append(lbl, mk_named_atom("shape", shape ? shape : "passive"));
    sexpr_list_append(lbl, mk_at(x, y, angle));
    sexpr_list_append(lbl, mk_effects_default());
    sexpr_list_append(lbl, mk_uuid_node());
    return lbl;
}

sexpr_t *mk_hier_label(const char *text, const char *shape,
                       double x, double y, double angle)
{
    sexpr_t *lbl = sexpr_make_list();
    sexpr_list_append(lbl, make_atom("hierarchical_label"));
    sexpr_list_append(lbl, make_str(text));
    sexpr_list_append(lbl, mk_named_atom("shape", shape ? shape : "passive"));
    sexpr_list_append(lbl, mk_at(x, y, angle));
    sexpr_list_append(lbl, mk_effects_default());
    sexpr_list_append(lbl, mk_uuid_node());
    return lbl;
}

sexpr_t *mk_no_connect(double x, double y)
{
    /* no_connect uses a 2-D (at X Y) without an angle. KiCad refuses
     * the 3-arg form. */
    sexpr_t *at = sexpr_make_list();
    char sx[32], sy[32];
    fmt_num(sx, sizeof(sx), x);
    fmt_num(sy, sizeof(sy), y);
    sexpr_list_append(at, make_atom("at"));
    sexpr_list_append(at, make_atom(sx));
    sexpr_list_append(at, make_atom(sy));

    sexpr_t *nc = sexpr_make_list();
    sexpr_list_append(nc, make_atom("no_connect"));
    sexpr_list_append(nc, at);
    sexpr_list_append(nc, mk_uuid_node());
    return nc;
}

/* ── Power-port emission + #PWR auto-counter ───────────────────────────── */

/* Module-static counter; seeded from existing schematic refs each
 * cmd_sch_net invocation so we never collide with prior power ports. */
static int g_pwr_counter = 0;

void seed_pwr_counter(const sexpr_t *root)
{
    if (!root) return;
    int max = 0;
    for (size_t i = 0; i < root->num_children; i++) {
        const sexpr_t *c = root->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children == 0) continue;
        const sexpr_t *head = c->children[0];
        if (!head || head->type != SEXPR_ATOM || strcmp(head->value, "symbol") != 0) continue;
        for (size_t j = 1; j < c->num_children; j++) {
            const sexpr_t *p = c->children[j];
            if (!p || p->type != SEXPR_LIST || p->num_children < 3) continue;
            const sexpr_t *ph = p->children[0];
            const sexpr_t *pn = p->children[1];
            const sexpr_t *pv = p->children[2];
            if (!ph || ph->type != SEXPR_ATOM || strcmp(ph->value, "property") != 0) continue;
            if (!pn || !pn->value || strcmp(pn->value, "Reference") != 0) continue;
            if (!pv || !pv->value) continue;
            if (strncmp(pv->value, "#PWR", 4) == 0) {
                int n = atoi(pv->value + 4);
                if (n > max) max = n;
            }
        }
    }
    if (max > g_pwr_counter) g_pwr_counter = max;
}

sexpr_t *mk_power_port(const char *rail, double x, double y, double angle,
                       const char *root_uuid, const char *project_name)
{
    char lib_id[96];
    snprintf(lib_id, sizeof(lib_id), "power:%s", rail);

    sexpr_t *sym = sexpr_make_list();
    sexpr_list_append(sym, make_atom("symbol"));
    sexpr_list_append(sym, mk_named_str("lib_id", lib_id));
    sexpr_list_append(sym, mk_at(x, y, angle));
    sexpr_list_append(sym, mk_named_atom("unit", "1"));
    sexpr_list_append(sym, mk_named_atom("exclude_from_sim", "no"));
    sexpr_list_append(sym, mk_named_atom("in_bom",   "no"));
    sexpr_list_append(sym, mk_named_atom("on_board", "yes"));
    sexpr_list_append(sym, mk_named_atom("dnp",      "no"));
    sexpr_list_append(sym, mk_uuid_node());

    char ref[32];
    snprintf(ref, sizeof(ref), "#PWR%04d", ++g_pwr_counter);
    sexpr_list_append(sym, mk_property("Reference", ref,  x, y - 3.81, 0, 1));
    sexpr_list_append(sym, mk_property("Value",     rail, x, y - 1.27, 0, 0));
    sexpr_list_append(sym, mk_property("Footprint", "",   x, y,        0, 1));
    sexpr_list_append(sym, mk_property("Datasheet", "~",  x, y,        0, 1));

    sexpr_t *pin = sexpr_make_list();
    sexpr_list_append(pin, make_atom("pin"));
    sexpr_list_append(pin, make_str("1"));
    sexpr_list_append(pin, mk_uuid_node());
    sexpr_list_append(sym, pin);

    char path_val[128];
    snprintf(path_val, sizeof(path_val), "/%s", root_uuid ? root_uuid : "");
    sexpr_t *path = sexpr_make_list();
    sexpr_list_append(path, make_atom("path"));
    sexpr_list_append(path, make_str(path_val));
    sexpr_list_append(path, mk_named_str("reference", ref));
    sexpr_list_append(path, mk_named_atom("unit", "1"));
    sexpr_t *proj = sexpr_make_list();
    sexpr_list_append(proj, make_atom("project"));
    sexpr_list_append(proj, make_str(project_name ? project_name : ""));
    sexpr_list_append(proj, path);
    sexpr_t *inst = sexpr_make_list();
    sexpr_list_append(inst, make_atom("instances"));
    sexpr_list_append(inst, proj);
    sexpr_list_append(sym, inst);
    return sym;
}

/* ── Load / save ───────────────────────────────────────────────────────── */

sexpr_t *load_sch(const char *path)
{
    char *buf = kicli_read_file(path, NULL);
    if (!buf) {
        kicli_set_error("cannot read '%s': %s", path, strerror(errno));
        return NULL;
    }
    char errbuf[256];
    sexpr_t *root = sexpr_parse(buf, errbuf, sizeof(errbuf));
    free(buf);
    if (!root) { kicli_set_error("parse error: %s", errbuf); return NULL; }
    return root;
}

int save_sch(sexpr_t *root, const char *path)
{
    return sexpr_write_file(root, path);
}
