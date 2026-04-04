/*
 * dump.c — kicli sch FILE dump  →  .kisch format
 *
 * Every pin line is self-contained and grep-friendly:
 *
 *   [U1: CAT24C256]
 *   U1:1   A0    in     → Net-(U1-A0)
 *   U1:5   SDA   bidir  → I2C2.SDA
 *   U1:8   VCC   pwr    → +3V3
 *
 * grep patterns:
 *   grep '^U1:'       all U1 pins
 *   grep '→ VCC'      everything on VCC
 *   grep 'bidir'      bidirectional pins
 *   grep '→ NC'       unconnected pins
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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
/*
 * KiCad appends "+no_connect" to the base type when a no-connect marker
 * is placed on the pin. We match on prefix only.
 */
static const char *short_type(const char *t)
{
    if (!t || !t[0])                            return "-";
    if (strncmp(t, "passive",       7)  == 0)  return "pass";
    if (strncmp(t, "input",         5)  == 0)  return "in";
    if (strncmp(t, "output",        6)  == 0)  return "out";
    if (strncmp(t, "bidirectional", 13) == 0)  return "bidir";
    if (strncmp(t, "tristate",      8)  == 0)  return "tri";
    if (strncmp(t, "power_in",      8)  == 0)  return "pwr";
    if (strncmp(t, "power_out",     9)  == 0)  return "pwr";
    if (strncmp(t, "open_collector",14) == 0)  return "oc";
    if (strncmp(t, "open_emitter",  12) == 0)  return "oe";
    if (strncmp(t, "unspecified",   11) == 0)  return "-";
    if (strncmp(t, "no_connect",    10) == 0)  return "-";
    if (strncmp(t, "free",           4) == 0)  return "free";
    return "-";
}

/* ── Data model ─────────────────────────────────────────────────────────── */

typedef struct {
    char num[32];
    char name[64];
    char type[48];   /* raw type from netlist (may have +no_connect suffix) */
    char net[128];
} kisch_pin_t;

typedef struct {
    char        ref[64];
    char        value[256];
    kisch_pin_t *pins;
    size_t       num_pins;
    size_t       cap_pins;
} kisch_comp_t;

/* ── Helpers ────────────────────────────────────────────────────────────── */

static kisch_comp_t *get_or_add(kisch_comp_t **arr, size_t *count,
                                 size_t *cap, const char *ref)
{
    for (size_t i = 0; i < *count; i++)
        if (strcmp((*arr)[i].ref, ref) == 0) return &(*arr)[i];

    if (*count >= *cap) {
        *cap = *cap ? *cap * 2 : 16;
        kisch_comp_t *tmp = realloc(*arr, *cap * sizeof(kisch_comp_t));
        if (!tmp) return NULL;
        *arr = tmp;
    }
    kisch_comp_t *c = &(*arr)[(*count)++];
    memset(c, 0, sizeof(*c));
    snprintf(c->ref, sizeof(c->ref), "%s", ref);
    return c;
}

static void add_pin(kisch_comp_t *c, const char *num, const char *name,
                    const char *type, const char *net)
{
    /* skip duplicate pins (multi-unit components appear once per unit) */
    for (size_t i = 0; i < c->num_pins; i++)
        if (strcmp(c->pins[i].num, num ? num : "") == 0) return;

    if (c->num_pins >= c->cap_pins) {
        c->cap_pins = c->cap_pins ? c->cap_pins * 2 : 8;
        kisch_pin_t *tmp = realloc(c->pins, c->cap_pins * sizeof(kisch_pin_t));
        if (!tmp) return;
        c->pins = tmp;
    }
    kisch_pin_t *p = &c->pins[c->num_pins++];
    snprintf(p->num,  sizeof(p->num),  "%s", num  ? num  : "?");
    snprintf(p->name, sizeof(p->name), "%s", name ? name : "~");
    snprintf(p->type, sizeof(p->type), "%s", type ? type : "");
    snprintf(p->net,  sizeof(p->net),  "%s", net  ? net  : "NC");
}

/* Strip leading "/" from KiCad net names. Shorten unconnected pseudo-names. */
static void clean_net(const char *in, char *out, size_t sz)
{
    if (!in || !in[0]) { snprintf(out, sz, "NC"); return; }
    if (strncmp(in, "unconnected", 11) == 0) { snprintf(out, sz, "NC"); return; }
    const char *p = in;
    while (*p == '/') p++;
    snprintf(out, sz, "%s", p[0] ? p : "NC");
}

/* Sort pins: numeric first, then lexicographic. */
static int pin_cmp(const void *a, const void *b)
{
    const kisch_pin_t *pa = a, *pb = b;
    int all_a = 1, all_b = 1;
    for (const char *p = pa->num; *p; p++) if (!isdigit((unsigned char)*p)) { all_a = 0; break; }
    for (const char *p = pb->num; *p; p++) if (!isdigit((unsigned char)*p)) { all_b = 0; break; }
    if (all_a && all_b) return atoi(pa->num) - atoi(pb->num);
    return strcmp(pa->num, pb->num);
}

static void free_comps(kisch_comp_t *comps, size_t count)
{
    for (size_t i = 0; i < count; i++) free(comps[i].pins);
    free(comps);
}

/* ── Parse kicadsexpr netlist ────────────────────────────────────────────── */

