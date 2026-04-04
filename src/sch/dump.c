/*
 * dump.c — kicli sch FILE dump  →  .kisch format
 *
 * Three-pass approach:
 *
 *   Pass 1: parse .kicad_sch
 *     • lib_symbols  → complete pin definitions (num, name, type, local pos)
 *     • placed syms  → ref, value, lib_id, position, angle, mirror
 *     • wires        → connectivity graph
 *     • NC markers   → explicit no-connect positions
 *
 *   Pass 2: kicad-cli netlist → labeled net assignments (kicadsexpr format)
 *
 *   Pass 3: wire-trace union-find
 *     • pins in netmap  → use netmap label
 *     • pins at NC pos  → "NC"
 *     • pins connected by wire but unlabeled → "REF:PIN, REF2:PIN2, ..."
 *     • pins with no wire → "~"
 *
 * grep patterns:
 *   grep '^Y1:'       all Y1 pins
 *   grep '→ NC'       no-connect marked pins
 *   grep '→ ~'        floating pins (ERC error)
 *   grep 'bidir'      bidirectional pins
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  define unlink   _unlink
#  define getpid   GetCurrentProcessId
#  define F_OK     0
#  define access   _access
#else
#  include <unistd.h>
#endif

#include "kicli/sch.h"
#include "kicli/kicad_cli.h"
#include "kicli/error.h"

/* ── Pin type abbreviations ─────────────────────────────────────────────── */

static const char *short_type(const char *t)
{
    if (!t || !t[0])                            return "-";
    if (strncmp(t, "passive",       7)  == 0)  return "pass";
    if (strncmp(t, "input",         5)  == 0)  return "in";
    if (strncmp(t, "output",        6)  == 0)  return "out";
    if (strncmp(t, "bidirectional", 13) == 0)  return "inout";
    if (strncmp(t, "tristate",      8)  == 0)  return "tri";
    if (strncmp(t, "power_in",      8)  == 0)  return "pwrin";
    if (strncmp(t, "power_out",     9)  == 0)  return "pwrout";
    if (strncmp(t, "open_collector",14) == 0)  return "oc";
    if (strncmp(t, "open_emitter",  12) == 0)  return "oe";
    if (strncmp(t, "unspecified",   11) == 0)  return "-";
    if (strncmp(t, "no_connect",    10) == 0)  return "-";
    if (strncmp(t, "free",           4) == 0)  return "free";
    return "-";
}

/* ── Strip leading "/" from KiCad net names ─────────────────────────────── */

static void clean_net(const char *in, char *out, size_t sz)
{
    if (!in || !in[0]) { snprintf(out, sz, "NC"); return; }
    if (strncmp(in, "unconnected", 11) == 0) { snprintf(out, sz, "NC"); return; }
    const char *p = in;
    while (*p == '/') p++;
    snprintf(out, sz, "%s", p[0] ? p : "NC");
}

/* ── Coord key (0.01 mm grid → integer) ─────────────────────────────────── */

static void coord_key(char *buf, size_t sz, double x, double y)
{
    snprintf(buf, sz, "%d,%d", (int)round(x * 100.0), (int)round(y * 100.0));
}

/* ── Union-Find ──────────────────────────────────────────────────────────── */

typedef struct {
    char key[32];   /* coord key */
    int  parent;
} uf_node_t;

static int uf_find(uf_node_t *nodes, int i)
{
    while (nodes[i].parent != i) {
        nodes[i].parent = nodes[nodes[i].parent].parent;
        i = nodes[i].parent;
    }
    return i;
}

static void uf_union(uf_node_t *nodes, int a, int b)
{
    a = uf_find(nodes, a);
    b = uf_find(nodes, b);
    if (a != b) nodes[a].parent = b;
}

static int uf_get_or_add(uf_node_t **nodes, int *count, int *cap,
                          const char *key)
{
    for (int i = 0; i < *count; i++)
        if (strcmp((*nodes)[i].key, key) == 0) return i;
    if (*count >= *cap) {
        *cap = *cap ? *cap * 2 : 256;
        uf_node_t *tmp = realloc(*nodes, (size_t)*cap * sizeof(uf_node_t));
        if (!tmp) return -1;
        *nodes = tmp;
    }
    int idx = (*count)++;
    snprintf((*nodes)[idx].key, sizeof((*nodes)[idx].key), "%s", key);
    (*nodes)[idx].parent = idx;
    return idx;
}

/* ── Pin record (with absolute position for wire tracing) ────────────────── */

typedef struct {
    char   num[64];
    char   name[128];
    char   type[48];
    char   net[256];    /* assigned net or "" */
    double ax, ay;      /* absolute schematic position */
    int    uf_idx;      /* index in uf_nodes */
} dump_pin_t;

typedef struct {
    char      ref[64];
    char      value[256];
    dump_pin_t *pins;
    size_t     num_pins;
    size_t     cap_pins;
} dump_comp_t;

