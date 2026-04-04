/*
 * sch.c -- kicli sch <file> <command>
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
#define CLR_RED    "\x1b[31m"
#define CLR_YELLOW "\x1b[33m"

/* ── JSON helpers ────────────────────────────────────────────────────────── */

/* Write a JSON-escaped string (without surrounding quotes) */
static void json_str(FILE *f, const char *s)
{
    if (!s) { fputs("\"\"", f); return; }
    fputc('"', f);
    for (; *s; s++) {
        switch (*s) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n",  f); break;
        case '\r': fputs("\\r",  f); break;
        case '\t': fputs("\\t",  f); break;
        default:
            if ((unsigned char)*s < 0x20)
                fprintf(f, "\\u%04x", (unsigned char)*s);
            else
                fputc(*s, f);
        }
    }
    fputc('"', f);
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

static int is_virtual(const kicli_symbol_t *s)
{
    return s->reference[0] == '#' ||
           strncmp(s->lib_id, "power:", 6) == 0;
}

/* ── list ────────────────────────────────────────────────────────────────── */

static int cmd_sch_list(const kicli_schematic_t *sch, int show_all, int json)
{
    if (json) {
        printf("[");
        int first = 1;
        for (size_t i = 0; i < sch->num_symbols; i++) {
            const kicli_symbol_t *s = &sch->symbols[i];
            if (!show_all && is_virtual(s)) continue;
            if (!first) printf(",");
            first = 0;
            printf("\n  {\"ref\":");
            json_str(stdout, s->reference);
            printf(",\"value\":");
            json_str(stdout, s->value ? s->value : "");
            printf(",\"lib\":");
            json_str(stdout, s->lib_id);
            printf(",\"footprint\":");
            json_str(stdout, s->footprint ? s->footprint : "");
            printf(",\"in_bom\":%s}", s->in_bom ? "true" : "false");
        }
        printf("\n]\n");
    } else {
        for (size_t i = 0; i < sch->num_symbols; i++) {
            const kicli_symbol_t *s = &sch->symbols[i];
            if (!show_all && is_virtual(s)) continue;
            printf("%-8s  %-20s  %-40s  %s\n",
                   s->reference,
                   s->value     ? s->value     : "",
                   s->lib_id,
                   s->footprint ? s->footprint : "");
        }
    }
    return 0;
}

/* ── info ────────────────────────────────────────────────────────────────── */

static int cmd_sch_info(const kicli_schematic_t *sch, const char *ref, int json)
{
    const kicli_symbol_t *s = kicli_sch_symbol_by_ref(
        (kicli_schematic_t *)sch, ref);

    if (!s) {
        if (json)
            fprintf(stderr, "{\"error\":\"NOT_FOUND\",\"ref\":\"%s\"}\n", ref);
        else
            fprintf(stderr, CLR_RED "error:" CLR_RESET
                    " component '%s' not found\n", ref);
        return 2;
    }

    if (json) {
        printf("{\"ref\":");
        json_str(stdout, s->reference);
        printf(",\"lib\":");
        json_str(stdout, s->lib_id);
        printf(",\"value\":");
        json_str(stdout, s->value ? s->value : "");
        printf(",\"footprint\":");
        json_str(stdout, s->footprint ? s->footprint : "");
        printf(",\"datasheet\":");
        json_str(stdout, s->datasheet ? s->datasheet : "");
        printf(",\"unit\":%u,\"in_bom\":%s,\"on_board\":%s",
               s->unit,
               s->in_bom   ? "true" : "false",
               s->on_board ? "true" : "false");
        printf(",\"position\":{\"x\":%.3f,\"y\":%.3f,\"angle\":%.1f}",
               s->at.x, s->at.y, s->at.angle);
        printf(",\"properties\":{");
        int first = 1;
        for (size_t j = 0; j < s->num_properties; j++) {
            const kicli_property_t *p = &s->properties[j];
            if (!first) printf(",");
            first = 0;
            json_str(stdout, p->key);
            printf(":");
            json_str(stdout, p->value ? p->value : "");
        }
        printf("}}\n");
    } else {
        printf("%s\n", s->reference);
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
            printf("\n  Properties:\n");
            for (size_t j = 0; j < s->num_properties; j++) {
                const kicli_property_t *p = &s->properties[j];
                if (p->hidden) continue;
                printf("    %-20s  %s\n", p->key, p->value ? p->value : "");
            }
        }

        if (s->num_pins > 0)
            printf("\n  Pins: %zu\n", s->num_pins);
    }

    return 0;
}

/* ── nets ────────────────────────────────────────────────────────────────── */

