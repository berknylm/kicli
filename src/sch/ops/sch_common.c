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

/* Forward decls for helpers used cross-section. */
static int  lib_property_coords(const sexpr_t *lib_def, const char *key,
                                double *lx, double *ly, double *la, int *hide);
static void add_inherited_property(sexpr_t *sym, const sexpr_t *lib_def,
                                   const char *key, const char *value,
                                   double sym_x, double sym_y, double sym_a);

/* If lib_def has (extends "Base"), return the base symbol's lib_def from
 * root. Returns NULL if no extends, base not loaded, or lib_id missing.
 * The returned pointer is non-owning. */
static sexpr_t *find_extends_base(const sexpr_t *lib_def, const sexpr_t *root,
                                   const char *lib_id)
{
    if (!lib_def || !lib_id) return NULL;
    for (size_t i = 1; i < lib_def->num_children; i++) {
        const sexpr_t *c = lib_def->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "extends") != 0) continue;
        const char *base_name = c->children[1]->value;
        if (!base_name) return NULL;
        const char *colon = strchr(lib_id, ':');
        if (!colon) return NULL;
        char base_id[256];
        snprintf(base_id, sizeof(base_id), "%.*s:%s",
                 (int)(colon - lib_id), lib_id, base_name);
        return find_lib_symbol_node(root, base_id);
    }
    return NULL;
}

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

    /* Resolve (extends "Base") aliases (e.g. LM358 → LM2904) — recurse
     * to import the base symbol too if it isn't already in the sheet. */
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
/* NOTE: ensure_lib_symbol_in_sheet keeps its own extends loop because it
 * recursively re-invokes ensure_lib_symbol_in_sheet (which find_extends_base
 * cannot do — it only returns existing nodes, not load them). */

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

/* Auto-place: 6 columns × N rows, 38.1 mm × 30.48 mm cell. Both dims are
 * 1.27 mm grid multiples (30 × and 24 ×) so placed pins still snap.
 * Wide enough that one symbol's labels rarely collide with the next. */