static int pin_cmp(const void *a, const void *b)
{
    const dump_pin_t *pa = a, *pb = b;
    int all_a = 1, all_b = 1;
    for (const char *p = pa->num; *p; p++)
        if (!isdigit((unsigned char)*p)) { all_a = 0; break; }
    for (const char *p = pb->num; *p; p++)
        if (!isdigit((unsigned char)*p)) { all_b = 0; break; }
    if (all_a && all_b) return atoi(pa->num) - atoi(pb->num);
    return strcmp(pa->num, pb->num);
}

static void comp_add_pin(dump_comp_t *c, const char *num, const char *name,
                          const char *type, double ax, double ay)
{
    for (size_t i = 0; i < c->num_pins; i++)
        if (strcmp(c->pins[i].num, num ? num : "") == 0) return;

    if (c->num_pins >= c->cap_pins) {
        c->cap_pins = c->cap_pins ? c->cap_pins * 2 : 8;
        dump_pin_t *tmp = realloc(c->pins, c->cap_pins * sizeof(dump_pin_t));
        if (!tmp) return;
        c->pins = tmp;
    }
    dump_pin_t *p = &c->pins[c->num_pins++];
    memset(p, 0, sizeof(*p));
    snprintf(p->num,  sizeof(p->num),  "%s", num  ? num  : "?");
    snprintf(p->name, sizeof(p->name), "%s", name ? name : "~");
    snprintf(p->type, sizeof(p->type), "%s", type ? type : "");
    p->ax = ax;
    p->ay = ay;
    p->uf_idx = -1;
}

/* ── Hierarchical sheet / label for dump ────────────────────────────────── */

typedef struct {
    char name[64];
    char direction[16];
} dump_sheet_pin_t;

typedef struct {
    char sheetname[128];
    char sheetfile[256];
    dump_sheet_pin_t *pins;
    size_t num_pins;
} dump_sheet_t;

typedef struct {
    char text[256];
    char shape[64];
} dump_hier_label_t;

/* ── Net map: "REF:PIN" → net name (from kicad-cli netlist) ─────────────── */

typedef struct {
    char refpin[192];
    char net[128];
} net_entry_t;

typedef struct {
    net_entry_t *entries;
    size_t       count;
    size_t       cap;
} net_map_t;

static void net_map_free(net_map_t *m)
{
    free(m->entries);
    m->entries = NULL;
    m->count = m->cap = 0;
}

static void net_map_add(net_map_t *m, const char *ref, const char *pin,
                         const char *net)
{
    if (m->count >= m->cap) {
        m->cap = m->cap ? m->cap * 2 : 256;
        net_entry_t *tmp = realloc(m->entries, m->cap * sizeof(net_entry_t));
        if (!tmp) return;
        m->entries = tmp;
    }
    net_entry_t *e = &m->entries[m->count++];
    snprintf(e->refpin, sizeof(e->refpin), "%s:%s", ref, pin);
    snprintf(e->net,    sizeof(e->net),    "%s", net);
}

static const char *net_map_lookup(const net_map_t *m, const char *ref,
                                   const char *pin)
{
    char key[192];
    snprintf(key, sizeof(key), "%s:%s", ref, pin);
    for (size_t i = 0; i < m->count; i++)
        if (strcmp(m->entries[i].refpin, key) == 0)
            return m->entries[i].net;
    return NULL;
}

static int parse_netlist_nets(const char *path, net_map_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { kicli_set_error("cannot open netlist '%s'", path); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); kicli_set_error("out of memory"); return -1; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';

    char errbuf[256];
    sexpr_t *root = sexpr_parse(buf, errbuf, sizeof(errbuf));
    free(buf);
    if (!root) { kicli_set_error("netlist parse: %s", errbuf); return -1; }

    sexpr_t *nets_list = sexpr_get(root, "nets");
    if (nets_list) {
        for (size_t i = 1; i < nets_list->num_children; i++) {
            sexpr_t *net = nets_list->children[i];
            if (!net || net->type != SEXPR_LIST) continue;
            const char *raw = sexpr_atom_value(net, "name");
            char net_name[128];
            clean_net(raw, net_name, sizeof(net_name));

            for (size_t j = 1; j < net->num_children; j++) {
                sexpr_t *node = net->children[j];
                if (!node || node->type != SEXPR_LIST || node->num_children == 0) continue;
                if (!node->children[0]->value ||
                    strcmp(node->children[0]->value, "node") != 0) continue;
                const char *ref = sexpr_atom_value(node, "ref");
                const char *pin = sexpr_atom_value(node, "pin");
                if (ref && pin)
                    net_map_add(out, ref, pin, net_name);
            }
        }
    }
    sexpr_free(root);
    return 0;
}

/* ── Lib symbol pin definitions ──────────────────────────────────────────── */

