/*
 * draw.c — kicli sch place / net / nc
 *
 * Label-first schematic authoring:
 *
 *   place  adds a (symbol ...) from the bundled KiCad library catalog.
 *   net    attaches a label or power port at each specified pin's world
 *          position. Wires are never emitted; every connection is a label.
 *   nc     places (no_connect ...) markers at pin positions.
 *
 * Design philosophy: agent provides a logical spec (components + nets);
 * kicli emits valid KiCad 10 s-expression with correct pin positions,
 * grid-snapped placements, and proper UUIDs. No 2-D layout optimization.
 * Output is electrically correct but cosmetically utilitarian — the
 * human (or GUI) rearranges if they want prettiness.
 */

#include "kicli/sch.h"
#include "kicli/error.h"
#include "kicli/portable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>

/* ── Shared helpers ─────────────────────────────────────────────────────── */

static sexpr_t *make_atom(const char *v)       { return sexpr_make_atom(v); }
static sexpr_t *make_str (const char *v)       { return sexpr_make_str(v); }

static void append_atoms(sexpr_t *list, const char *first, ...)
{
    /* Unused placeholder for readability; kept for future builder DSL. */
    (void)list; (void)first;
}

/* Build (tag v1 v2 ...) from bare atoms. Handy for (at 100 50 0) style. */
static sexpr_t *mklist_atoms(const char *tag, ...)
{
    (void)tag;
    return NULL; /* not needed; explicit builders below are clearer */
}

/* Double → compact string "123.45" with up to 6 fractional digits, no
 * trailing zeros. KiCad itself uses the same cleanup on save. */
static void fmt_num(char *out, size_t sz, double v)
{
    snprintf(out, sz, "%.6f", v);
    /* trim trailing zeros and dangling dot */
    char *dot = strchr(out, '.');
    if (!dot) return;
    char *end = out + strlen(out) - 1;
    while (end > dot && *end == '0') *end-- = '\0';
    if (end == dot) *end = '\0';
}

static sexpr_t *mk_at(double x, double y, double a)
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

static sexpr_t *mk_named_atom(const char *tag, const char *val)
{
    sexpr_t *l = sexpr_make_list();
    sexpr_list_append(l, make_atom(tag));
    sexpr_list_append(l, make_atom(val));
    return l;
}

static sexpr_t *mk_named_str(const char *tag, const char *val)
{
    sexpr_t *l = sexpr_make_list();
    sexpr_list_append(l, make_atom(tag));
    sexpr_list_append(l, make_str(val));
    return l;
}

/* Snap to a grid size (in mm). Default 1.27 matches KiCad's schematic
 * default and all bundled lib pins land on it. */
static double snap_grid(double v, double step)
{
    if (step <= 0) return v;
    return round(v / step) * step;
}

/* ── Schematic traversal ────────────────────────────────────────────────── */

/* Return root (uuid "...") value or NULL. */
static const char *get_root_uuid(const sexpr_t *root)
{
    return sexpr_atom_value(root, "uuid");
}

/* Derive project name from the schematic path:
 *   /path/to/Foo.kicad_sch   → "Foo"
 *   /path/to/foo/Foo.kicad_sch → "Foo" (basename stem)
 * The (instances) block in each placed symbol references this. */
static void project_name_for(const char *sch_path, char *out, size_t sz)
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

/* Find (lib_symbols ...) block, creating it right before any (symbol|sheet|…)
 * child if missing. Returns pointer into the tree. */
static sexpr_t *get_or_create_lib_symbols(sexpr_t *root)
{
    sexpr_t *ls = sexpr_get(root, "lib_symbols");
    if (ls) return ls;
    ls = sexpr_make_list();
    sexpr_list_append(ls, make_atom("lib_symbols"));
    sexpr_list_append(root, ls);
    return ls;
}

static int lib_symbols_has(const sexpr_t *ls, const char *lib_id)
{
    for (size_t i = 1; i < ls->num_children; i++) {
        sexpr_t *c = ls->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "symbol") != 0) continue;
        if (c->children[1]->value && strcmp(c->children[1]->value, lib_id) == 0) return 1;
    }
    return 0;
}

