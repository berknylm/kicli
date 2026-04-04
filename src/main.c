/*
 * kicli — agent-friendly KiCad CLI wrapper
 *
 * Usage:
 *   kicli new <name>                    Create a KiCad 10 project
 *   kicli config kicad-path [path]      Show or set kicad-cli path
 *   kicli kicad-path                    Show where kicad-cli is installed
 *   kicli kicad-version                 Show KiCad version
 *   kicli sch <file> list               List components (grep-friendly)
 *   kicli sch <file> export pdf         Delegate to kicad-cli
 *   kicli fetch <LCSC_ID>               Fetch a component (M6)
 *   kicli stock <PART>                  Check stock (M7)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "kicli/config.h"
#include "kicli/error.h"
#include "kicli/kicad_cli.h"
#include "kicli/jlcpcb.h"

/* ── VT color support on Windows 10+ ───────────────────────────────────── */
#ifdef _WIN32
#  include <windows.h>
static void enable_vt_colors(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}
#else
static void enable_vt_colors(void) {}
#endif

#define CLR_RESET  "\x1b[0m"
#define CLR_BOLD   "\x1b[1m"
#define CLR_RED    "\x1b[31m"
#define CLR_GREEN  "\x1b[32m"
#define CLR_CYAN   "\x1b[36m"

/* ── Forward declarations ───────────────────────────────────────────────── */

int cmd_new   (int argc, char **argv);
int cmd_fetch (int argc, char **argv);
int cmd_stock (int argc, char **argv);
int cmd_jlcpcb(int argc, char **argv);
int cmd_sch   (int argc, char **argv, const kicli_config_t *cfg);

/* ── Usage ──────────────────────────────────────────────────────────────── */

static void print_usage(void) {
    printf(CLR_BOLD "kicli" CLR_RESET " — pipe-friendly CLI for KiCad 10\n");
    printf("Designed for AI agents and shell automation. All output is machine-parseable.\n\n");
    printf("Usage: kicli <command> [args]\n\n");

    printf(CLR_BOLD "Project:\n" CLR_RESET);
    printf("  " CLR_CYAN "new <name>" CLR_RESET "                    Create KiCad 10 project with local libs\n");

    printf("\n" CLR_BOLD "Configuration:\n" CLR_RESET);
    printf("  " CLR_CYAN "config kicad-path" CLR_RESET "             Show current kicad-cli path\n");
    printf("  " CLR_CYAN "config kicad-path <path>" CLR_RESET "      Set kicad-cli path manually\n");
    printf("  " CLR_CYAN "kicad-path" CLR_RESET "                    Print resolved kicad-cli binary path\n");
    printf("  " CLR_CYAN "kicad-version" CLR_RESET "                 Print KiCad version (e.g. 10.0.0)\n");

    printf("\n" CLR_BOLD "Schematic — Read:\n" CLR_RESET);
    printf("  " CLR_CYAN "sch <file> list [--all]" CLR_RESET "       List components (tab-separated: REF VALUE LIB FOOTPRINT)\n");
    printf("  " CLR_CYAN "sch <file> info <REF>" CLR_RESET "         Show all properties of a component\n");
    printf("  " CLR_CYAN "sch <file> nets" CLR_RESET "               List all local and global net labels\n");
    printf("  " CLR_CYAN "sch <file> stats" CLR_RESET "              Component/wire/net/junction counts\n");
    printf("  " CLR_CYAN "sch <file> dump [-o f.kisch]" CLR_RESET "  Full pin+net table in .kisch format\n");

    printf("\n" CLR_BOLD "Schematic — Write:\n" CLR_RESET);
    printf("  " CLR_CYAN "sch <file> set <REF> <FIELD> <VAL>" CLR_RESET "\n");
    printf("                                    Set a property on one component\n");
    printf("  " CLR_CYAN "sch <file|dir> set-all <VALUE_MATCH> <FIELD> <NEW_VAL>" CLR_RESET "\n");
    printf("                                    Bulk-set a field on all components matching value\n");
    printf("                                    If <dir>, applies across all .kicad_sch files\n");

    printf("\n" CLR_BOLD "Schematic — Export (kicad-cli passthrough):\n" CLR_RESET);
    printf("  " CLR_CYAN "sch <file> export pdf|svg|netlist|bom" CLR_RESET "\n");
    printf("  " CLR_CYAN "sch <file> erc" CLR_RESET "                Electrical rules check\n");

    printf("\n" CLR_BOLD "JLCPCB:\n" CLR_RESET);
    printf("  " CLR_CYAN "jlcpcb part <LCSC_ID>" CLR_RESET "        Lookup part detail (brand, model, stock, price)\n");
    printf("  " CLR_CYAN "jlcpcb search <query> [-n N]" CLR_RESET "  Search JLCPCB catalog (type/description/stock)\n");
    printf("  " CLR_CYAN "jlcpcb bom <sch> [-o out.csv]" CLR_RESET " Generate JLCPCB-ready BOM CSV\n");
    printf("                                    Columns: Comment,Designator,Footprint,LCSC\n");
    printf("                                    See Sample-BOM_JLCSMT.xlsx for JLCPCB format reference\n");

    printf("\n" CLR_BOLD "Typical agent workflow — JLCPCB BOM:\n" CLR_RESET);
    printf("  1. kicli sch board.kicad_sch list          # inspect components\n");
    printf("  2. kicli jlcpcb bom board.kicad_sch        # check which LCSC codes are missing\n");
    printf("  3. kicli jlcpcb search \"100nF 0402\"         # find LCSC part number\n");
    printf("  4. kicli sch proj/ set-all \"100nF\" LCSC C1525  # assign to all matching\n");
    printf("  5. kicli jlcpcb bom board.kicad_sch -o bom.csv # export final BOM\n");

    printf("\nConfig: ~/.config/kicli/config.toml | .kicli.toml (project)\n");
    printf("Env:    KICAD_CLI_PATH overrides kicad-cli location\n");
    printf("\nRun " CLR_CYAN "kicli <command> --help" CLR_RESET " for detailed help on each subcommand:\n");
    printf("  kicli sch --help       kicli jlcpcb --help       kicli config --help\n");
}

