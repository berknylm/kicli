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
#include "kicli/portable.h"

/* forward declarations */
int cmd_sch_view   (const char *sch_path, int argc, char **argv);
int cmd_sch_view_dir(const char *dir, int argc, char **argv);
int cmd_sch_set    (const char *sch_path, const char *ref, const char *field, const char *value);
int cmd_sch_set_all(const char *path, int argc, char **argv);

#define CLR_RESET  "\x1b[0m"
#define CLR_RED    "\x1b[31m"
#define CLR_YELLOW "\x1b[33m"

/* ── helpers ─────────────────────────────────────────────────────────────── */

static int is_virtual(const kicli_symbol_t *s)
{
    return s->reference[0] == '#' ||
           strncmp(s->lib_id, "power:", 6) == 0;
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

static const char *find_lcsc(const kicli_symbol_t *s)
{
    for (size_t i = 0; i < s->num_properties; i++) {
        const char *k = s->properties[i].key;
        if (strcmp(k, "LCSC") == 0 || strcmp(k, "lcsc") == 0 ||
            strcmp(k, "JLC_PART") == 0 || strcmp(k, "JLCPCB#") == 0)
            return s->properties[i].value ? s->properties[i].value : "";
    }
    return "";
}

/* ── list ────────────────────────────────────────────────────────────────── */

static int cmd_sch_list_prefixed(const kicli_schematic_t *sch, int show_all,
                                  const char *sheet_prefix)
{
    for (size_t i = 0; i < sch->num_symbols; i++) {
        const kicli_symbol_t *s = &sch->symbols[i];
        if (!show_all && is_virtual(s)) continue;
        const char *lcsc = find_lcsc(s);
        /* Tab-separated: REF \t VALUE \t LIB \t FOOTPRINT \t PartNo [\t SHEET] */
        printf("%s\t%s\t%s\t%s\t%s",
               s->reference,
               s->value     ? s->value     : "",
               s->lib_id,
               s->footprint ? s->footprint : "",
               lcsc[0] ? lcsc : "(unset)");
        if (sheet_prefix && sheet_prefix[0])
            printf("\t%s", sheet_prefix);
        printf("\n");
    }
    return 0;
}


/* ── info ────────────────────────────────────────────────────────────────── */

static const char *pin_type_str(kicli_pin_type_t t)
{
    switch (t) {
        case PIN_INPUT:       return "in";
        case PIN_OUTPUT:      return "out";
        case PIN_BIDI:        return "inout";
        case PIN_TRISTATE:    return "tri";
        case PIN_PASSIVE:     return "pass";
        case PIN_POWER_IN:    return "pwrin";
        case PIN_POWER_OUT:   return "pwrout";
        case PIN_OPEN_COLL:   return "oc";
        case PIN_OPEN_EMIT:   return "oe";
        case PIN_NO_CONNECT:  return "nc";
        case PIN_UNSPECIFIED: return "-";
    }
    return "-";
}

static int cmd_sch_info(kicli_schematic_t *sch, const char *ref, int show_pins)
{
    const kicli_symbol_t *s = kicli_sch_symbol_by_ref(sch, ref);

    if (!s) {
        fprintf(stderr, CLR_RED "error:" CLR_RESET
                " component '%s' not found\n", ref);
        return 2;
    }

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

    const char *lcsc = find_lcsc(s);
    printf("  PartNo:     %s\n", lcsc[0] ? lcsc : "(unset)");

    if (s->num_properties > 0) {
        printf("\n  Properties:\n");
        for (size_t j = 0; j < s->num_properties; j++) {
            const kicli_property_t *p = &s->properties[j];
            if (p->hidden) continue;
            printf("    %-20s  %s\n", p->key, p->value ? p->value : "");
        }
    }

    if (s->num_pins > 0) {
        if (show_pins) {
            printf("\n  Pins: %zu\n", s->num_pins);
            kicli_lib_symbol_t *lib =
                kicli_sch_find_lib_symbol(sch, s->lib_id);
            printf("    %-8s  %-24s  %-8s  %s\n", "NUM", "NAME", "TYPE", "NET");
            for (size_t j = 0; j < s->num_pins; j++) {
                const kicli_pin_ref_t *pr = &s->pins[j];
                /* Look up the library pin by number for name/type. */
                const char *name = "";
                const char *type = "-";
                if (lib) {
                    for (size_t k = 0; k < lib->num_pins; k++) {
                        if (strcmp(lib->pins[k].number, pr->number) == 0) {
                            name = lib->pins[k].name;
                            type = pin_type_str(lib->pins[k].pin_type);
                            break;
                        }
                    }
                }
                printf("    %-8s  %-24s  %-8s  %s\n",
                       pr->number, name, type,
                       pr->net[0] ? pr->net : "~");
            }
        } else {
            printf("\n  Pins: %zu  (use --pins to list)\n", s->num_pins);
        }
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

    /* -o - (or --output -) means stream the report to stdout without
     * the "Found N violations / Saved ERC Report to ..." chatter that
     * kicad-cli normally prints. Capture kicad-cli's stdout into a
     * discard buffer; the actual report goes to our temp file. */
    if (output && strcmp(output, "-") == 0) {
        char tmp[KICLI_PATH_MAX];
        kicli_temp_path(tmp, sizeof(tmp), "erc", "rpt");
        const char *args[8];
        int n = 0;
        args[n++] = "sch"; args[n++] = "erc";
        args[n++] = "--output"; args[n++] = tmp;
        args[n++] = sch_path;
        args[n]   = NULL;

        char *discard = NULL;
        kicli_err_t rc = kicad_cli_capture(args, &discard);
        free(discard);

        FILE *f = fopen(tmp, "r");
        if (f) {
            char buf[4096];
            size_t n2;
            while ((n2 = fread(buf, 1, sizeof(buf), f)) > 0)
                fwrite(buf, 1, n2, stdout);
            fclose(f);
        }
        kicli_unlink(tmp);
        /* kicad-cli returns non-zero when ERC violations exist; report was
         * already printed, so surface a clean exit code (0=ok, 1=violations). */
        return (rc == KICLI_OK) ? 0 : 1;
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

/* ── cmd_sch (dispatcher) ────────────────────────────────────────────────── */

/* ── Multi-sheet helpers ─────────────────────────────────────────────────── */

/* Extract basename minus the trailing .kicad_sch. Out must be >= 256 bytes. */
static void sheet_label_from_path(char *out, size_t outsz, const char *path)
{
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bs = strrchr(path, '\\');
    if (!slash || (bs && bs > slash)) slash = bs;
#endif
    const char *base = slash ? slash + 1 : path;
    snprintf(out, outsz, "%s", base);
    char *dot = strstr(out, ".kicad_sch");
    if (dot) *dot = '\0';
}

static int run_list_on_file(const char *file, int show_all,
                             const char *sheet_label)
{
    kicli_schematic_t *sch = NULL;
    kicli_err_t rc = kicli_sch_read(file, &sch);
    if (rc != KICLI_OK) {
        fprintf(stderr, CLR_RED "error:" CLR_RESET " %s: %s\n",
                file, kicli_last_error());
        return (rc == KICLI_ERR_IO) ? 3 : 4;
    }
    int ret = cmd_sch_list_prefixed(sch, show_all, sheet_label);
    kicli_sch_free(sch);
    return ret;
}

static int run_info_on_file(const char *file, const char *ref, int show_pins)
{
    kicli_schematic_t *sch = NULL;
    kicli_err_t rc = kicli_sch_read(file, &sch);
    if (rc != KICLI_OK) {
        fprintf(stderr, CLR_RED "error:" CLR_RESET " %s: %s\n",
                file, kicli_last_error());
        return (rc == KICLI_ERR_IO) ? 3 : 4;
    }
    /* On "not found", return 2 — but info across a dir should keep searching. */
    kicli_symbol_t *s = kicli_sch_symbol_by_ref(sch, ref);
    int ret = 2;
    if (s) ret = cmd_sch_info(sch, ref, show_pins);
    kicli_sch_free(sch);
    return ret;
}

int cmd_sch(int argc, char **argv, const kicli_config_t *cfg)
{
    (void)cfg;

    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        printf("Usage: kicli sch <file|dir> <command> [args]\n");
        printf("\nRead (accepts either a single .kicad_sch or a project directory):\n");
        printf("  list [--all]              Tab-separated: REF\\tVALUE\\tLIB\\tFOOTPRINT\\tPartNo\n");
        printf("                            --all includes power/virtual symbols\n");
        printf("                            On a directory: adds 6th column SHEET\n");
        printf("  info <REF> [--pins]       All properties + PartNo status\n");
        printf("                            --pins adds the full pin table (NUM NAME TYPE NET)\n");
        printf("  view [-o FILE]            Full pin+net connectivity table\n");
        printf("                            On a directory: walks all .kicad_sch, unified net\n");
        printf("                            names via the root schematic's netlist (resolved\n");
        printf("                            across sheet pins ↔ hierarchical labels)\n");
        printf("\nWrite:\n");
        printf("  set <REF> <FIELD> <VALUE>                          One file only.\n");
        printf("    e.g. kicli sch board.kicad_sch set R1 LCSC C25744\n");
        printf("  set-all <VALUE> <FIELD> <NEW> [--footprint <glob>] [--dry-run]\n");
        printf("    Accepts file or dir. Dir form walks every .kicad_sch.\n");
        printf("    e.g. kicli sch proj/ set-all \"LED\" LCSC C2286 --footprint \"*0603*\"\n");
        printf("\nExport (kicad-cli passthrough, single file only):\n");
        printf("  export pdf|svg|netlist|bom [-o FILE]\n");
        printf("  erc [-o FILE|-]           Electrical rules check (-o - streams to stdout)\n");
        return 0;
    }

    const char *sch_path = argv[1];

    if (argc < 3) {
        fprintf(stderr, CLR_RED "error:" CLR_RESET
                " missing command. Run 'kicli sch --help'\n");
        return 1;
    }

    const char *subcmd = argv[2];
    int is_dir = kicli_is_dir(sch_path);

    /* Passthrough commands need a single file. */
    if (strcmp(subcmd, "export")  == 0) {
        if (is_dir) {
            fprintf(stderr, CLR_RED "error:" CLR_RESET
                    " 'export' requires a single .kicad_sch file, not a directory\n");
            return 1;
        }
        return cmd_sch_export(sch_path, argc - 3, argv + 3);
    }
    if (strcmp(subcmd, "erc") == 0) {
        if (is_dir) {
            fprintf(stderr, CLR_RED "error:" CLR_RESET
                    " 'erc' requires a single .kicad_sch file, not a directory\n");
            return 1;
        }
        return cmd_sch_erc(sch_path, argc - 3, argv + 3);
    }
    if (strcmp(subcmd, "view") == 0) {
        if (is_dir)
            return cmd_sch_view_dir(sch_path, argc - 3, argv + 3);
        return cmd_sch_view(sch_path, argc - 3, argv + 3);
    }

    if (strcmp(subcmd, "set") == 0) {
        if (is_dir) {
            fprintf(stderr, CLR_RED "error:" CLR_RESET
                    " 'set' requires a single .kicad_sch file (use 'set-all' for directories)\n");
            return 1;
        }
        if (argc < 6) {
            fprintf(stderr, "Usage: kicli sch <file> set <REF> <FIELD> <VALUE>\n");
            return 1;
        }
        return cmd_sch_set(sch_path, argv[3], argv[4], argv[5]);
    }

    if (strcmp(subcmd, "set-all") == 0) {
        return cmd_sch_set_all(sch_path, argc - 3, argv + 3);
    }

    /* list and info — both support file and dir. */
    if (strcmp(subcmd, "list") == 0) {
        int show_all = 0;
        for (int i = 3; i < argc; i++)
            if (strcmp(argv[i], "--all") == 0) { show_all = 1; break; }

        if (!is_dir)
            return run_list_on_file(sch_path, show_all, NULL);

        /* Directory mode: iterate .kicad_sch files. */
        kicli_dir_t *d = kicli_opendir(sch_path);
        if (!d) {
            fprintf(stderr, CLR_RED "error:" CLR_RESET
                    " cannot open dir '%s'\n", sch_path);
            return 3;
        }
        const char *name;
        int any = 0, worst = 0;
        while ((name = kicli_readdir(d)) != NULL) {
            size_t nl = strlen(name);
            if (nl < 10 || strcmp(name + nl - 10, ".kicad_sch") != 0) continue;

            char file[KICLI_PATH_MAX];
            snprintf(file, sizeof(file), "%s%c%s", sch_path, KICLI_PATH_SEP, name);
            char label[256];
            sheet_label_from_path(label, sizeof(label), name);
            int rc = run_list_on_file(file, show_all, label);
            if (rc != 0 && rc > worst) worst = rc;
            any = 1;
        }
        kicli_closedir(d);
        if (!any) {
            fprintf(stderr, CLR_RED "error:" CLR_RESET
                    " no .kicad_sch files in '%s'\n", sch_path);
            return 2;
        }
        return worst;
    }

    if (strcmp(subcmd, "info") == 0) {
        if (argc < 4) {
            fprintf(stderr, CLR_RED "error:" CLR_RESET " 'info' requires a REF\n");
            return 1;
        }
        const char *ref = argv[3];
        int show_pins = 0;
        for (int i = 4; i < argc; i++)
            if (strcmp(argv[i], "--pins") == 0) { show_pins = 1; break; }

        if (!is_dir)
            return run_info_on_file(sch_path, ref, show_pins);

        /* Directory mode: search all .kicad_sch, stop at first match. */
        kicli_dir_t *d = kicli_opendir(sch_path);
        if (!d) {
            fprintf(stderr, CLR_RED "error:" CLR_RESET
                    " cannot open dir '%s'\n", sch_path);
            return 3;
        }
        const char *name;
        int found = 0;
        while ((name = kicli_readdir(d)) != NULL) {
            size_t nl = strlen(name);
            if (nl < 10 || strcmp(name + nl - 10, ".kicad_sch") != 0) continue;
            char file[KICLI_PATH_MAX];
            snprintf(file, sizeof(file), "%s%c%s", sch_path, KICLI_PATH_SEP, name);
            int rc = run_info_on_file(file, ref, show_pins);
            if (rc != 2) { /* 2 = not found on this sheet; keep looking */
                kicli_closedir(d);
                return rc;
            }
            found = 1;
        }
        kicli_closedir(d);
        if (!found) {
            fprintf(stderr, CLR_RED "error:" CLR_RESET
                    " no .kicad_sch files in '%s'\n", sch_path);
            return 2;
        }
        fprintf(stderr, CLR_RED "error:" CLR_RESET
                " component '%s' not found in any sheet\n", ref);
        return 2;
    }

    fprintf(stderr, CLR_RED "error:" CLR_RESET
            " unknown sch command '%s'\n", subcmd);
    fprintf(stderr, "Run 'kicli sch --help' for usage.\n");
    return 1;
}