/* Copy the bundled-library symbol definition into the sheet's lib_symbols.
 * Returns 0 on success, -1 on not found, -2 on OOM. */
static int ensure_lib_symbol_in_sheet(sexpr_t *root, const char *lib_id)
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

    /* Resolve (extends "Base") — if the bundled copy references a sibling,
     * also clone the sibling into the sheet. Needed for aliases like
     * LM358 → LM2904. */
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
        if (!lib_symbols_has(ls, base_lib_id)) {
            (void)ensure_lib_symbol_in_sheet(root, base_lib_id);
        }
        break;
    }
    return 0;
}

static sexpr_t *find_lib_symbol_node(const sexpr_t *root, const char *lib_id)
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

/* Walk root's children for (symbol ...) nodes, count non-power, non-pwr
 * placements — used for auto-grid slot selection. */
static size_t count_placed_symbols(const sexpr_t *root)
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

/* auto-grid: column-major, 10 columns, 25mm × 20mm cell. */
static void auto_slot(size_t idx, double *x, double *y)
{
    size_t col = idx % 10;
    size_t row = idx / 10;
    *x = 50.0 + col * 25.0;
    *y = 50.0 + row * 20.0;
}

/* Find a placed (symbol ...) whose (property "Reference" "REF") matches. */
static sexpr_t *find_placed_by_ref(const sexpr_t *root, const char *ref)
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

/* Extract "Lib:Name" value from a placed (symbol (lib_id "...") ...). */
static const char *placed_lib_id(const sexpr_t *sym)
{
    for (size_t i = 1; i < sym->num_children; i++) {
        sexpr_t *c = sym->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "lib_id") != 0) continue;
        return c->children[1]->value;
    }
    return NULL;
}

/* Extract (at X Y A) from a placed symbol. */
static int placed_at(const sexpr_t *sym, double *x, double *y, double *a)
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

/* Extract 1 if symbol has (mirror x/y). */
static int placed_mirror_x(const sexpr_t *sym)
{
    for (size_t i = 1; i < sym->num_children; i++) {
        sexpr_t *c = sym->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "mirror") != 0) continue;
        if (c->children[1]->value && strcmp(c->children[1]->value, "x") == 0) return 1;
    }
    return 0;
}

/* Walk a library-symbol definition, including sub-units, and find the pin
 * whose (number "N") matches `num`. Writes its (at X Y ANGLE) into out
 * coords. Returns 0 on success. */
