/*
 * sch.c — kicli sch <file> <command>
 *
 * M3 (own parser):  list, info, nets, tree, stats
 * M4 (kicad-cli):   export, erc, upgrade
 * M3+ (kicad-cli+parse): dump → .kisch format
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kicli/sch.h"
#include "kicli/error.h"
#include "kicli/config.h"
#include "kicli/kicad_cli.h"

/* forward declarations */
int cmd_sch_dump   (const char *sch_path, int argc, char **argv);
int cmd_sch_set    (const char *sch_path, const char *ref, const char *field, const char *value);
int cmd_sch_set_all(const char *path, const char *val_match, const char *field, const char *new_val);

#define CLR_RESET  "\x1b[0m"
#define CLR_BOLD   "\x1b[1m"
#define CLR_RED    "\x1b[31m"
#define CLR_CYAN   "\x1b[36m"
#define CLR_DIM    "\x1b[2m"
#define CLR_YELLOW "\x1b[33m"

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void err(const char *msg)
{
    fprintf(stderr, CLR_RED "error:" CLR_RESET " %s\n", msg);
}

/* is ref a power/virtual symbol? (starts with #, or lib_id starts with power:) */
static int is_virtual(const kicli_symbol_t *s)
{
    return s->reference[0] == '#' ||
           strncmp(s->lib_id, "power:", 6) == 0;
}

/* ── list ────────────────────────────────────────────────────────────────── */

/*
 * Output: tab-separated columns, one component per line.
 * Filter out virtual/power symbols (ref starts with #).
 * Format: REF\tVALUE\tLIB_ID\tFOOTPRINT
 */
static int cmd_sch_list(const kicli_schematic_t *sch, int show_all)
{
    for (size_t i = 0; i < sch->num_symbols; i++) {
        const kicli_symbol_t *s = &sch->symbols[i];
        if (!show_all && is_virtual(s)) continue;
        printf("%-8s  %-20s  %-40s  %s\n",
               s->reference,
               s->value     ? s->value     : "",
               s->lib_id,
               s->footprint ? s->footprint : "");
    }
    return 0;
}

/* ── info ────────────────────────────────────────────────────────────────── */

static int cmd_sch_info(const kicli_schematic_t *sch, const char *ref)
{
    const kicli_symbol_t *s = kicli_sch_symbol_by_ref(
        (kicli_schematic_t *)sch, ref);

    if (!s) {
        fprintf(stderr, CLR_RED "error:" CLR_RESET
                " component '%s' not found\n", ref);
        return 1;
    }

    printf(CLR_BOLD "%s" CLR_RESET "\n", s->reference);
    printf("  lib_id:     %s\n", s->lib_id);
    printf("  value:      %s\n", s->value     ? s->value     : "");
    printf("  footprint:  %s\n", s->footprint ? s->footprint : "");
    printf("  datasheet:  %s\n", s->datasheet ? s->datasheet : "");
    printf("  unit:       %u\n", s->unit);
    printf("  in_bom:     %s\n", s->in_bom    ? "yes" : "no");
    printf("  on_board:   %s\n", s->on_board  ? "yes" : "no");
    printf("  position:   (%.3f, %.3f) angle=%.1f\n",
           s->at.x, s->at.y, s->at.angle);

    if (s->num_properties > 0) {
        printf("\n  " CLR_BOLD "Properties:\n" CLR_RESET);
        for (size_t j = 0; j < s->num_properties; j++) {
            const kicli_property_t *p = &s->properties[j];
            if (p->hidden) continue;  /* skip hidden fields */
            printf("    %-20s  %s\n", p->key, p->value ? p->value : "");
        }
    }

    if (s->num_pins > 0) {
        printf("\n  " CLR_BOLD "Pins: " CLR_RESET "%zu\n", s->num_pins);
    }

    return 0;
}

/* ── nets ────────────────────────────────────────────────────────────────── */

static int cmd_sch_nets(const kicli_schematic_t *sch)
{
    if (sch->num_labels == 0 && sch->num_global_labels == 0) {
        printf("(no net labels found)\n");
        return 0;
    }

    if (sch->num_labels > 0) {
        printf(CLR_BOLD "Local nets:\n" CLR_RESET);
        for (size_t i = 0; i < sch->num_labels; i++) {
            printf("  %s\n", sch->labels[i].text);
        }
    }

    if (sch->num_global_labels > 0) {
        printf(CLR_BOLD "Global nets:\n" CLR_RESET);
        for (size_t i = 0; i < sch->num_global_labels; i++) {
            printf("  %-30s  (%s)\n",
                   sch->global_labels[i].text,
                   sch->global_labels[i].shape);
        }
    }

    return 0;
}

