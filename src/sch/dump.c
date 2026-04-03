/*
 * dump.c — kicli sch FILE dump
 *
 * Exports .kisch format: component-centric, pin-explicit, agent-friendly.
 *
 *   [U1: NRF52840]
 *     1     VDD           power_in      → VCC
 *     21    NRST          input         → NRST
 *     29    SWDIO         bidir         → SWDIO
 *
 * Usage:
 *   kicli sch board.kicad_sch dump              stdout
 *   kicli sch board.kicad_sch dump -o out.kisch file
 *
 * Implementation:
 *   1. Run kicad-cli to export kicadsexpr netlist → temp file
 *   2. Parse with our sexpr_parse (same parser used for .kicad_sch)
 *   3. Build component → pins[] map from (components) and (nets) sections
 *   4. Sort pins numerically, output .kisch
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  define F_OK 0
#  define access _access
#  define unlink _unlink
#  define getpid GetCurrentProcessId
#else
#  include <unistd.h>
#endif

#include "kicli/sch.h"
#include "kicli/kicad_cli.h"
#include "kicli/error.h"

/* ── Data model ──────────────────────────────────────────────────────────── */

typedef struct {
    char num[32];    /* pin number (may be alpha: "A1", "GND") */
    char name[64];   /* pin function name */
    char type[32];   /* pin type: passive, input, output, bidir, power_in … */
    char net[128];   /* net name, or "NC" */
} kisch_pin_t;

typedef struct {
    char        ref[64];
    char        value[256];
    kisch_pin_t *pins;
    size_t       num_pins;
    size_t       cap_pins;
} kisch_comp_t;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Find existing component by ref, or append a new one. */
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

/* Strip leading "/" from KiCad net names. Shorten unconnected names. */
static void clean_net(const char *in, char *out, size_t sz)
{
    if (!in || !in[0]) { snprintf(out, sz, "NC"); return; }
    if (strncmp(in, "unconnected", 11) == 0) { snprintf(out, sz, "NC"); return; }
    const char *p = in;
    while (*p == '/') p++;        /* strip leading slashes */
    snprintf(out, sz, "%s", p[0] ? p : "NC");
}

/* Pin sort: numeric first, then lexicographic fallback. */
static int pin_cmp(const void *a, const void *b)
{
    const kisch_pin_t *pa = a, *pb = b;

    /* Check if both are purely numeric */
    int all_a = 1, all_b = 1;
    for (const char *p = pa->num; *p; p++) if (!isdigit((unsigned char)*p)) { all_a = 0; break; }
    for (const char *p = pb->num; *p; p++) if (!isdigit((unsigned char)*p)) { all_b = 0; break; }

    if (all_a && all_b) {
        int na = atoi(pa->num), nb = atoi(pb->num);
        return na - nb;
    }
    return strcmp(pa->num, pb->num);
}

/* Free component array */
static void free_comps(kisch_comp_t *comps, size_t count)
{
    for (size_t i = 0; i < count; i++) free(comps[i].pins);
    free(comps);
}

/* ── Netlist → kisch_comp_t[] ────────────────────────────────────────────── */