static int find_lib_pin_recursive(const sexpr_t *node, const char *num,
                                   double *x, double *y, double *a)
{
    if (!node || node->type != SEXPR_LIST || node->num_children == 0) return -1;
    const char *tag = node->children[0]->value;
    if (!tag) return -1;

    if (strcmp(tag, "pin") == 0) {
        /* (pin <type> <dir> (at X Y ANGLE) ... (number "N" ...)) */
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

    for (size_t i = 1; i < node->num_children; i++) {
        if (find_lib_pin_recursive(node->children[i], num, x, y, a) == 0)
            return 0;
    }
    return -1;
}

/* Compute the world (sheet-space) position + outward angle of a pin on a
 * placed symbol. Matches dump.c's formula so `view` and `net` stay in
 * lock-step. Returns 0 on success. */
static int world_pin_pos(sexpr_t *root, const char *ref, const char *pin_num,
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

    /* Aliases like LM358 store their pins on the (extends "LM2904") base.
     * The base was already copied to lib_symbols by ensure_lib_symbol_in_sheet,
     * so we can walk that sibling here. */
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

    /* World pin outward angle. Library pin angle points away from the
     * symbol body. Rotation of symbol adds; y-inversion of schematic
     * frame means we negate the symbol rotation. Mirror-x reflects it. */
    double out = langle - sym_a;
    if (mirror_x) out = 180.0 - out;
    while (out < 0)    out += 360.0;
    while (out >= 360) out -= 360.0;
    *wangle = out;
    return 0;
}

/* ── Power-rail detection ───────────────────────────────────────────────── */

static int str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* Returns 1 if net_name looks like a power rail and writes the canonical
 * `power:` symbol name to canon_out. Otherwise returns 0. Keep the table
 * tiny; users can always override with --as local/global/power. */
static int detect_power(const char *net_name, char *canon_out, size_t sz)
{
    if (!net_name || !*net_name) return 0;

    /* Exact matches (case-insensitive) for well-known global rails. */
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

    /* Voltage-looking patterns: optional '+' or '-', digits, optional
     * 'V' or '.digits', then optional 'V'. Examples:
     *   +3V3, +3.3V, +5V, +12V, -12V, 3V3, 5V, 12V, +48V, +1V8 */
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

    /* Canonical form: always prefix with '+' for positive rails. */
    if (sign == 0) snprintf(canon_out, sz, "+%s", num_start);
    else           snprintf(canon_out, sz, "%c%s", sign, num_start);
    return 1;
}

/* ── Node builders ──────────────────────────────────────────────────────── */

static sexpr_t *mk_effects_default(void)
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

static sexpr_t *mk_uuid_node(void)
{
    char u[40];
    kicli_uuid4(u, sizeof(u));
    sexpr_t *n = sexpr_make_list();
    sexpr_list_append(n, make_atom("uuid"));
    sexpr_list_append(n, make_str(u));
    return n;
}

static sexpr_t *mk_property(const char *key, const char *value,
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

/* Look up a (property "key" "value" ...) in a lib_def, optionally falling
 * back to its (extends "Base") sibling. Returns the cached const char* or
 * NULL if neither has the property. */
static const char *lib_default_property(const sexpr_t *lib_def,
                                         const sexpr_t *root,
                                         const char *lib_id,
                                         const char *key)
{
    if (!lib_def) return NULL;
    for (size_t i = 1; i < lib_def->num_children; i++) {
        sexpr_t *c = lib_def->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children < 3) continue;
        if (!c->children[0]->value || strcmp(c->children[0]->value, "property") != 0) continue;
        if (c->children[1]->value && strcmp(c->children[1]->value, key) == 0)
            return c->children[2]->value;
    }
    /* Try extends base — common for aliases (LM358 → LM2904) */
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

static sexpr_t *mk_placed_symbol(const char *lib_id, const char *ref,
                                  const char *value,
                                  double x, double y, double angle,
                                  const sexpr_t *lib_def,
                                  const sexpr_t *root,
                                  const char *root_uuid,
                                  const char *project_name)
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

    /* Inherit defaults from the library symbol — the same convention KiCad
     * uses when a user drops a symbol from the palette. The `place --footprint`
     * flag still wins because we patch the Footprint property after. */
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
        /* sub-unit: walk its pins */
        for (size_t j = 1; j < c->num_children; j++) {
            sexpr_t *p = c->children[j];
            if (!p || p->type != SEXPR_LIST || p->num_children == 0) continue;
            if (!p->children[0]->value
                || strcmp(p->children[0]->value, "pin") != 0) continue;
            /* extract number */
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

static sexpr_t *mk_label(const char *text, double x, double y, double angle)
{
    sexpr_t *lbl = sexpr_make_list();
    sexpr_list_append(lbl, make_atom("label"));
    sexpr_list_append(lbl, make_str(text));
    sexpr_list_append(lbl, mk_at(x, y, angle));
    sexpr_list_append(lbl, mk_effects_default());
    sexpr_list_append(lbl, mk_uuid_node());
    return lbl;
}

static sexpr_t *mk_global_label(const char *text, const char *shape,
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

static sexpr_t *mk_no_connect(double x, double y)
{
    /* no_connect uses a 2-D (at X Y) without an angle. KiCad refuses
     * the 3-arg form (it's what broke the earlier round-trip test). */
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

/* Power port = placed symbol from KiCad's "power" library. */
static sexpr_t *mk_power_port(const char *rail, double x, double y, double angle,
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
    sexpr_list_append(sym, mk_named_atom("in_bom",   "no"));   /* power symbols not in BOM */
    sexpr_list_append(sym, mk_named_atom("on_board", "yes"));
    sexpr_list_append(sym, mk_named_atom("dnp",      "no"));
    sexpr_list_append(sym, mk_uuid_node());

    /* Reference prefix is "#PWR" — KiCad convention, the '#' hides it
     * from lists, BOMs, and footprint linkage. */
    static int pwr_counter = 0;
    char ref[32];
    snprintf(ref, sizeof(ref), "#PWR%04d", ++pwr_counter);
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

/* ── Shared load+parse+save scaffolding ─────────────────────────────────── */

static sexpr_t *load_sch(const char *path)
{
    char *buf = kicli_read_file(path, NULL);
    if (!buf) { kicli_set_error("cannot read '%s': %s", path, strerror(errno)); return NULL; }
    char errbuf[256];
    sexpr_t *root = sexpr_parse(buf, errbuf, sizeof(errbuf));
    free(buf);
    if (!root) { kicli_set_error("parse error: %s", errbuf); return NULL; }
    return root;
}

static int save_sch(sexpr_t *root, const char *path)
{
    return sexpr_write_file(root, path);
}

/* ── cmd_sch_place ──────────────────────────────────────────────────────── */

int cmd_sch_place(const char *sch_path, int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
          "Usage: kicli sch <file> place <lib_id> <ref> [<value>]\n"
          "                   [--at X,Y] [--angle {0|90|180|270}] [--mirror x|y]\n"
          "                   [--footprint FP]\n"
          "\n"
          "Places a symbol from the bundled KiCad library catalog. If --at\n"
          "is omitted, kicli auto-picks a grid slot (10 columns × N rows).\n"
          "All coordinates snap to the 1.27 mm grid so library pins\n"
          "electrically align with future labels.\n");
        return 1;
    }

    const char *lib_id = argv[0];
    const char *ref    = argv[1];
    const char *value  = (argc >= 3 && argv[2][0] != '-') ? argv[2] : ref;

    double at_x = 0, at_y = 0, at_a = 0;
    int have_at = 0, mirror_x = 0, mirror_y = 0;
    const char *footprint = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--at") == 0 && i + 1 < argc) {
            const char *s = argv[++i];
            if (sscanf(s, "%lf,%lf", &at_x, &at_y) != 2) {
                fprintf(stderr, "error: --at expects X,Y (e.g. --at 100,50)\n");
                return 1;
            }
            have_at = 1;
        } else if (strcmp(argv[i], "--angle") == 0 && i + 1 < argc) {
            at_a = atof(argv[++i]);
        } else if (strcmp(argv[i], "--mirror") == 0 && i + 1 < argc) {
            const char *m = argv[++i];
            if (*m == 'x' || *m == 'X') mirror_x = 1;
            else if (*m == 'y' || *m == 'Y') mirror_y = 1;
        } else if (strcmp(argv[i], "--footprint") == 0 && i + 1 < argc) {
            footprint = argv[++i];
        }
    }

    sexpr_t *root = load_sch(sch_path);
    if (!root) { fprintf(stderr, "error: %s\n", kicli_last_error()); return 3; }

    if (find_placed_by_ref(root, ref)) {
        fprintf(stderr, "error: reference '%s' already exists — pick a different one\n", ref);
        sexpr_free(root); return 1;
    }

    int rc = ensure_lib_symbol_in_sheet(root, lib_id);
    if (rc != 0) {
        fprintf(stderr, "error: library symbol '%s' not found in bundled catalog\n", lib_id);
        fprintf(stderr, "  (project-local libs not yet supported by `place` — see roadmap)\n");
        sexpr_free(root); return 2;
    }
    sexpr_t *lib_def = find_lib_symbol_node(root, lib_id);

    if (!have_at) {
        size_t idx = count_placed_symbols(root);
        auto_slot(idx, &at_x, &at_y);
    }
    at_x = snap_grid(at_x, 1.27);
    at_y = snap_grid(at_y, 1.27);

    const char *root_uuid = get_root_uuid(root);
    char proj[128];
    project_name_for(sch_path, proj, sizeof(proj));

    sexpr_t *sym = mk_placed_symbol(lib_id, ref, value, at_x, at_y, at_a,
                                     lib_def, root, root_uuid, proj);

    /* Optional (mirror x|y) override. Added after builder for clarity. */
    if (mirror_x || mirror_y) {
        sexpr_t *m = sexpr_make_list();
        sexpr_list_append(m, make_atom("mirror"));
        sexpr_list_append(m, make_atom(mirror_x ? "x" : "y"));
        /* Insert right after (at ...) — we know index 2 in our layout. */
        /* Simpler: just append; KiCad re-orders on save anyway. */
        sexpr_list_append(sym, m);
    }

    /* Override Footprint property if user gave one. */
    if (footprint && *footprint) {
        for (size_t i = 1; i < sym->num_children; i++) {
            sexpr_t *p = sym->children[i];
            if (!p || p->type != SEXPR_LIST || p->num_children < 3) continue;
            if (!p->children[0]->value || strcmp(p->children[0]->value, "property") != 0) continue;
            if (p->children[1]->value && strcmp(p->children[1]->value, "Footprint") == 0) {
                free(p->children[2]->value);
                p->children[2]->value = strdup(footprint);
                break;
            }
        }
    }

    sexpr_list_append(root, sym);

    if (save_sch(root, sch_path) != 0) {
        fprintf(stderr, "error: cannot write '%s': %s\n", sch_path, strerror(errno));
        sexpr_free(root); return 3;
    }

    printf("placed %s at %.2f,%.2f (%s)\n", ref, at_x, at_y, lib_id);
    sexpr_free(root);
    return 0;
}

/* ── cmd_sch_net ────────────────────────────────────────────────────────── */

int cmd_sch_net(const char *sch_path, int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
          "Usage: kicli sch <file> net <net-name> <ref>:<pin> [<ref>:<pin> ...]\n"
          "                   [--as local|global|power]\n"
          "\n"
          "Attaches a label (or power port for known rails) at the exact\n"
          "world position of each listed pin. Label rotation matches the\n"
          "pin's outward angle so text reads away from the symbol.\n"
          "\n"
          "Power rail heuristic (can override with --as):\n"
          "  GND, VCC, VDD, VSS, VEE, GNDA, AGND, PGND, +BATT, -BATT, EARTH\n"
          "  +3V3, +5V, +12V, -12V, 3V3, 5V, 12V, +1V8, +3.3V …\n");
        return 1;
    }

    const char *net  = argv[0];
    const char *force_as = NULL;
    int pin_arg_start = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--as") == 0 && i + 1 < argc) {
            force_as = argv[++i];
        }
    }

    char canon[96];
    int is_power = detect_power(net, canon, sizeof(canon));
    if (force_as) {
        if (strcmp(force_as, "power") == 0) { is_power = 1; if (!canon[0]) snprintf(canon, sizeof(canon), "%s", net); }
        else is_power = 0;
    }

    sexpr_t *root = load_sch(sch_path);
    if (!root) { fprintf(stderr, "error: %s\n", kicli_last_error()); return 3; }

    const char *root_uuid = get_root_uuid(root);
    char proj[128];
    project_name_for(sch_path, proj, sizeof(proj));

    if (is_power) {
        /* Copy the "power:<rail>" lib symbol into lib_symbols so KiCad
         * renders the port graphic. Failure = unknown rail. */
        char lib_id[128];
        snprintf(lib_id, sizeof(lib_id), "power:%s", canon);
        if (ensure_lib_symbol_in_sheet(root, lib_id) != 0) {
            fprintf(stderr,
                "warning: power rail '%s' not found in bundled `power` library;\n"
                "         falling back to a (label \"%s\") instead\n",
                canon, net);
            is_power = 0;
        }
    }

    int n_pins = 0, n_err = 0;
    for (int i = pin_arg_start; i < argc; i++) {
        if (argv[i][0] == '-') {
            /* option — skip including its value (--as <v>) */
            if (strcmp(argv[i], "--as") == 0 && i + 1 < argc) i++;
            continue;
        }
        char refpin[128];
        snprintf(refpin, sizeof(refpin), "%s", argv[i]);
        char *colon = strchr(refpin, ':');
        if (!colon) {
            fprintf(stderr, "error: expected REF:PIN, got '%s'\n", argv[i]);
            n_err++; continue;
        }
        *colon = '\0';
        const char *ref = refpin;
        const char *pin = colon + 1;

        double wx, wy, wa;
        if (world_pin_pos(root, ref, pin, &wx, &wy, &wa) != 0) {
            fprintf(stderr, "error: %s\n", kicli_last_error());
            n_err++; continue;
        }

        wx = snap_grid(wx, 1.27);
        wy = snap_grid(wy, 1.27);

        sexpr_t *primitive;
        if (is_power) {
            primitive = mk_power_port(canon, wx, wy, wa, root_uuid, proj);
        } else {
            primitive = mk_label(net, wx, wy, wa);
        }
        sexpr_list_append(root, primitive);
        n_pins++;
    }

    if (n_pins == 0) {
        fprintf(stderr, "error: no valid pins specified\n");
        sexpr_free(root);
        return 1;
    }

    if (save_sch(root, sch_path) != 0) {
        fprintf(stderr, "error: cannot write '%s': %s\n", sch_path, strerror(errno));
        sexpr_free(root); return 3;
    }

    printf("net '%s' attached to %d pin(s)%s%s\n",
           net, n_pins,
           is_power ? " (power port)" : " (label)",
           n_err ? " — some pins were skipped, see warnings above" : "");
    sexpr_free(root);
    return n_err ? 1 : 0;
}