typedef struct {
    char   num[64];
    char   name[128];
    char   type[48];
    double at_x, at_y;   /* pin endpoint in lib-local coords */
} lib_pin_def_t;

typedef struct {
    char          lib_id[256];
    char          extends[256];
    lib_pin_def_t *pins;
    size_t         num_pins;
    size_t         cap_pins;
} lib_sym_def_t;

static void lib_sym_add_pin(lib_sym_def_t *s, const char *num,
                             const char *name, const char *type,
                             double at_x, double at_y)
{
    for (size_t i = 0; i < s->num_pins; i++)
        if (strcmp(s->pins[i].num, num ? num : "") == 0) return;

    if (s->num_pins >= s->cap_pins) {
        s->cap_pins = s->cap_pins ? s->cap_pins * 2 : 16;
        lib_pin_def_t *tmp = realloc(s->pins, s->cap_pins * sizeof(lib_pin_def_t));
        if (!tmp) return;
        s->pins = tmp;
    }
    lib_pin_def_t *p = &s->pins[s->num_pins++];
    snprintf(p->num,  sizeof(p->num),  "%s", num  ? num  : "?");
    snprintf(p->name, sizeof(p->name), "%s", name ? name : "~");
    snprintf(p->type, sizeof(p->type), "%s", type ? type : "");
    p->at_x = at_x;
    p->at_y = at_y;
}

static void collect_pins_rec(const sexpr_t *node, lib_sym_def_t *s)
{
    if (!node || node->type != SEXPR_LIST || node->num_children == 0) return;
    const char *tag = node->children[0]->value;
    if (!tag) return;

    if (strcmp(tag, "pin") == 0 && node->num_children >= 3) {
        const char *type = node->children[1]->value;
        const char *pnum = NULL, *pname = NULL;
        double px = 0, py = 0;

        for (size_t i = 2; i < node->num_children; i++) {
            sexpr_t *c = node->children[i];
            if (!c || c->type != SEXPR_LIST || c->num_children < 2) continue;
            const char *ctag = c->children[0]->value;
            if (!ctag) continue;
            if (strcmp(ctag, "at") == 0 && c->num_children >= 3) {
                px = c->children[1]->value ? atof(c->children[1]->value) : 0;
                py = c->children[2]->value ? atof(c->children[2]->value) : 0;
            } else if (strcmp(ctag, "number") == 0) {
                pnum  = c->children[1]->value;
            } else if (strcmp(ctag, "name") == 0) {
                pname = c->children[1]->value;
            }
        }
        if (pnum)
            lib_sym_add_pin(s, pnum, pname, type, px, py);
        return;
    }
    for (size_t i = 1; i < node->num_children; i++)
        collect_pins_rec(node->children[i], s);
}

/* ── Placed symbol ───────────────────────────────────────────────────────── */

typedef struct {
    char   ref[64];
    char   value[256];
    char   lib_id[256];
    double at_x, at_y, angle;
    int    mirror_x;
    int    is_virtual;
} placed_sym_t;

typedef struct { double x, y; } nc_pt_t;
typedef struct { double x1,y1,x2,y2; } wire_seg_t;

/* ── Parse .kicad_sch ────────────────────────────────────────────────────── */