/* ── cmd_config ─────────────────────────────────────────────────────────── */

static int cmd_config(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "kicad-path") != 0) {
        printf("Usage: kicli config kicad-path [path]\n");
        printf("  Without path: show current setting\n");
        printf("  With path:    save and use that path\n");
        return 0;
    }

    if (argc >= 3) {
        kicli_err_t err = kicad_cli_set_path(argv[2]);
        if (err != KICLI_OK) {
            fprintf(stderr, CLR_RED "error:" CLR_RESET " %s\n", kicli_last_error());
            return 1;
        }
        printf(CLR_GREEN "saved:" CLR_RESET " kicad_path=%s\n", argv[2]);
        char cfgpath[KICLI_CONFIG_PATH_MAX];
        kicli_config_path(cfgpath, sizeof(cfgpath));
        printf("       (written to %s)\n", cfgpath);
        return 0;
    }

    kicli_config_t cfg;
    kicli_config_load(&cfg);
    char cfgpath[KICLI_CONFIG_PATH_MAX];
    kicli_config_path(cfgpath, sizeof(cfgpath));
    printf("config file: %s\n", cfgpath);
    if (cfg.kicad_path[0])
        printf("kicad_path:  %s\n", cfg.kicad_path);
    else
        printf("kicad_path:  (not set — will auto-discover)\n");
    return 0;
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    enable_vt_colors();

    if (argc < 2) { print_usage(); return 0; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-V") == 0) {
        printf("kicli 0.3.0\n");
        return 0;
    }
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_usage();
        return 0;
    }

    if (strcmp(cmd, "config")       == 0) return cmd_config    (argc - 1, argv + 1);
    if (strcmp(cmd, "new")          == 0) return cmd_new       (argc - 1, argv + 1);
    if (strcmp(cmd, "fetch")        == 0) return cmd_fetch     (argc - 1, argv + 1);
    if (strcmp(cmd, "stock")        == 0) return cmd_stock     (argc - 1, argv + 1);
    if (strcmp(cmd, "jlcpcb")       == 0) return cmd_jlcpcb    (argc - 1, argv + 1);

    if (strcmp(cmd, "kicad-path") == 0) {
        char path[KICAD_CLI_MAX_PATH];
        if (kicad_cli_find(path) != KICLI_OK) {
            fprintf(stderr, CLR_RED "error:" CLR_RESET " %s\n", kicli_last_error());
            return 1;
        }
        printf("%s\n", path);
        return 0;
    }
    if (strcmp(cmd, "kicad-version") == 0) {
        char ver[32];
        if (kicad_cli_version(ver) != KICLI_OK) {
            fprintf(stderr, CLR_RED "error:" CLR_RESET " %s\n", kicli_last_error());
            return 1;
        }
        printf("%s\n", ver);
        return 0;
    }
    if (strcmp(cmd, "sch") == 0) {
        kicli_config_t cfg;
        kicli_config_load(&cfg);
        return cmd_sch(argc - 1, argv + 1, &cfg);
    }

    fprintf(stderr, CLR_RED "error:" CLR_RESET " unknown command '%s'\n", cmd);
    fprintf(stderr, "Run 'kicli --help' for usage.\n");
    return 1;
}