/* Suppress unused-function warnings on the placeholder helpers. */
static void unused_hush_(void) {
    (void)append_atoms;
    (void)mklist_atoms;
    (void)mk_global_label;
    (void)mk_named_str;
}

/* ── cmd_sch_nc ─────────────────────────────────────────────────────────── */

int cmd_sch_nc(const char *sch_path, int argc, char **argv)
{
    (void)unused_hush_;  /* keep the placeholder helpers referenced */

    if (argc < 1) {
        fprintf(stderr,
          "Usage: kicli sch <file> nc <ref>:<pin> [<ref>:<pin> ...]\n"
          "\n"
          "Places (no_connect) markers at the given pin positions. Use this\n"
          "on pins you deliberately leave floating — it silences ERC.\n");
        return 1;
    }

    sexpr_t *root = load_sch(sch_path);
    if (!root) { fprintf(stderr, "error: %s\n", kicli_last_error()); return 3; }

    int n_pins = 0, n_err = 0;
    for (int i = 0; i < argc; i++) {
        char refpin[128];
        snprintf(refpin, sizeof(refpin), "%s", argv[i]);
        char *colon = strchr(refpin, ':');
        if (!colon) {
            fprintf(stderr, "error: expected REF:PIN, got '%s'\n", argv[i]);
            n_err++; continue;
        }
        *colon = '\0';
        const char *ref = refpin;
        const char *pin = colon + 1;

        double wx, wy, wa;
        if (world_pin_pos(root, ref, pin, &wx, &wy, &wa) != 0) {
            fprintf(stderr, "error: %s\n", kicli_last_error());
            n_err++; continue;
        }
        wx = snap_grid(wx, 1.27);
        wy = snap_grid(wy, 1.27);
        sexpr_list_append(root, mk_no_connect(wx, wy));
        n_pins++;
    }

    if (n_pins == 0) {
        fprintf(stderr, "error: no valid pins specified\n");
        sexpr_free(root);
        return 1;
    }

    if (save_sch(root, sch_path) != 0) {
        fprintf(stderr, "error: cannot write '%s': %s\n", sch_path, strerror(errno));
        sexpr_free(root); return 3;
    }

    printf("NC marker attached to %d pin(s)\n", n_pins);
    sexpr_free(root);
    return n_err ? 1 : 0;
}