static int parse_kicad_sch(const char *path,
                            lib_sym_def_t  **libs_out,   size_t *lib_count_out,
                            placed_sym_t   **placed_out, size_t *placed_count_out,
                            nc_pt_t        **nc_out,     size_t *nc_count_out,
                            wire_seg_t     **wire_out,   size_t *wire_count_out,
                            dump_sheet_t   **sheets_out, size_t *sheet_count_out,
                            dump_hier_label_t **hlbl_out, size_t *hlbl_count_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { kicli_set_error("cannot open '%s'", path); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); kicli_set_error("out of memory"); return -1; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';

    char errbuf[256];
    sexpr_t *root = sexpr_parse(buf, errbuf, sizeof(errbuf));
    free(buf);
    if (!root) { kicli_set_error("sch parse: %s", errbuf); return -1; }

    lib_sym_def_t     *libs   = NULL; size_t lib_count = 0, lib_cap = 0;
    placed_sym_t      *placed = NULL; size_t placed_count = 0, placed_cap = 0;
    nc_pt_t           *nc_pts = NULL; size_t nc_count = 0, nc_cap = 0;
    wire_seg_t        *wires  = NULL; size_t wire_count = 0, wire_cap = 0;
    dump_sheet_t      *sheets = NULL; size_t sheet_count = 0, sheet_cap = 0;
    dump_hier_label_t *hlbls  = NULL; size_t hlbl_count = 0, hlbl_cap = 0;

    /* ── lib_symbols ─────────────────────────────────────────────────── */
    sexpr_t *lib_syms = sexpr_get(root, "lib_symbols");
    if (lib_syms) {
        for (size_t i = 1; i < lib_syms->num_children; i++) {
            sexpr_t *sym = lib_syms->children[i];
            if (!sym || sym->type != SEXPR_LIST || sym->num_children < 2) continue;
            if (!sym->children[0]->value ||
                strcmp(sym->children[0]->value, "symbol") != 0) continue;
            const char *name = sym->children[1]->value;
            if (!name) continue;

            if (lib_count >= lib_cap) {
                lib_cap = lib_cap ? lib_cap * 2 : 32;
                lib_sym_def_t *tmp = realloc(libs, lib_cap * sizeof(lib_sym_def_t));
                if (!tmp) goto oom;
                libs = tmp;
            }
            lib_sym_def_t *ls = &libs[lib_count++];
            memset(ls, 0, sizeof(*ls));
            snprintf(ls->lib_id, sizeof(ls->lib_id), "%s", name);
            const char *ext = sexpr_atom_value(sym, "extends");
            if (ext) snprintf(ls->extends, sizeof(ls->extends), "%s", ext);
            collect_pins_rec(sym, ls);
        }
        /* resolve extends */
        for (size_t i = 0; i < lib_count; i++) {
            if (libs[i].num_pins > 0 || !libs[i].extends[0]) continue;
            for (size_t j = 0; j < lib_count; j++) {
                if (i == j) continue;
                if (strcmp(libs[j].lib_id, libs[i].extends) == 0 &&
                    libs[j].num_pins > 0) {
                    libs[i].cap_pins = libs[j].num_pins;
                    libs[i].pins = malloc(libs[i].cap_pins * sizeof(lib_pin_def_t));
                    if (!libs[i].pins) goto oom;
                    memcpy(libs[i].pins, libs[j].pins,
                           libs[j].num_pins * sizeof(lib_pin_def_t));
                    libs[i].num_pins = libs[j].num_pins;
                    break;
                }
            }
        }
    }

    /* ── Walk root children: NC markers, wires, placed symbols ──────── */
    for (size_t i = 1; i < root->num_children; i++) {
        sexpr_t *c = root->children[i];
        if (!c || c->type != SEXPR_LIST || c->num_children == 0) continue;
        const char *tag = c->children[0]->value;
        if (!tag) continue;

        /* no_connect */
        if (strcmp(tag, "no_connect") == 0) {
            sexpr_t *at = sexpr_get(c, "at");
            if (!at || at->num_children < 3) continue;
            if (nc_count >= nc_cap) {
                nc_cap = nc_cap ? nc_cap * 2 : 32;
                nc_pt_t *tmp = realloc(nc_pts, nc_cap * sizeof(nc_pt_t));
                if (!tmp) goto oom;
                nc_pts = tmp;
            }
            nc_pts[nc_count].x = atof(at->children[1]->value);
            nc_pts[nc_count].y = atof(at->children[2]->value);
            nc_count++;
            continue;
        }

        /* wire */
        if (strcmp(tag, "wire") == 0) {
            sexpr_t *pts = sexpr_get(c, "pts");
            if (!pts) continue;
            double xs[2] = {0}, ys[2] = {0};
            int found = 0;
            for (size_t j = 1; j < pts->num_children && found < 2; j++) {
                sexpr_t *xy = pts->children[j];
                if (!xy || xy->type != SEXPR_LIST || xy->num_children < 3) continue;
                if (!xy->children[0]->value ||
                    strcmp(xy->children[0]->value, "xy") != 0) continue;
                xs[found] = atof(xy->children[1]->value);
                ys[found] = atof(xy->children[2]->value);
                found++;
            }
            if (found < 2) continue;
            if (wire_count >= wire_cap) {
                wire_cap = wire_cap ? wire_cap * 2 : 256;
                wire_seg_t *tmp = realloc(wires, wire_cap * sizeof(wire_seg_t));
                if (!tmp) goto oom;
                wires = tmp;
            }
            wires[wire_count++] = (wire_seg_t){xs[0], ys[0], xs[1], ys[1]};
            continue;
        }

        /* placed symbol */
        if (strcmp(tag, "symbol") == 0) {
            const char *lid = sexpr_atom_value(c, "lib_id");
            if (!lid) continue;
            const char *ref = NULL, *val = NULL;
            for (size_t j = 0; j < c->num_children; j++) {
                sexpr_t *prop = c->children[j];
                if (!prop || prop->type != SEXPR_LIST || prop->num_children < 3) continue;
                if (!prop->children[0]->value ||
                    strcmp(prop->children[0]->value, "property") != 0) continue;
                if (!prop->children[1]->value) continue;
                if (strcmp(prop->children[1]->value, "Reference") == 0)
                    ref = prop->children[2]->value;
                else if (strcmp(prop->children[1]->value, "Value") == 0)
                    val = prop->children[2]->value;
            }
            if (!ref) continue;
            int found = 0;
            for (size_t j = 0; j < placed_count; j++)
                if (strcmp(placed[j].ref, ref) == 0) { found = 1; break; }
            if (found) continue;

            if (placed_count >= placed_cap) {
                placed_cap = placed_cap ? placed_cap * 2 : 32;
                placed_sym_t *tmp = realloc(placed, placed_cap * sizeof(placed_sym_t));
                if (!tmp) goto oom;
                placed = tmp;
            }
            placed_sym_t *ps = &placed[placed_count++];
            memset(ps, 0, sizeof(*ps));
            snprintf(ps->ref,    sizeof(ps->ref),    "%s", ref);
            snprintf(ps->value,  sizeof(ps->value),  "%s", val ? val : "");
            snprintf(ps->lib_id, sizeof(ps->lib_id), "%s", lid);
            sexpr_t *at = sexpr_get(c, "at");
            if (at && at->num_children >= 3) {
                ps->at_x  = atof(at->children[1]->value);
                ps->at_y  = atof(at->children[2]->value);
                ps->angle = at->num_children >= 4 && at->children[3]->value
                            ? atof(at->children[3]->value) : 0;
            }
            sexpr_t *mir = sexpr_get(c, "mirror");
            if (mir && mir->num_children >= 2 && mir->children[1]->value &&
                strcmp(mir->children[1]->value, "x") == 0)
                ps->mirror_x = 1;
            ps->is_virtual = (ref[0] == '#' || strncmp(lid, "power:", 6) == 0);
        }

        /* hierarchical_label */
        if (strcmp(tag, "hierarchical_label") == 0) {
            if (hlbl_count >= hlbl_cap) {
                hlbl_cap = hlbl_cap ? hlbl_cap * 2 : 16;
                dump_hier_label_t *tmp = realloc(hlbls, hlbl_cap * sizeof(dump_hier_label_t));
                if (!tmp) goto oom;
                hlbls = tmp;
            }
            dump_hier_label_t *h = &hlbls[hlbl_count++];
            memset(h, 0, sizeof(*h));
            if (c->num_children >= 2 && c->children[1]->value)
                snprintf(h->text, sizeof(h->text), "%s", c->children[1]->value);
            const char *shape = sexpr_atom_value(c, "shape");
            if (shape) snprintf(h->shape, sizeof(h->shape), "%s", shape);
        }

        /* sheet */
        if (strcmp(tag, "sheet") == 0) {
            if (sheet_count >= sheet_cap) {
                sheet_cap = sheet_cap ? sheet_cap * 2 : 8;
                dump_sheet_t *tmp = realloc(sheets, sheet_cap * sizeof(dump_sheet_t));
                if (!tmp) goto oom;
                sheets = tmp;
            }
            dump_sheet_t *s = &sheets[sheet_count++];
            memset(s, 0, sizeof(*s));

            /* (property "Sheetname" "POWER") / (property "Sheetfile" "power.kicad_sch") */
            for (size_t j = 0; j < c->num_children; j++) {
                sexpr_t *prop = c->children[j];
                if (!prop || prop->type != SEXPR_LIST || prop->num_children < 3) continue;
                if (!prop->children[0]->value || strcmp(prop->children[0]->value, "property") != 0) continue;
                if (!prop->children[1]->value || !prop->children[2]->value) continue;
                if (strcmp(prop->children[1]->value, "Sheetname") == 0)
                    snprintf(s->sheetname, sizeof(s->sheetname), "%s", prop->children[2]->value);
                else if (strcmp(prop->children[1]->value, "Sheetfile") == 0)
                    snprintf(s->sheetfile, sizeof(s->sheetfile), "%s", prop->children[2]->value);
            }

            /* sheet pins: (pin "CAN_TX" input ...) */
            size_t pin_cap = 8;
            s->pins = malloc(pin_cap * sizeof(dump_sheet_pin_t));
            for (size_t j = 0; j < c->num_children; j++) {
                sexpr_t *pin = c->children[j];
                if (!pin || pin->type != SEXPR_LIST || pin->num_children < 3) continue;
                if (!pin->children[0]->value || strcmp(pin->children[0]->value, "pin") != 0) continue;
                if (s->num_pins >= pin_cap) {
                    pin_cap *= 2;
                    dump_sheet_pin_t *tmp = realloc(s->pins, pin_cap * sizeof(dump_sheet_pin_t));
                    if (!tmp) continue;
                    s->pins = tmp;
                }
                dump_sheet_pin_t *sp = &s->pins[s->num_pins++];
                memset(sp, 0, sizeof(*sp));
                if (pin->children[1]->value)
                    snprintf(sp->name, sizeof(sp->name), "%s", pin->children[1]->value);
                if (pin->children[2]->value)
                    snprintf(sp->direction, sizeof(sp->direction), "%s", pin->children[2]->value);
            }
        }
    }

    sexpr_free(root);
    *libs_out         = libs;   *lib_count_out    = lib_count;
    *placed_out       = placed; *placed_count_out = placed_count;
    *nc_out           = nc_pts; *nc_count_out     = nc_count;
    *wire_out         = wires;  *wire_count_out   = wire_count;
    *sheets_out       = sheets; *sheet_count_out  = sheet_count;
    *hlbl_out         = hlbls;  *hlbl_count_out   = hlbl_count;
    return 0;

oom:
    sexpr_free(root);
    for (size_t i = 0; i < lib_count; i++) free(libs[i].pins);
    for (size_t i = 0; i < sheet_count; i++) free(sheets[i].pins);
    free(libs); free(placed); free(nc_pts); free(wires);
    free(sheets); free(hlbls);
    kicli_set_error("out of memory");
    return -1;
}

