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
#include "skills_md.h"

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
    printf("usage: kicli [-h | --help] [-V | --version] <command> [<args>]\n\n");

    printf("read a schematic\n");
    printf("   list        List all components (tab-separated)\n");
    printf("   info        Show all properties of a component\n");
    printf("   view        Pin-level connectivity table (grep-friendly)\n");

    printf("\nedit a schematic\n");
    printf("   set         Set a field on one component\n");
    printf("   set-all     Bulk-set a field on all matching components\n");

    printf("\nexport via kicad-cli\n");
    printf("   export      Export schematic as PDF, SVG, netlist, or BOM\n");
    printf("   erc         Run electrical rules check\n");

    printf("\nJLCPCB component database\n");
    printf("   part        Lookup part detail, stock, price, datasheet URL\n");
    printf("   search      Search JLCPCB catalog (CSV output)\n");
    printf("   bom         Generate JLCPCB-ready BOM from schematic\n");

    printf("\nproject\n");
    printf("   new         Create a new KiCad 10 project\n");
    printf("   config      Show or set kicad-cli path\n");
    printf("   skills      Print agent skill guide (SKILLS.md)\n");

    printf("\n'kicli sch --help' and 'kicli jlcpcb --help' for full syntax.\n");
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
        printf("kicli 0.4.0\n");
        return 0;
    }
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_usage();
        return 0;
    }

    if (strcmp(cmd, "skills") == 0) {
        printf("%s", skills_md);
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