/* ── tree ────────────────────────────────────────────────────────────────── */

static int cmd_sch_tree(const kicli_schematic_t *sch)
{
    kicli_sch_print_tree(sch);
    return 0;
}

/* ── stats ───────────────────────────────────────────────────────────────── */

static int cmd_sch_stats(const kicli_schematic_t *sch)
{
    size_t real = 0, virtual_cnt = 0;
    for (size_t i = 0; i < sch->num_symbols; i++) {
        if (is_virtual(&sch->symbols[i])) virtual_cnt++;
        else real++;
    }

    printf("components:    %zu\n", real);
    printf("power/virtual: %zu\n", virtual_cnt);
    printf("wires:         %zu\n", sch->num_wires);
    printf("junctions:     %zu\n", sch->num_junctions);
    printf("net labels:    %zu\n", sch->num_labels);
    printf("global nets:   %zu\n", sch->num_global_labels);

    if (sch->has_title_block && sch->title_block.title[0])
        printf("title:         %s\n", sch->title_block.title);

    return 0;
}

/* ── export (kicad-cli passthrough) ─────────────────────────────────────── */

static int cmd_sch_export(const char *sch_path, int argc, char **argv)
{
    /* argv[0] is the format (pdf/svg/netlist/bom) */
    if (argc < 1) {
        fprintf(stderr, "Usage: kicli sch <file> export <format> [--output FILE]\n");
        fprintf(stderr, "  formats: pdf, svg, netlist, bom\n");
        return 1;
    }

    const char *fmt = argv[0];
    const char *output = NULL;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--output") == 0 || strcmp(argv[i], "-o") == 0)
            output = argv[i + 1];
    }

    /* Build kicad-cli args:
     *   kicad-cli sch export --format pdf --output FILE schematic.kicad_sch */
    const char *args[16];
    int n = 0;
    args[n++] = "sch";
    args[n++] = "export";

    if (strcmp(fmt, "pdf") == 0) {
        args[n++] = "pdf";
    } else if (strcmp(fmt, "svg") == 0) {
        args[n++] = "svg";
    } else if (strcmp(fmt, "netlist") == 0) {
        args[n++] = "netlist";
    } else if (strcmp(fmt, "bom") == 0) {
        /* BOM uses different kicad-cli sub-command */
        args[n - 1] = "bom";
    } else {
        fprintf(stderr, CLR_RED "error:" CLR_RESET
                " unknown export format '%s'\n", fmt);
        fprintf(stderr, "  supported: pdf, svg, netlist, bom\n");
        return 1;
    }

    if (output) {
        args[n++] = "--output";
        args[n++] = output;
    }

    args[n++] = sch_path;
    args[n]   = NULL;

    kicli_err_t rc = kicad_cli_run(args);
    if (rc != KICLI_OK) {
        err(kicli_last_error());
        return 1;
    }
    return 0;
}

/* ── erc (kicad-cli passthrough) ────────────────────────────────────────── */

static int cmd_sch_erc(const char *sch_path, int argc, char **argv)
{
    const char *output = NULL;
    for (int i = 0; i < argc - 1; i++) {
        if (strcmp(argv[i], "--output") == 0 || strcmp(argv[i], "-o") == 0)
            output = argv[i + 1];
    }

    const char *args[8];
    int n = 0;
    args[n++] = "sch";
    args[n++] = "erc";
    if (output) {
        args[n++] = "--output";
        args[n++] = output;
    }
    args[n++] = sch_path;
    args[n]   = NULL;

    kicli_err_t rc = kicad_cli_run(args);
    return (rc == KICLI_OK) ? 0 : 1;
}

/* ── upgrade (kicad-cli passthrough) ────────────────────────────────────── */

static int cmd_sch_upgrade(const char *sch_path)
{
    const char *args[] = { "sch", "upgrade", sch_path, NULL };
    kicli_err_t rc = kicad_cli_run(args);
    return (rc == KICLI_OK) ? 0 : 1;
}

/* ── cmd_sch (dispatcher) ─────────────────────────────────────────────────── */