/* ── Sort helpers ────────────────────────────────────────────────────────── */

static int placed_cmp(const void *a, const void *b)
{
    return strcmp(((const placed_sym_t *)a)->ref,
                  ((const placed_sym_t *)b)->ref);
}

/* ── Write .kisch output ─────────────────────────────────────────────────── */

static void write_kisch(FILE *f, const char *sch_path,
                        const dump_comp_t *comps, size_t count,
                        const dump_sheet_t *sheets, size_t sheet_count,
                        const dump_hier_label_t *hlbls, size_t hlbl_count)
{
    size_t total_pins = 0;
    int w_refpin = 4, w_name = 4, w_type = 4;

    for (size_t i = 0; i < count; i++) {
        const dump_comp_t *c = &comps[i];
        total_pins += c->num_pins;
        for (size_t j = 0; j < c->num_pins; j++) {
            const dump_pin_t *p = &c->pins[j];
            int rp = (int)strlen(c->ref) + 1 + (int)strlen(p->num);
            int nm = (int)strlen(p->name);
            int tp = (int)strlen(short_type(p->type));
            if (rp > w_refpin) w_refpin = rp;
            if (nm > w_name)   w_name   = nm;
            if (tp > w_type)   w_type   = tp;
        }
    }
    w_refpin += 1;
    w_name   += 1;
    w_type   += 1;

    const char *fname = strrchr(sch_path, '/');
#ifdef _WIN32
    const char *fname2 = strrchr(sch_path, '\\');
    if (!fname || (fname2 && fname2 > fname)) fname = fname2;
#endif
    fprintf(f, "# %s  (%zu components, %zu pins)\n",
            fname ? fname + 1 : sch_path, count, total_pins);
    fprintf(f, "# REF:PIN  NAME  TYPE  → NET\n");
    fprintf(f, "# NC=no-connect marker  ~=floating  unlabeled shown as peer list\n");
    fprintf(f, "\n");

    for (size_t i = 0; i < count; i++) {
        const dump_comp_t *c = &comps[i];
        if (c->value[0])
            fprintf(f, "[%s: %s]\n", c->ref, c->value);
        else
            fprintf(f, "[%s]\n", c->ref);

        if (c->num_pins == 0) {
            fprintf(f, "# (no pin definitions found)\n");
        } else {
            for (size_t j = 0; j < c->num_pins; j++) {
                const dump_pin_t *p = &c->pins[j];
                char refpin[128];
                snprintf(refpin, sizeof(refpin), "%s:%s", c->ref, p->num);
                fprintf(f, "%-*s  %-*s  %-*s  → %s\n",
                        w_refpin, refpin,
                        w_name,   p->name,
                        w_type,   short_type(p->type),
                        p->net);
            }
        }
        fprintf(f, "\n");
    }

    /* ── Hierarchical labels (connections to parent sheet) ───────────── */
    if (hlbl_count > 0) {
        fprintf(f, "# Hierarchical labels\n");
        for (size_t i = 0; i < hlbl_count; i++)
            fprintf(f, "HLABEL  %-30s  %s\n", hlbls[i].text, short_type(hlbls[i].shape));
        fprintf(f, "\n");
    }

    /* ── Sub-sheets and their pins ──────────────────────────────────── */
    if (sheet_count > 0) {
        fprintf(f, "# Sub-sheets\n");
        for (size_t i = 0; i < sheet_count; i++) {
            const dump_sheet_t *s = &sheets[i];
            fprintf(f, "[SHEET: %s → %s]\n", s->sheetname, s->sheetfile);
            for (size_t j = 0; j < s->num_pins; j++)
                fprintf(f, "  %-30s  %s\n", s->pins[j].name, short_type(s->pins[j].direction));
            fprintf(f, "\n");
        }
    }
}