static int cmd_sch_nets(const kicli_schematic_t *sch, int json)
{
    if (json) {
        printf("{\"local\":[");
        for (size_t i = 0; i < sch->num_labels; i++) {
            if (i) printf(",");
            json_str(stdout, sch->labels[i].text);
        }
        printf("],\"global\":[");
        for (size_t i = 0; i < sch->num_global_labels; i++) {
            if (i) printf(",");
            printf("{\"name\":");
            json_str(stdout, sch->global_labels[i].text);
            printf(",\"shape\":");
            json_str(stdout, sch->global_labels[i].shape);
            printf("}");
        }
        printf("]}\n");
    } else {
        if (sch->num_labels == 0 && sch->num_global_labels == 0) {
            printf("(no net labels found)\n");
            return 0;
        }
        if (sch->num_labels > 0) {
            printf("Local nets:\n");
            for (size_t i = 0; i < sch->num_labels; i++)
                printf("  %s\n", sch->labels[i].text);
        }
        if (sch->num_global_labels > 0) {
            printf("Global nets:\n");
            for (size_t i = 0; i < sch->num_global_labels; i++)
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

static int cmd_sch_stats(const kicli_schematic_t *sch, int json)
{
    size_t real = 0, virtual_cnt = 0;
    for (size_t i = 0; i < sch->num_symbols; i++) {
        if (is_virtual(&sch->symbols[i])) virtual_cnt++;
        else real++;
    }

    if (json) {
        printf("{\"components\":%zu,\"power_virtual\":%zu,"
               "\"wires\":%zu,\"junctions\":%zu,"
               "\"net_labels\":%zu,\"global_nets\":%zu",
               real, virtual_cnt,
               sch->num_wires, sch->num_junctions,
               sch->num_labels, sch->num_global_labels);
        if (sch->has_title_block && sch->title_block.title[0]) {
            printf(",\"title\":");
            json_str(stdout, sch->title_block.title);
        }
        printf("}\n");
    } else {
        printf("components:    %zu\n", real);
        printf("power/virtual: %zu\n", virtual_cnt);
        printf("wires:         %zu\n", sch->num_wires);
        printf("junctions:     %zu\n", sch->num_junctions);
        printf("net labels:    %zu\n", sch->num_labels);
        printf("global nets:   %zu\n", sch->num_global_labels);
        if (sch->has_title_block && sch->title_block.title[0])
            printf("title:         %s\n", sch->title_block.title);
    }
    return 0;
}

/* ── export (kicad-cli passthrough) ──────────────────────────────────────── */

static int cmd_sch_export(const char *sch_path, int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "Usage: kicli sch <file> export <format> [-o FILE]\n");
        fprintf(stderr, "  formats: pdf, svg, netlist, bom\n");
        return 1;
    }

    const char *fmt = argv[0];
    const char *output = NULL;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--output") == 0 || strcmp(argv[i], "-o") == 0)
            output = argv[i + 1];
    }

    const char *args[16];
    int n = 0;
    args[n++] = "sch";
    args[n++] = "export";

    if (strcmp(fmt, "pdf") == 0)         args[n++] = "pdf";
    else if (strcmp(fmt, "svg") == 0)    args[n++] = "svg";
    else if (strcmp(fmt, "netlist") == 0) args[n++] = "netlist";
    else if (strcmp(fmt, "bom") == 0)    args[n - 1] = "bom";
    else {
        fprintf(stderr, CLR_RED "error:" CLR_RESET
                " unknown export format '%s'\n", fmt);
        fprintf(stderr, "  supported: pdf, svg, netlist, bom\n");
        return 1;
    }

    if (output) { args[n++] = "--output"; args[n++] = output; }
    args[n++] = sch_path;
    args[n]   = NULL;

    kicli_err_t rc = kicad_cli_run(args);
    if (rc != KICLI_OK) {
        fprintf(stderr, CLR_RED "error:" CLR_RESET " %s\n", kicli_last_error());
        return 1;
    }
    return 0;
}

/* ── erc (kicad-cli passthrough) ─────────────────────────────────────────── */

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
    if (output) { args[n++] = "--output"; args[n++] = output; }
    args[n++] = sch_path;
    args[n]   = NULL;

    kicli_err_t rc = kicad_cli_run(args);
    return (rc == KICLI_OK) ? 0 : 1;
}

/* ── upgrade (kicad-cli passthrough) ─────────────────────────────────────── */

static int cmd_sch_upgrade(const char *sch_path)
{
    const char *args[] = { "sch", "upgrade", sch_path, NULL };
    kicli_err_t rc = kicad_cli_run(args);
    return (rc == KICLI_OK) ? 0 : 1;
}

/* ── cmd_sch (dispatcher) ────────────────────────────────────────────────── */