void auto_slot(size_t idx, double *x, double *y)
{
    size_t col = idx % 6;
    size_t row = idx / 6;
    *x = 50.0 + col * 38.1;
    *y = 50.0 + row * 30.48;
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

/* Find a (sheet) child whose (property "Sheetname" "name") matches.
 * Walks the root's children directly. Pin coords on a sheet block are
 * stored as absolute world coords already (no rotation/mirror to fold in). */
int sheet_pin_pos(const sexpr_t *root, const char *sheet_name,
                  const char *pin_name,
                  double *wx, double *wy, double *wangle)
{
    *wx = *wy = *wangle = 0;
    for (size_t i = 0; i < root->num_children; i++) {
        const sexpr_t *sh = root->children[i];
        if (!sh || sh->type != SEXPR_LIST || sh->num_children == 0) continue;
        if (!sh->children[0]->value || strcmp(sh->children[0]->value, "sheet") != 0) continue;

        /* Match Sheetname */
        const char *got_name = NULL;
        for (size_t j = 1; j < sh->num_children; j++) {
            const sexpr_t *c = sh->children[j];
            if (!c || c->type != SEXPR_LIST || c->num_children < 3) continue;
            if (!c->children[0]->value || strcmp(c->children[0]->value, "property") != 0) continue;
            if (c->children[1]->value && strcmp(c->children[1]->value, "Sheetname") == 0) {
                got_name = c->children[2]->value;
                break;
            }
        }
        if (!got_name || strcmp(got_name, sheet_name) != 0) continue;

        /* Walk the sheet's (pin "name" type (at X Y A) …) children. */
        for (size_t j = 1; j < sh->num_children; j++) {
            const sexpr_t *p = sh->children[j];
            if (!p || p->type != SEXPR_LIST || p->num_children < 4) continue;
            if (!p->children[0]->value || strcmp(p->children[0]->value, "pin") != 0) continue;
            if (!p->children[1]->value || strcmp(p->children[1]->value, pin_name) != 0) continue;
            for (size_t k = 2; k < p->num_children; k++) {
                const sexpr_t *at = p->children[k];
                if (!at || at->type != SEXPR_LIST || at->num_children < 3) continue;
                if (!at->children[0]->value || strcmp(at->children[0]->value, "at") != 0) continue;
                if (at->children[1]->value) *wx = atof(at->children[1]->value);
                if (at->children[2]->value) *wy = atof(at->children[2]->value);
                if (at->num_children > 3 && at->children[3]->value)
                    *wangle = atof(at->children[3]->value);
                return 0;
            }
        }
        kicli_set_error("sheet '%s' has no pin '%s'", sheet_name, pin_name);
        return -1;
    }
    kicli_set_error("sheet named '%s' not found", sheet_name);
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
        sexpr_t *base = find_extends_base(lib_def, root, lib_id);
        if (base && find_lib_pin_recursive(base, pin_num, &lx, &ly, &langle) == 0)
            found = 1;
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

/* Walk lib_def for a pin with matching number; return its electrical-type
 * atom (children[1] of the pin node). Static buffer — caller must copy
 * if storing across calls. */
static const char *find_lib_pin_type(const sexpr_t *node, const char *num)
{
    if (!node || node->type != SEXPR_LIST || node->num_children == 0) return NULL;
    const char *tag = node->children[0]->value;
    if (!tag) return NULL;

    if (strcmp(tag, "pin") == 0) {
        const char *etype = (node->num_children >= 2 && node->children[1])
                            ? node->children[1]->value : NULL;
        for (size_t i = 1; i < node->num_children; i++) {
            const sexpr_t *c = node->children[i];
            if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
            if (!c->children[0]->value || strcmp(c->children[0]->value, "number") != 0) continue;
            if (c->children[1]->value && strcmp(c->children[1]->value, num) == 0)
                return etype;
        }
        return NULL;
    }
    for (size_t i = 1; i < node->num_children; i++) {
        const char *r = find_lib_pin_type(node->children[i], num);
        if (r) return r;
    }
    return NULL;
}

const char *placed_pin_type(sexpr_t *root, const char *ref, const char *pin_num)
{
    sexpr_t *sym = find_placed_by_ref(root, ref);
    if (!sym) return NULL;
    const char *lib_id = placed_lib_id(sym);
    if (!lib_id) return NULL;
    sexpr_t *lib_def = find_lib_symbol_node(root, lib_id);
    if (!lib_def) return NULL;
    const char *t = find_lib_pin_type(lib_def, pin_num);
    if (t) return t;
    /* Try extends-base for aliases. */
    sexpr_t *base = find_extends_base(lib_def, root, lib_id);
    return base ? find_lib_pin_type(base, pin_num) : NULL;
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

/* Label-style effects: same default font + KiCad's standard label justify
 * convention so the text "tail" sits at the wire and the body extends
 * outward, not overlapping the connecting symbol.
 *
 *   angle  0 / 90  → (justify left  bottom)   anchor on tail end
 *   angle 180 / 270→ (justify right bottom)
 *
 * Verified against KiCad's "Add Label" output for both horizontal pins. */
static sexpr_t *mk_effects_label(double angle)
{
    sexpr_t *eff = mk_effects_default();
    double a = fmod(angle + 720.0, 360.0);
    const char *h = (a < 180.0 - 0.001) ? "left" : "right";
    sexpr_t *just = sexpr_make_list();
    sexpr_list_append(just, make_atom("justify"));
    sexpr_list_append(just, make_atom(h));
    sexpr_list_append(just, make_atom("bottom"));
    sexpr_list_append(eff, just);
    return eff;
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
    /* KiCad's GUI emits these on every placed-symbol property; without
     * (show_name no) KiCad renders "Reference: R1" instead of just "R1". */
    if (hide) {
        sexpr_t *h = sexpr_make_list();
        sexpr_list_append(h, make_atom("hide"));
        sexpr_list_append(h, make_atom("yes"));
        sexpr_list_append(prop, h);
    }
    sexpr_list_append(prop, mk_named_atom("show_name", "no"));
    sexpr_list_append(prop, mk_named_atom("do_not_autoplace", "no"));
    sexpr_list_append(prop, mk_effects_default());
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
    sexpr_t *base = find_extends_base(lib_def, root, lib_id);
    if (base) {
        /* Recurse with base's lib_id; needed for chained extends. */
        const char *colon = strchr(lib_id, ':');
        char base_id[256];
        for (size_t i = 1; colon && i < lib_def->num_children; i++) {
            const sexpr_t *c = lib_def->children[i];
            if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
            if (!c->children[0]->value || strcmp(c->children[0]->value, "extends") != 0) continue;
            snprintf(base_id, sizeof(base_id), "%.*s:%s",
                     (int)(colon - lib_id), lib_id, c->children[1]->value);
            return lib_default_property(base, root, base_id, key);
        }
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
    sexpr_list_append(sym, mk_named_atom("body_style", "1"));
    sexpr_list_append(sym, mk_named_atom("exclude_from_sim", "no"));
    sexpr_list_append(sym, mk_named_atom("in_bom",   "yes"));
    sexpr_list_append(sym, mk_named_atom("on_board", "yes"));
    sexpr_list_append(sym, mk_named_atom("dnp",      "no"));
    sexpr_list_append(sym, mk_named_atom("in_pos_files", "yes"));
    sexpr_list_append(sym, mk_named_atom("fields_autoplaced", "yes"));
    sexpr_list_append(sym, mk_uuid_node());

    /* Inherit defaults + property positions from the library symbol so
     * Reference / Value / Footprint text lands where KiCad would draw it
     * natively (no hardcoded offsets). `--footprint` overrides the value
     * by patching after the builder returns. */
    const char *def_fp   = lib_default_property(lib_def, root, lib_id, "Footprint");
    const char *def_ds   = lib_default_property(lib_def, root, lib_id, "Datasheet");
    const char *def_desc = lib_default_property(lib_def, root, lib_id, "Description");

    add_inherited_property(sym, lib_def, "Reference", ref,   x, y, angle);
    add_inherited_property(sym, lib_def, "Value",     value, x, y, angle);
    add_inherited_property(sym, lib_def, "Footprint", def_fp ? def_fp : "",  x, y, angle);
    add_inherited_property(sym, lib_def, "Datasheet", def_ds ? def_ds : "",  x, y, angle);
    add_inherited_property(sym, lib_def, "Description", def_desc ? def_desc : "", x, y, angle);

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
    sexpr_list_append(lbl, mk_effects_label(angle));
    sexpr_list_append(lbl, mk_uuid_node());
    return lbl;
}

sexpr_t *mk_global_label(const char *text, const char *shape,
                         double x, double y, double angle)
{
    sexpr_t *lbl = sexpr_make_list();
    sexpr_list_append(lbl, make_atom("global_label"));
    sexpr_list_append(lbl, make_str(text));
    sexpr_list_append(lbl, mk_named_atom("shape", shape ? shape : "input"));
    sexpr_list_append(lbl, mk_at(x, y, angle));
    /* Global labels have their own arrow-shaped frame; text centers
     * inside the frame. KiCad's GUI default uses (justify left)
     * (left = horizontal anchor, no vertical) — text is vertically
     * centered automatically. */
    /* Global label justify: angle 0/90 → left (text extends right of anchor),
     * angle 180/270 → right (text extends left). Without this, KiCad renders
     * the label rotated 180° (upside-down) on left-side pins. */
    sexpr_t *eff = mk_effects_default();
    double a = fmod(angle + 720.0, 360.0);
    const char *h = (a < 180.0 - 0.001) ? "left" : "right";
    sexpr_t *just = sexpr_make_list();
    sexpr_list_append(just, make_atom("justify"));
    sexpr_list_append(just, make_atom(h));
    sexpr_list_append(eff, just);
    sexpr_list_append(lbl, eff);
    sexpr_list_append(lbl, mk_uuid_node());
    /* KiCad always emits an Intersheetrefs property on global_label so
     * cross-sheet reference annotations can be rendered. */
    sexpr_list_append(lbl, mk_property("Intersheetrefs", "${INTERSHEET_REFS}",
                                       x, y, angle, 1));
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
    sexpr_list_append(lbl, mk_effects_label(angle));
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

/* Walk a power-lib symbol definition and return its only pin's angle
 * (in lib-coord convention: 0=right, 90=up, 180=left, 270=down).
 * Returns -1 if the lib_def isn't loaded yet — caller falls back to 0. */
static double first_lib_pin_angle(const sexpr_t *lib_def)
{
    if (!lib_def) return -1;
    double x, y, a;
    /* Power symbols all use pin number "1". */
    if (find_lib_pin_recursive(lib_def, "1", &x, &y, &a) == 0) return a;
    return -1;
}

/* Place-angle so the power-port body extends AWAY from the connecting
 * symbol (i.e., in the connecting pin's OUTWARD direction).
 *
 * Both lib pin angles and (at) rotation use the same convention. The
 * power port's body extends in its lib pin's direction (since the body
 * is reached by following the pin from its open end into the symbol).
 * After rotating the placement by α (KiCad CW positive), the body's
 * direction on screen becomes (lib_pin_angle - α) mod 360.
 *
 * We want body direction = OUTWARD = (wa + 180) mod 360 where wa is the
 * connecting pin's INTO direction (what world_pin_pos returns).
 * Solve:  α = (lib_pin_angle - outward + 360) mod 360
 *           = (lib_pin_angle - wa + 180 + 720) mod 360
 *
 * Worked examples (matches what KiCad's "Choose Symbol" produces when
 * you snap +3.3V to the top of a vertical resistor and GND to the
 * bottom — both end up at α=0, the lib default):
 *
 *   R pin 1 (top, wa=270) + +3.3V (lib=90):  α = 90-270+180 = 0   ✓
 *   R pin 2 (bot, wa= 90) + GND   (lib=270): α = 270-90+180 = 360 ✓
 *   pin facing right (wa=180)    + GND     : α = 270-180+180 = 270 ✓
 */
static double aligned_power_port_angle(double lib_pin_angle, double pin_into_angle)
{
    if (lib_pin_angle < 0) return 0;  /* unknown lib geometry — accept default */
    return fmod(lib_pin_angle - pin_into_angle + 180.0 + 720.0, 360.0);
}

/* (wire (pts (xy x1 y1) (xy x2 y2)) (stroke (width 0) (type default)) (uuid …)) */
sexpr_t *mk_wire(double x1, double y1, double x2, double y2)
{
    sexpr_t *wire = sexpr_make_list();
    sexpr_list_append(wire, make_atom("wire"));
    sexpr_t *pts = sexpr_make_list();
    sexpr_list_append(pts, make_atom("pts"));
    char b[32];
    sexpr_t *xy1 = sexpr_make_list();
    sexpr_list_append(xy1, make_atom("xy"));
    fmt_num(b, sizeof(b), x1); sexpr_list_append(xy1, make_atom(b));
    fmt_num(b, sizeof(b), y1); sexpr_list_append(xy1, make_atom(b));
    sexpr_list_append(pts, xy1);
    sexpr_t *xy2 = sexpr_make_list();
    sexpr_list_append(xy2, make_atom("xy"));
    fmt_num(b, sizeof(b), x2); sexpr_list_append(xy2, make_atom(b));
    fmt_num(b, sizeof(b), y2); sexpr_list_append(xy2, make_atom(b));
    sexpr_list_append(pts, xy2);
    sexpr_list_append(wire, pts);
    sexpr_t *stroke = sexpr_make_list();
    sexpr_list_append(stroke, make_atom("stroke"));
    sexpr_t *w = sexpr_make_list();
    sexpr_list_append(w, make_atom("width"));
    sexpr_list_append(w, make_atom("0"));
    sexpr_list_append(stroke, w);
    sexpr_t *t = sexpr_make_list();
    sexpr_list_append(t, make_atom("type"));
    sexpr_list_append(t, make_atom("default"));
    sexpr_list_append(stroke, t);
    sexpr_list_append(wire, stroke);
    sexpr_list_append(wire, mk_uuid_node());
    return wire;
}

/* Convert pin's into-body angle to a (dx, dy) outward offset of `step` mm.
 * Coordinate convention matches world_pin_pos: +x right, +y down on screen,
 * angle math (0=right, 90=up math = up on screen). */
void offset_outward(double pin_into_angle, double step, double *dx, double *dy)
{
    double outward = fmod(pin_into_angle + 180.0 + 720.0, 360.0);
    double rad = outward * (3.14159265358979323846 / 180.0);
    *dx = step * cos(rad);
    *dy = -step * sin(rad);  /* y-down on screen → positive sin = up = -dy */
}

/* Look up a property's lib coords in a lib_def. Returns 1 if found. */
static int lib_property_coords(const sexpr_t *lib_def, const char *key,
                               double *lx, double *ly, double *la, int *hide)
{
    if (!lib_def) return 0;
    for (size_t i = 1; i < lib_def->num_children; i++) {
        const sexpr_t *p = lib_def->children[i];
        if (!p || p->type != SEXPR_LIST || p->num_children < 4) continue;
        if (!p->children[0]->value || strcmp(p->children[0]->value, "property") != 0) continue;
        if (!p->children[1]->value || strcmp(p->children[1]->value, key) != 0) continue;
        const sexpr_t *at = p->children[3];
        if (!at || at->type != SEXPR_LIST || at->num_children < 4) return 0;
        if (lx) *lx = atof(at->children[1]->value ? at->children[1]->value : "0");
        if (ly) *ly = atof(at->children[2]->value ? at->children[2]->value : "0");
        if (la) *la = atof(at->children[3]->value ? at->children[3]->value : "0");
        /* hide can live as a top-level (hide yes) sibling of (at) (lib
         * convention), OR inside (effects … (hide yes) …) (placed
         * convention). Check both. */
        if (hide) {
            *hide = 0;
            for (size_t k = 4; k < p->num_children; k++) {
                const sexpr_t *c = p->children[k];
                if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
                if (c->children[0]->value && strcmp(c->children[0]->value, "hide") == 0
                    && c->children[1]->value && strcmp(c->children[1]->value, "yes") == 0) {
                    *hide = 1;
                    continue;
                }
                if (c->children[0]->value && strcmp(c->children[0]->value, "effects") == 0) {
                    for (size_t m = 1; m < c->num_children; m++) {
                        const sexpr_t *h = c->children[m];
                        if (!h || h->type != SEXPR_LIST || h->num_children < 2) continue;
                        if (h->children[0]->value && strcmp(h->children[0]->value, "hide") == 0
                            && h->children[1]->value && strcmp(h->children[1]->value, "yes") == 0)
                            *hide = 1;
                    }
                }
            }
        }
        return 1;
    }
    return 0;
}

/* Add a property at lib_def's stored coordinate, transformed by the
 * placement (sym_x, sym_y, sym_a). Falls back to (sym_x, sym_y) if the
 * lib doesn't have the property. */
static void add_inherited_property(sexpr_t *sym, const sexpr_t *lib_def,
                                   const char *key, const char *value,
                                   double sym_x, double sym_y, double sym_a)
{
    double lx = 0, ly = 0, la = 0; int hide = 0;
    int found = lib_property_coords(lib_def, key, &lx, &ly, &la, &hide);
    if (!found) { lx = ly = 0; la = 0; hide = (strcmp(key,"Reference")==0); }
    /* Same transform as world_pin_pos so property text follows the same
     * rotation as the pins. KiCad y is +down on screen; lib +y is up. */
    double rad = sym_a * (3.14159265358979323846 / 180.0);
    double ca = cos(rad), sa = sin(rad);
    double wx = sym_x + lx * ca - ly * sa;
    double wy = sym_y - lx * sa - ly * ca;
    /* KiCad's text-angle rule (verified against GUI-corrected examples):
     *   VISIBLE properties (hide=no, e.g. Reference, Value) → keep lib
     *     angle as-is. Symbol rotation moves the position but NOT the
     *     text orientation (text stays in lib frame for readability).
     *   HIDDEN properties (hide=yes, e.g. Footprint, Datasheet) → rotate
     *     with the symbol: text_angle = (lib_angle + sym_a) mod 360. */
    double text_angle = hide
        ? fmod(la + sym_a + 720.0, 360.0)
        : la;
    sexpr_list_append(sym, mk_property(key, value ? value : "",
                                       wx, wy, text_angle, hide));
}

/* See header for contract — placed AT the connecting pin coord (matches
 * KiCad's "Choose Symbol" snap behavior; power-port pin length is 0 so
 * the (at) coord IS the connection point). Properties (Reference, Value,
 * Footprint, Datasheet) inherit their positions from the lib_def, so the
 * "+3.3V" / "GND" text lands where KiCad would draw it natively. */
sexpr_t *mk_power_port(const sexpr_t *root, const char *rail,
                       double x, double y, double pin_into_angle,
                       const char *root_uuid, const char *project_name,
                       double *out_x, double *out_y)
{
    char lib_id[96];
    snprintf(lib_id, sizeof(lib_id), "power:%s", rail);

    if (out_x) *out_x = x;
    if (out_y) *out_y = y;

    const sexpr_t *lib_def = find_lib_symbol_node(root, lib_id);
    double lib_pin_angle = first_lib_pin_angle(lib_def);
    double angle = aligned_power_port_angle(lib_pin_angle, pin_into_angle);

    sexpr_t *sym = sexpr_make_list();
    sexpr_list_append(sym, make_atom("symbol"));
    sexpr_list_append(sym, mk_named_str("lib_id", lib_id));
    sexpr_list_append(sym, mk_at(x, y, angle));
    sexpr_list_append(sym, mk_named_atom("unit", "1"));
    sexpr_list_append(sym, mk_named_atom("body_style", "1"));
    sexpr_list_append(sym, mk_named_atom("exclude_from_sim", "no"));
    sexpr_list_append(sym, mk_named_atom("in_bom",   "no"));
    sexpr_list_append(sym, mk_named_atom("on_board", "yes"));
    sexpr_list_append(sym, mk_named_atom("dnp",      "no"));
    sexpr_list_append(sym, mk_named_atom("in_pos_files", "yes"));
    sexpr_list_append(sym, mk_named_atom("fields_autoplaced", "yes"));
    sexpr_list_append(sym, mk_uuid_node());

    char ref[32];
    snprintf(ref, sizeof(ref), "#PWR%04d", ++g_pwr_counter);
    add_inherited_property(sym, lib_def, "Reference", ref,  x, y, angle);
    add_inherited_property(sym, lib_def, "Value",     rail, x, y, angle);
    add_inherited_property(sym, lib_def, "Footprint", "",   x, y, angle);
    add_inherited_property(sym, lib_def, "Datasheet", "",   x, y, angle);

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