/* ── Public: cmd_sch_dump ────────────────────────────────────────────────── */

int cmd_sch_dump(const char *sch_path, int argc, char **argv)
{
    const char *outfile = NULL;
    for (int i = 0; i < argc; i++) {
        if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0)
            && i + 1 < argc)
            outfile = argv[i + 1];
    }

    /* ── Pass 1: parse .kicad_sch ─────────────────────────────────────── */
    lib_sym_def_t     *libs   = NULL; size_t lib_count = 0;
    placed_sym_t      *placed = NULL; size_t placed_count = 0;
    nc_pt_t           *nc_pts = NULL; size_t nc_count = 0;
    wire_seg_t        *wires  = NULL; size_t wire_count = 0;
    dump_sheet_t      *sheets = NULL; size_t sheet_count = 0;
    dump_hier_label_t *hlbls  = NULL; size_t hlbl_count = 0;

    if (parse_kicad_sch(sch_path,
                        &libs, &lib_count,
                        &placed, &placed_count,
                        &nc_pts, &nc_count,
                        &wires, &wire_count,
                        &sheets, &sheet_count,
                        &hlbls, &hlbl_count) != 0) {
        fprintf(stderr, "error: %s\n", kicli_last_error());
        return 1;
    }

    /* ── Pass 2: kicad-cli netlist → labeled nets ─────────────────────── */
    char tmp[512];