int cmd_sch(int argc, char **argv, const kicli_config_t *cfg)
{
    (void)cfg;

    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        printf("Usage: kicli sch <file> <command> [args]\n");
        printf("       kicli sch <dir>  set-all ...   (bulk across all .kicad_sch)\n\n");
        printf("Flags:\n");
        printf("  --json                 Output as JSON (list, info, nets, stats)\n");
        printf("\nRead:\n");
        printf("  list [--all]           Tab-separated: REF VALUE LIB FOOTPRINT\n");
        printf("                         --all includes power/virtual symbols\n");
        printf("  info <REF>             All properties of a component\n");
        printf("  nets                   Local and global net labels\n");
        printf("  tree                   Hierarchical schematic structure\n");
        printf("  stats                  Component/wire/net/junction counts\n");
        printf("  dump [-o file.kisch]   Full pin+net connectivity table\n");
        printf("\nWrite:\n");
        printf("  set <REF> <FIELD> <VALUE>           Set one component's property\n");
        printf("    e.g. kicli sch board.kicad_sch set R1 LCSC C25744\n");
        printf("  set-all <VALUE_MATCH> <FIELD> <NEW> Bulk-set field on all matching\n");
        printf("    e.g. kicli sch project/ set-all \"100nF\" LCSC C1525\n");
        printf("\nExport (kicad-cli passthrough):\n");
        printf("  export pdf|svg|netlist|bom [-o FILE]\n");
        printf("  erc [-o FILE]          Electrical rules check\n");
        printf("  upgrade                Upgrade schematic format\n");
        return 0;
    }

    const char *sch_path = argv[1];

    if (argc < 3) {
        fprintf(stderr, CLR_RED "error:" CLR_RESET
                " missing command. Run 'kicli sch --help'\n");
        return 1;
    }

    /* scan for --json flag anywhere after argv[2] */
    int json = 0;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) { json = 1; break; }
    }

    const char *subcmd = argv[2];

    if (strcmp(subcmd, "export")  == 0) return cmd_sch_export(sch_path, argc - 3, argv + 3);
    if (strcmp(subcmd, "erc")     == 0) return cmd_sch_erc(sch_path, argc - 3, argv + 3);
    if (strcmp(subcmd, "upgrade") == 0) return cmd_sch_upgrade(sch_path);
    if (strcmp(subcmd, "dump")    == 0) return cmd_sch_dump(sch_path, argc - 3, argv + 3);

    if (strcmp(subcmd, "set") == 0) {
        if (argc < 6) {
            fprintf(stderr, "Usage: kicli sch <file> set <REF> <FIELD> <VALUE>\n");
            return 1;
        }
        return cmd_sch_set(sch_path, argv[3], argv[4], argv[5]);
    }

    if (strcmp(subcmd, "set-all") == 0) {
        if (argc < 6) {
            fprintf(stderr, "Usage: kicli sch <file|dir> set-all <VALUE> <FIELD> <NEW_VALUE>\n");
            fprintf(stderr, "  If <dir>, applies to all .kicad_sch files in that directory\n");
            return 1;
        }
        return cmd_sch_set_all(sch_path, argv[3], argv[4], argv[5]);
    }

    if (strcmp(subcmd, "add") == 0 || strcmp(subcmd, "remove") == 0 ||
        strcmp(subcmd, "move") == 0 || strcmp(subcmd, "connect") == 0 ||
        strcmp(subcmd, "rename") == 0) {
        fprintf(stderr, CLR_YELLOW "not yet implemented:" CLR_RESET
                " '%s' is planned for a future release\n", subcmd);
        return 1;
    }

    kicli_schematic_t *sch = NULL;
    kicli_err_t rc = kicli_sch_read(sch_path, &sch);
    if (rc != KICLI_OK) {
        if (json)
            fprintf(stderr, "{\"error\":\"%s\",\"message\":\"%s\"}\n",
                    rc == KICLI_ERR_IO ? "IO_ERROR" : "PARSE_ERROR",
                    kicli_last_error());
        else
            fprintf(stderr, CLR_RED "error:" CLR_RESET " %s\n", kicli_last_error());
        return rc == KICLI_ERR_IO ? 3 : 4;
    }

    int ret = 0;

    if (strcmp(subcmd, "list") == 0) {
        int show_all = 0;
        for (int i = 3; i < argc; i++)
            if (strcmp(argv[i], "--all") == 0) { show_all = 1; break; }
        ret = cmd_sch_list(sch, show_all, json);
    } else if (strcmp(subcmd, "info") == 0) {
        if (argc < 4) {
            fprintf(stderr, CLR_RED "error:" CLR_RESET " 'info' requires a REF\n");
            ret = 1;
        } else {
            ret = cmd_sch_info(sch, argv[3], json);
        }
    } else if (strcmp(subcmd, "nets")  == 0) {
        ret = cmd_sch_nets(sch, json);
    } else if (strcmp(subcmd, "tree")  == 0) {
        ret = cmd_sch_tree(sch);
    } else if (strcmp(subcmd, "stats") == 0) {
        ret = cmd_sch_stats(sch, json);
    } else {
        fprintf(stderr, CLR_RED "error:" CLR_RESET
                " unknown sch command '%s'\n", subcmd);
        fprintf(stderr, "Run 'kicli sch --help' for usage.\n");
        ret = 1;
    }

    kicli_sch_free(sch);
    return ret;
}