int cmd_sch(int argc, char **argv, const kicli_config_t *cfg)
{
    (void)cfg;

    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        printf("Usage: kicli sch <file> <command> [args]\n");
        printf("       kicli sch <dir>  set-all ...   (bulk operation across all .kicad_sch)\n\n");
        printf(CLR_BOLD "Read:\n" CLR_RESET);
        printf("  list [--all]           List components — tab-separated: REF VALUE LIB FOOTPRINT\n");
        printf("                         --all includes power/virtual symbols (ref starts with #)\n");
        printf("  info <REF>             All properties of a single component\n");
        printf("  nets                   Local and global net labels\n");
        printf("  tree                   Hierarchical schematic structure\n");
        printf("  stats                  Component/wire/net/junction counts\n");
        printf("  dump [-o file.kisch]   Full pin+net connectivity table (.kisch format)\n");
        printf("\n" CLR_BOLD "Write:\n" CLR_RESET);
        printf("  set <REF> <FIELD> <VALUE>           Set one component's property\n");
        printf("    e.g. kicli sch board.kicad_sch set R1 LCSC C25744\n");
        printf("  set-all <VALUE_MATCH> <FIELD> <NEW> Bulk-set field on all matching components\n");
        printf("    e.g. kicli sch project/ set-all \"100nF\" LCSC C1525\n");
        printf("\n" CLR_BOLD "Export (kicad-cli passthrough):\n" CLR_RESET);
        printf("  export pdf|svg|netlist|bom [-o FILE]\n");
        printf("  erc [-o FILE]          Electrical rules check\n");
        printf("  upgrade                Upgrade schematic format\n");
        return 0;
    }

    /* argv[1] is the schematic file, argv[2] is the command */
    const char *sch_path = argv[1];

    if (argc < 3) {
        fprintf(stderr, CLR_RED "error:" CLR_RESET
                " missing command. Run 'kicli sch --help'\n");
        return 1;
    }

    const char *subcmd = argv[2];

    /* kicad-cli passthrough / dump — no sexpr parse needed */
    if (strcmp(subcmd, "export")  == 0) return cmd_sch_export(sch_path, argc - 3, argv + 3);
    if (strcmp(subcmd, "erc")     == 0) return cmd_sch_erc(sch_path, argc - 3, argv + 3);
    if (strcmp(subcmd, "upgrade") == 0) return cmd_sch_upgrade(sch_path);
    if (strcmp(subcmd, "dump")    == 0) return cmd_sch_dump(sch_path, argc - 3, argv + 3);

    /* set <ref> <field> <value> */
    if (strcmp(subcmd, "set") == 0) {
        if (argc < 6) {
            fprintf(stderr, "Usage: kicli sch <file> set <REF> <FIELD> <VALUE>\n");
            return 1;
        }
        return cmd_sch_set(sch_path, argv[3], argv[4], argv[5]);
    }

    /* set-all <value_match> <field> <new_value>  — works on file or directory */
    if (strcmp(subcmd, "set-all") == 0) {
        if (argc < 6) {
            fprintf(stderr, "Usage: kicli sch <file|dir> set-all <VALUE> <FIELD> <NEW_VALUE>\n");
            fprintf(stderr, "  Sets FIELD=NEW_VALUE on all components with matching VALUE\n");
            fprintf(stderr, "  If <dir>, applies to all .kicad_sch files in that directory\n");
            return 1;
        }
        return cmd_sch_set_all(sch_path, argv[3], argv[4], argv[5]);
    }

    /* M5 stubs */
    if (strcmp(subcmd, "add") == 0 || strcmp(subcmd, "remove") == 0 ||
        strcmp(subcmd, "move") == 0 || strcmp(subcmd, "connect") == 0 ||
        strcmp(subcmd, "rename") == 0) {
        fprintf(stderr, CLR_YELLOW "not yet implemented:" CLR_RESET
                " sch write commands coming in M5\n");
        return 1;
    }

    /* Parse the schematic for read commands */
    kicli_schematic_t *sch = NULL;
    kicli_err_t rc = kicli_sch_read(sch_path, &sch);
    if (rc != KICLI_OK) {
        fprintf(stderr, CLR_RED "error:" CLR_RESET " %s\n", kicli_last_error());
        return 1;
    }

    int ret = 0;

    if (strcmp(subcmd, "list") == 0) {
        int show_all = (argc > 3 && strcmp(argv[3], "--all") == 0);
        ret = cmd_sch_list(sch, show_all);
    } else if (strcmp(subcmd, "info") == 0) {
        if (argc < 4) {
            fprintf(stderr, CLR_RED "error:" CLR_RESET " 'info' requires a REF\n");
            ret = 1;
        } else {
            ret = cmd_sch_info(sch, argv[3]);
        }
    } else if (strcmp(subcmd, "nets")  == 0) {
        ret = cmd_sch_nets(sch);
    } else if (strcmp(subcmd, "tree")  == 0) {
        ret = cmd_sch_tree(sch);
    } else if (strcmp(subcmd, "stats") == 0) {
        ret = cmd_sch_stats(sch);
    } else {
        fprintf(stderr, CLR_RED "error:" CLR_RESET
                " unknown sch command '%s'\n", subcmd);
        fprintf(stderr, "Run 'kicli sch --help' for usage.\n");
        ret = 1;
    }

    kicli_sch_free(sch);
    return ret;
}
