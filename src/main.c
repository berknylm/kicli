/*
 * kicli — KiCad CLI toolkit
 * Entry point and top-level argument dispatch.
 *
 * Build:
 *   cmake --preset debug && cmake --build build/debug
 *
 * Usage:
 *   kicli fetch <ID> [--source lcsc] [--lib mylib]
 *   kicli stock check <PART> ...
 *   kicli sch read <FILE>
 *   kicli --help
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* optparse: portable getopt replacement (header-only, no getopt.h needed) */
#define OPTPARSE_IMPLEMENTATION
#define OPTPARSE_API static
#include "optparse.h"

#include "kicli/config.h"
#include "kicli/error.h"
#include "kicli/fetch.h"
#include "kicli/stock.h"
#include "kicli/sch.h"

/* ── Colour helpers (enable VT on Windows 10+) ─────────────────────────── */
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
#define CLR_YELLOW "\x1b[33m"
#define CLR_CYAN   "\x1b[36m"

/* ── Usage ──────────────────────────────────────────────────────────────── */

static void print_usage(void) {
    printf(CLR_BOLD "kicli" CLR_RESET " — KiCad CLI toolkit\n\n");
    printf("Usage: kicli <command> [subcommand] [options]\n\n");
    printf(CLR_BOLD "Commands:\n" CLR_RESET);
    printf("  " CLR_CYAN "fetch" CLR_RESET "   Fetch component symbols, footprints and 3D models\n");
    printf("  " CLR_CYAN "stock" CLR_RESET "   Check component stock levels and pricing\n");
    printf("  " CLR_CYAN "sch"   CLR_RESET "     Read and manipulate KiCad schematic files\n");
    printf("\nRun 'kicli <command> --help' for command-specific help.\n");
}

/* ── Sub-dispatchers (implemented in their own files) ───────────────────── */

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

    /* Global --version / --help before config load */
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
        printf("kicli 0.1.0\n");
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage();
        return 0;
    }

    /* Load config (tolerates missing files) */
    kicli_config_t cfg;
    kicli_config_load(&cfg);

    const char *cmd = argv[1];

    if (strcmp(cmd, "fetch") == 0) return cmd_fetch(argc - 1, argv + 1, &cfg);
    if (strcmp(cmd, "stock") == 0) return cmd_stock(argc - 1, argv + 1, &cfg);
    if (strcmp(cmd, "sch")   == 0) return cmd_sch  (argc - 1, argv + 1, &cfg);

    fprintf(stderr, CLR_RED "error:" CLR_RESET " unknown command '%s'\n", cmd);
    print_usage();
    return 1;
}