static int parse_netlist(const char *path,
                         kisch_comp_t **out, size_t *count_out)
{
    /* Read file */
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

    /* 1. (components (comp (ref "X") (value "Y") ...) ...) */
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

    /* 2. (nets (net (name "N") (node (ref "X") (pin "1") ...) ...) ...) */
    sexpr_t *nets_list = sexpr_get(root, "nets");
    if (nets_list) {
        for (size_t i = 1; i < nets_list->num_children; i++) {
            sexpr_t *net = nets_list->children[i];
            if (!net || net->type != SEXPR_LIST) continue;

            const char *raw_name = sexpr_atom_value(net, "name");
            char net_name[128];
            clean_net(raw_name, net_name, sizeof(net_name));

            /* walk child nodes */
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

    /* Sort each component's pins */
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

/* ── Output ──────────────────────────────────────────────────────────────── */

static void write_kisch(FILE *f, const char *sch_path,
                        const kisch_comp_t *comps, size_t count)
{
    /* Header */
    const char *fname = strrchr(sch_path, '/');
#ifdef _WIN32
    const char *fname2 = strrchr(sch_path, '\\');
    if (!fname || (fname2 && fname2 > fname)) fname = fname2;
#endif
    fprintf(f, "# %s\n", fname ? fname + 1 : sch_path);
    fprintf(f, "# .kisch — kicli schematic text format\n");
    fprintf(f, "#\n");
    fprintf(f, "# Columns: pin_num  pin_name  pin_type  → net\n");
    fprintf(f, "# grep examples:\n");
    fprintf(f, "#   grep '→ VCC'        find everything on VCC\n");
    fprintf(f, "#   grep '^\\[U1'        show U1 block\n");
    fprintf(f, "#   grep 'power_in'     find all power pins\n");
    fprintf(f, "#   grep '→ NC'         find unconnected pins\n");
    fprintf(f, "\n");

    for (size_t i = 0; i < count; i++) {
        const kisch_comp_t *c = &comps[i];

        /* Component header */
        if (c->value[0])
            fprintf(f, "[%s: %s]\n", c->ref, c->value);
        else
            fprintf(f, "[%s]\n", c->ref);

        if (c->num_pins == 0) {
            fprintf(f, "  (no connected pins)\n");
        } else {
            /* Compute column widths for this component */
            int w_num = 4, w_name = 6, w_type = 7;
            for (size_t j = 0; j < c->num_pins; j++) {
                int n = (int)strlen(c->pins[j].num);
                int k = (int)strlen(c->pins[j].name);
                int t = (int)strlen(c->pins[j].type);
                if (n > w_num)  w_num  = n;
                if (k > w_name) w_name = k;
                if (t > w_type) w_type = t;
            }
            w_num  += 2;
            w_name += 2;
            w_type += 2;

            for (size_t j = 0; j < c->num_pins; j++) {
                const kisch_pin_t *p = &c->pins[j];
                fprintf(f, "  %-*s  %-*s  %-*s  → %s\n",
                        w_num,  p->num,
                        w_name, p->name,
                        w_type, p->type,
                        p->net);
            }
        }
        fprintf(f, "\n");
    }
}

/* ── Public: cmd_sch_dump ────────────────────────────────────────────────── */

int cmd_sch_dump(const char *sch_path, int argc, char **argv)
{
    /* Parse -o / --output flag */
    const char *outfile = NULL;
    for (int i = 0; i < argc; i++) {
        if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0)
            && i + 1 < argc)
        {
            outfile = argv[i + 1];
        }
    }

    /* Temp file for netlist */
    char tmp[512];
#ifdef _WIN32
    char tmpdir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpdir);
    snprintf(tmp, sizeof(tmp), "%skicli_nl_%lu.net",
             tmpdir, (unsigned long)GetCurrentProcessId());
#else
    snprintf(tmp, sizeof(tmp), "/tmp/kicli_nl_%d.net", (int)getpid());
#endif

    /* Export netlist via kicad-cli */
    const char *args[] = {
        "sch", "export", "netlist",
        "--format", "kicadsexpr",
        "--output", tmp,
        sch_path,
        NULL
    };

    kicli_err_t err = kicad_cli_run(args);
    if (err != KICLI_OK) {
        fprintf(stderr, "error: kicad-cli netlist export failed: %s\n",
                kicli_last_error());
        unlink(tmp);
        return 1;
    }

    /* Parse netlist */
    kisch_comp_t *comps = NULL;
    size_t count = 0;
    if (parse_netlist(tmp, &comps, &count) != 0) {
        fprintf(stderr, "error: %s\n", kicli_last_error());
        unlink(tmp);
        return 1;
    }
    unlink(tmp);

    /* Write output */
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
        printf("wrote %s (%zu components)\n", outfile, count);
    }

    free_comps(comps, count);
    return 0;
}
