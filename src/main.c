/*
 * kicli — agent-friendly KiCad CLI wrapper
 *
 * Build:
 *   cmake --preset debug && cmake --build build/debug
 *
 * Usage:
 *   kicli new <name>                    Create a KiCad 10 project
 *   kicli kicad-path                    Show where kicad-cli is installed
 *   kicli kicad-version                 Show KiCad version
 *   kicli sch <file> list               List components (grep-friendly)
 *   kicli sch <file> export pdf         Delegate to kicad-cli
 *   kicli fetch <LCSC_ID>               Fetch a component
 *   kicli stock <PART>                  Check stock
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define OPTPARSE_IMPLEMENTATION
#define OPTPARSE_API static
#include "optparse.h"

#include "kicli/config.h"
#include "kicli/error.h"
#include "kicli/kicad_cli.h"

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
#define CLR_CYAN   "\x1b[36m"

/* ── Usage ──────────────────────────────────────────────────────────────── */

static void print_usage(void) {
    printf(CLR_BOLD "kicli" CLR_RESET " — agent-friendly KiCad CLI wrapper\n\n");
    printf("Usage: kicli <command> [args]\n\n");
    printf(CLR_BOLD "Project:\n" CLR_RESET);
    printf("  " CLR_CYAN "new" CLR_RESET " <name>              Create a new KiCad 10 project\n");
    printf("\n" CLR_BOLD "KiCad:\n" CLR_RESET);
    printf("  " CLR_CYAN "kicad-path" CLR_RESET "              Show path to kicad-cli\n");
    printf("  " CLR_CYAN "kicad-version" CLR_RESET "           Show installed KiCad version\n");
    printf("\n" CLR_BOLD "Schematic:\n" CLR_RESET);
    printf("  " CLR_CYAN "sch" CLR_RESET " <file> list         List all components\n");
    printf("  " CLR_CYAN "sch" CLR_RESET " <file> info <ref>   Show component details\n");
    printf("  " CLR_CYAN "sch" CLR_RESET " <file> nets         List all nets\n");
    printf("  " CLR_CYAN "sch" CLR_RESET " <file> export <fmt> Export (pdf/svg/netlist/bom)\n");
    printf("  " CLR_CYAN "sch" CLR_RESET " <file> erc          Run electrical rules check\n");
    printf("  " CLR_CYAN "sch" CLR_RESET " <file> set <r> <f> <v>  Edit a field\n");
    printf("\n" CLR_BOLD "Components:\n" CLR_RESET);
    printf("  " CLR_CYAN "fetch" CLR_RESET " <LCSC_ID>         Fetch component from LCSC\n");
    printf("  " CLR_CYAN "fetch" CLR_RESET " search <query>    Search components\n");
    printf("\n" CLR_BOLD "Stock:\n" CLR_RESET);
    printf("  " CLR_CYAN "stock" CLR_RESET " <part> [part...]  Check stock and pricing\n");
    printf("  " CLR_CYAN "stock" CLR_RESET " bom <file>        Check all BOM parts\n");
    printf("\nRun 'kicli <command> --help' for detailed usage.\n");
}

/* ── Forward declarations ───────────────────────────────────────────────── */

int cmd_new  (int argc, char **argv);
int cmd_fetch(int argc, char **argv, const kicli_config_t *cfg);
int cmd_stock(int argc, char **argv, const kicli_config_t *cfg);
int cmd_sch  (int argc, char **argv, const kicli_config_t *cfg);

/* ── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    enable_vt_colors();

    if (argc < 2) {
        print_usage();
        return 0;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-V") == 0) {
        printf("kicli 0.1.0\n");
        return 0;
    }
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_usage();
        return 0;
    }

    /* kicad-path and kicad-version don't need config */
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

    /* new doesn't need config either */
    if (strcmp(cmd, "new") == 0) {
        return cmd_new(argc - 1, argv + 1);
    }

    /* Load config (tolerates missing files — uses defaults) */
    kicli_config_t cfg;
    kicli_config_load(&cfg);

    if (strcmp(cmd, "fetch") == 0) return cmd_fetch(argc - 1, argv + 1, &cfg);
    if (strcmp(cmd, "stock") == 0) return cmd_stock(argc - 1, argv + 1, &cfg);
    if (strcmp(cmd, "sch")   == 0) return cmd_sch  (argc - 1, argv + 1, &cfg);

    fprintf(stderr, CLR_RED "error:" CLR_RESET " unknown command '%s'\n", cmd);
    fprintf(stderr, "Run 'kicli --help' for usage.\n");
    return 1;
}