static int parse_netlist(const char *path,
                         kisch_comp_t **out, size_t *count_out)
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
    if (!root) { kicli_set_error("netlist parse error: %s", errbuf); return -1; }

    kisch_comp_t *comps = NULL;
    size_t count = 0, cap = 0;

    /* 1. (components ...) → ref, value */
    sexpr_t *comp_list = sexpr_get(root, "components");
    if (comp_list) {
        for (size_t i = 1; i < comp_list->num_children; i++) {
            sexpr_t *c = comp_list->children[i];
            if (!c || c->type != SEXPR_LIST) continue;
            const char *ref = sexpr_atom_value(c, "ref");
            if (!ref) continue;
            kisch_comp_t *comp = get_or_add(&comps, &count, &cap, ref);
            if (!comp) goto oom;
            const char *val = sexpr_atom_value(c, "value");
            if (val) snprintf(comp->value, sizeof(comp->value), "%s", val);
        }
    }

    /* 2. (nets ...) → pin assignments */
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
                if (!node || node->type != SEXPR_LIST || !node->num_children) continue;
                if (!node->children[0]->value ||
                    strcmp(node->children[0]->value, "node") != 0) continue;

                const char *ref  = sexpr_atom_value(node, "ref");
                const char *pin  = sexpr_atom_value(node, "pin");
                const char *pfn  = sexpr_atom_value(node, "pinfunction");
                const char *ptyp = sexpr_atom_value(node, "pintype");
                if (!ref) continue;

                kisch_comp_t *comp = get_or_add(&comps, &count, &cap, ref);
                if (!comp) goto oom;
                add_pin(comp, pin, pfn, ptyp, net_name);
            }
        }
    }

    sexpr_free(root);

    for (size_t i = 0; i < count; i++)
        qsort(comps[i].pins, comps[i].num_pins, sizeof(kisch_pin_t), pin_cmp);

    *out = comps;
    *count_out = count;
    return 0;

oom:
    sexpr_free(root);
    free_comps(comps, count);
    kicli_set_error("out of memory");
    return -1;
}

/* ── Output ─────────────────────────────────────────────────────────────── */

static void write_kisch(FILE *f, const char *sch_path,
                        const kisch_comp_t *comps, size_t count)
{
    /* Count total pins and compute file-wide column widths */
    size_t total_pins = 0;
    int w_refpin = 4, w_name = 4, w_type = 4;

    for (size_t i = 0; i < count; i++) {
        const kisch_comp_t *c = &comps[i];
        total_pins += c->num_pins;
        for (size_t j = 0; j < c->num_pins; j++) {
            const kisch_pin_t *p = &c->pins[j];
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

    /* Header */
    const char *fname = strrchr(sch_path, '/');
#ifdef _WIN32
    const char *fname2 = strrchr(sch_path, '\\');
    if (!fname || (fname2 && fname2 > fname)) fname = fname2;
#endif
    fprintf(f, "# %s  (%zu components, %zu pins)\n",
            fname ? fname + 1 : sch_path, count, total_pins);
    fprintf(f, "# REF:PIN  NAME  TYPE  → NET\n");
    fprintf(f, "# grep: '^U1:'  '→ VCC'  'bidir'  '→ NC'\n");
    fprintf(f, "\n");

    for (size_t i = 0; i < count; i++) {
        const kisch_comp_t *c = &comps[i];

        /* Component header (just for context — not a data line) */
        if (c->value[0])
            fprintf(f, "[%s: %s]\n", c->ref, c->value);
        else
            fprintf(f, "[%s]\n", c->ref);

        if (c->num_pins == 0) {
            fprintf(f, "# (no connected pins)\n");
        } else {
            for (size_t j = 0; j < c->num_pins; j++) {
                const kisch_pin_t *p = &c->pins[j];
                char refpin[96];
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

    /* Temp file for kicad-cli netlist output */
    char tmp[512];
#ifdef _WIN32
    char tmpdir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpdir);
    snprintf(tmp, sizeof(tmp), "%skicli_nl_%lu.net",
             tmpdir, (unsigned long)GetCurrentProcessId());
#else
    snprintf(tmp, sizeof(tmp), "/tmp/kicli_nl_%d.net", (int)getpid());
#endif

    const char *args[] = {
        "sch", "export", "netlist",
        "--format", "kicadsexpr",
        "--output", tmp,
        sch_path,
        NULL
    };

    kicli_err_t err = kicad_cli_run(args);
    if (err != KICLI_OK) {
        fprintf(stderr, "error: kicad-cli failed: %s\n", kicli_last_error());
        unlink(tmp);
        return 1;
    }

    kisch_comp_t *comps = NULL;
    size_t count = 0;
    if (parse_netlist(tmp, &comps, &count) != 0) {
        fprintf(stderr, "error: %s\n", kicli_last_error());
        unlink(tmp);
        return 1;
    }
    unlink(tmp);

    FILE *f = stdout;
    if (outfile) {
        f = fopen(outfile, "w");
        if (!f) {
            fprintf(stderr, "error: cannot write '%s'\n", outfile);
            free_comps(comps, count);
            return 1;
        }
    }

    write_kisch(f, sch_path, comps, count);

    if (outfile) {
        fclose(f);
        /* count total pins for summary */
        size_t total = 0;
        for (size_t i = 0; i < count; i++) total += comps[i].num_pins;
        printf("wrote %s  (%zu components, %zu pins)\n", outfile, count, total);
    }

    free_comps(comps, count);
    return 0;
}