#ifdef _WIN32
    char tmpdir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpdir);
    snprintf(tmp, sizeof(tmp), "%skicli_nl_%lu.net",
             tmpdir, (unsigned long)GetCurrentProcessId());
#else
    snprintf(tmp, sizeof(tmp), "/tmp/kicli_nl_%d.net", (int)getpid());
#endif

    const char *nl_args[] = {
        "sch", "export", "netlist",
        "--format", "kicadsexpr",
        "--output", tmp,
        sch_path, NULL
    };

    net_map_t nmap = {NULL, 0, 0};
    kicli_err_t err = kicad_cli_run(nl_args);
    if (err == KICLI_OK) {
        parse_netlist_nets(tmp, &nmap);
        unlink(tmp);
    } else {
        fprintf(stderr, "warning: kicad-cli netlist failed — nets may be incomplete\n");
        unlink(tmp);
    }

    /* ── Sort placed symbols ──────────────────────────────────────────── */
    qsort(placed, placed_count, sizeof(placed_sym_t), placed_cmp);

    /* ── Build comp array with pin positions ──────────────────────────── */
    dump_comp_t *comps = NULL;
    size_t comp_count = 0, comp_cap = 0;

    for (size_t i = 0; i < placed_count; i++) {
        placed_sym_t *ps = &placed[i];
        if (ps->is_virtual) continue;

        lib_sym_def_t *ls = NULL;
        for (size_t j = 0; j < lib_count; j++) {
            if (strcmp(libs[j].lib_id, ps->lib_id) == 0) { ls = &libs[j]; break; }
        }

        if (comp_count >= comp_cap) {
            comp_cap = comp_cap ? comp_cap * 2 : 16;
            dump_comp_t *tmp2 = realloc(comps, comp_cap * sizeof(dump_comp_t));
            if (!tmp2) goto oom;
            comps = tmp2;
        }
        dump_comp_t *c = &comps[comp_count++];
        memset(c, 0, sizeof(*c));
        snprintf(c->ref,   sizeof(c->ref),   "%s", ps->ref);
        snprintf(c->value, sizeof(c->value), "%s", ps->value);

        if (ls) {
            double rad   = ps->angle * (3.14159265358979323846 / 180.0);
            double cos_a = cos(rad), sin_a = sin(rad);

            for (size_t k = 0; k < ls->num_pins; k++) {
                double lx = ps->mirror_x ? -ls->pins[k].at_x : ls->pins[k].at_x;
                double ly = ls->pins[k].at_y;
                /* KiCad schematic: Y-down, lib pin Y is inverted when placed */
                double ax = ps->at_x + lx * cos_a - ly * sin_a;
                double ay = ps->at_y - lx * sin_a - ly * cos_a;
                comp_add_pin(c, ls->pins[k].num, ls->pins[k].name,
                             ls->pins[k].type, ax, ay);
            }
        }
        if (c->num_pins > 1)
            qsort(c->pins, c->num_pins, sizeof(dump_pin_t), pin_cmp);
    }

    /* ── Pass 3: assign nets via union-find ───────────────────────────── */

    /* Build union-find nodes from all wire endpoints */
    uf_node_t *uf_nodes = NULL;
    int uf_count = 0, uf_cap = 0;

    for (size_t i = 0; i < wire_count; i++) {
        char ka[32], kb[32];
        coord_key(ka, sizeof(ka), wires[i].x1, wires[i].y1);
        coord_key(kb, sizeof(kb), wires[i].x2, wires[i].y2);
        int ia = uf_get_or_add(&uf_nodes, &uf_count, &uf_cap, ka);
        int ib = uf_get_or_add(&uf_nodes, &uf_count, &uf_cap, kb);
        if (ia >= 0 && ib >= 0)
            uf_union(uf_nodes, ia, ib);
    }

    /* Assign uf_idx for each pin and build refpin list per root */
    for (size_t i = 0; i < comp_count; i++) {
        dump_comp_t *c = &comps[i];
        for (size_t j = 0; j < c->num_pins; j++) {
            dump_pin_t *p = &c->pins[j];
            char key[32];
            coord_key(key, sizeof(key), p->ax, p->ay);
            int idx = uf_get_or_add(&uf_nodes, &uf_count, &uf_cap, key);
            p->uf_idx = idx;
        }
    }

    /* Build root → comma-separated refpin list for unlabeled wire clusters */
    /* We'll generate these on demand to avoid pre-allocating a huge map */

    /* Assign final net strings */
    for (size_t i = 0; i < comp_count; i++) {
        dump_comp_t *c = &comps[i];
        for (size_t j = 0; j < c->num_pins; j++) {
            dump_pin_t *p = &c->pins[j];

            /* 1. kicad-cli labeled net → use it */
            const char *labeled = net_map_lookup(&nmap, c->ref, p->num);
            if (labeled) {
                snprintf(p->net, sizeof(p->net), "%s", labeled);
                continue;
            }

            /* 2. NC marker at this position */
            int is_nc = 0;
            for (size_t n = 0; n < nc_count; n++) {
                double dx = nc_pts[n].x - p->ax;
                double dy = nc_pts[n].y - p->ay;
                if (dx*dx + dy*dy < 0.01) { is_nc = 1; break; }
            }
            if (is_nc) {
                snprintf(p->net, sizeof(p->net), "NC");
                continue;
            }

            /* 3. Wire-connected but unlabeled: find peers in same UF component */
            if (p->uf_idx < 0) {
                snprintf(p->net, sizeof(p->net), "~");
                continue;
            }
            int root = uf_find(uf_nodes, p->uf_idx);

            /* Collect all other pins in same root (different ref:pin) */
            char peers[256];
            peers[0] = '\0';
            size_t peers_len = 0;
            int peer_count = 0;

            for (size_t ii = 0; ii < comp_count; ii++) {
                dump_comp_t *cc = &comps[ii];
                for (size_t jj = 0; jj < cc->num_pins; jj++) {
                    dump_pin_t *pp = &cc->pins[jj];
                    if (ii == i && jj == j) continue;  /* skip self */
                    if (pp->uf_idx < 0) continue;
                    if (uf_find(uf_nodes, pp->uf_idx) != root) continue;
                    /* same net cluster */
                    char refpin[96];
                    snprintf(refpin, sizeof(refpin), "%s:%s", cc->ref, pp->num);
                    if (peers_len + strlen(refpin) + 3 < sizeof(peers)) {
                        if (peer_count > 0) {
                            strncat(peers, ", ", sizeof(peers) - peers_len - 1);
                            peers_len += 2;
                        }
                        strncat(peers, refpin, sizeof(peers) - peers_len - 1);
                        peers_len += strlen(refpin);
                        peer_count++;
                    }
                }
            }

            if (peer_count > 0)
                snprintf(p->net, sizeof(p->net), "%s", peers);
            else
                snprintf(p->net, sizeof(p->net), "~");
        }
    }

    free(uf_nodes);

    /* ── Output ───────────────────────────────────────────────────────── */
    FILE *f = stdout;
    if (outfile) {
        f = fopen(outfile, "w");
        if (!f) {
            fprintf(stderr, "error: cannot write '%s'\n", outfile);
            goto cleanup;
        }
    }

    write_kisch(f, sch_path, comps, comp_count, sheets, sheet_count, hlbls, hlbl_count);

    if (outfile) {
        fclose(f);
        size_t total = 0;
        for (size_t i = 0; i < comp_count; i++) total += comps[i].num_pins;
        printf("wrote %s  (%zu components, %zu pins)\n",
               outfile, comp_count, total);
    }

cleanup:
    net_map_free(&nmap);
    for (size_t i = 0; i < lib_count; i++) free(libs[i].pins);
    free(libs); free(placed); free(nc_pts); free(wires);
    for (size_t i = 0; i < sheet_count; i++) free(sheets[i].pins);
    free(sheets); free(hlbls);
    for (size_t i = 0; i < comp_count; i++) free(comps[i].pins);
    free(comps);
    return 0;

oom:
    net_map_free(&nmap);
    for (size_t i = 0; i < lib_count; i++) free(libs[i].pins);
    free(libs); free(placed); free(nc_pts); free(wires);
    for (size_t i = 0; i < sheet_count; i++) free(sheets[i].pins);
    free(sheets); free(hlbls);
    for (size_t i = 0; i < comp_count; i++) free(comps[i].pins);
    free(comps);
    kicli_set_error("out of memory");
    return 1;
}
