/*
 * project.c — kicli new <name>
 *
 * Creates a complete KiCad 10 project:
 *   <name>/
 *     <name>.kicad_pro      KiCad 10 project file (JSON)
 *     <name>.kicad_sch      Blank schematic (s-expression)
 *     sym-lib-table         Symbol library table -> libs/symbols/
 *     fp-lib-table          Footprint library table -> libs/footprints/
 *     libs/symbols/
 *     libs/footprints/<name>-footprints.pretty/
 *     libs/3dmodels/
 *     .kicli.toml           kicli project config
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#  include <direct.h>
static int make_dir(const char *p) { return _mkdir(p); }
#else
#  include <sys/stat.h>
static int make_dir(const char *p) { return mkdir(p, 0755); }
#endif

#define CLR_RESET  "\x1b[0m"
#define CLR_BOLD   "\x1b[1m"
#define CLR_RED    "\x1b[31m"
#define CLR_GREEN  "\x1b[32m"
#define CLR_CYAN   "\x1b[36m"
#define CLR_DIM    "\x1b[2m"

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static int ensure_dir(const char *path) {
    if (make_dir(path) != 0 && errno != EEXIST) {
        fprintf(stderr, CLR_RED "error:" CLR_RESET
                " cannot create '%s': %s\n", path, strerror(errno));
        return -1;
    }
    printf(CLR_DIM "  mkdir  %s" CLR_RESET "\n", path);
    return 0;
}

static int write_file(const char *path, const char *fmt, ...) {
    char buf[16384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, CLR_RED "error:" CLR_RESET
                " cannot write '%s': %s\n", path, strerror(errno));
        return -1;
    }
    fputs(buf, f);
    fclose(f);
    printf(CLR_DIM "  write  %s" CLR_RESET "\n", path);
    return 0;
}

/* ── KiCad 10 file templates ─────────────────────────────────────────────── */

static int write_pro(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.kicad_pro", dir, name);
    return write_file(path,
        "{\n"
        "  \"board\": { \"3dviewports\": [], \"design_settings\": {}, \"layer_presets\": [], \"viewports\": [] },\n"
        "  \"boards\": [],\n"
        "  \"cvpcb\": { \"equivalence_files\": [] },\n"
        "  \"libraries\": { \"pinned_footprint_libs\": [], \"pinned_symbol_libs\": [] },\n"
        "  \"meta\": { \"filename\": \"%s.kicad_pro\", \"version\": 1 },\n"
        "  \"net_settings\": { \"classes\": [], \"meta\": { \"version\": 3 }, \"net_colors\": {} },\n"
        "  \"pcbnew\": { \"last_paths\": {}, \"page_layout_descr_file\": \"\" },\n"
        "  \"schematic\": { \"annotate_start_num\": 0, \"drawing\": {}, \"legacy_lib_dir\": \"\", \"legacy_lib_list\": [] },\n"
        "  \"text_variables\": {}\n"
        "}\n",
        name
    );
}

static int write_sch(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.kicad_sch", dir, name);

    /* Simple UUID from time + rand */
    srand((unsigned int)time(NULL));
    char uuid[40];
    snprintf(uuid, sizeof(uuid), "%08x-%04x-4%03x-%04x-%012x",
             rand(), rand()&0xffff, rand()&0xfff,
             (rand()&0x3fff)|0x8000, rand());

    return write_file(path,
        "(kicad_sch\n"
        "  (version 20241010)\n"
        "  (generator \"kicli\")\n"
        "  (generator_version \"0.1\")\n"
        "  (uuid \"%s\")\n"
        "  (paper \"A4\")\n"
        "  (title_block\n"
        "    (title \"%s\")\n"
        "    (date \"\")\n"
        "    (rev \"1\")\n"
        "    (company \"\")\n"
        "  )\n"
        "  (lib_symbols)\n"
        "  (sheet_instances\n"
        "    (path \"/\" (page \"1\"))\n"
        "  )\n"
        ")\n",
        uuid, name
    );
}

static int write_sym_lib_table(const char *dir, const char *lib_name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/sym-lib-table", dir);
    return write_file(path,
        "(sym_lib_table\n"
        "  (version 7)\n"
        "  (lib (name \"%s\") (type \"KiCad\")\n"
        "    (uri \"${KIPRJMOD}/libs/symbols/%s.kicad_sym\")\n"
        "    (options \"\") (descr \"Local symbols — managed by kicli\"))\n"
        ")\n",
        lib_name, lib_name
    );
}

static int write_fp_lib_table(const char *dir, const char *lib_name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/fp-lib-table", dir);
    return write_file(path,
        "(fp_lib_table\n"
        "  (version 7)\n"
        "  (lib (name \"%s\") (type \"KiCad\")\n"
        "    (uri \"${KIPRJMOD}/libs/footprints/%s.pretty\")\n"
        "    (options \"\") (descr \"Local footprints — managed by kicli\"))\n"
        ")\n",
        lib_name, lib_name
    );
}

static int write_kicli_toml(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/.kicli.toml", dir);
    return write_file(path,
        "[project]\n"
        "kicad_version = \"10\"\n"
        "lib_dir = \"libs\"\n"
        "\n"
        "[fetch]\n"
        "default_source = \"lcsc\"\n"
        "auto_3d_models = true\n"
        "symbol_lib = \"%s-symbols\"\n"
        "footprint_lib = \"%s-footprints\"\n"
        "\n"
        "[stock]\n"
        "suppliers = [\"lcsc\", \"digikey\", \"mouser\"]\n"
        "currency = \"USD\"\n"
        "\n"
        "[sch]\n"
        "backup_on_write = true\n",
        name, name
    );
}

/* ── cmd_new ─────────────────────────────────────────────────────────────── */

int cmd_new(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0) {
        printf("Usage: kicli new <project-name> [directory]\n\n");
        printf("Creates a new KiCad 10 project:\n");
        printf("  <name>.kicad_pro    Project file\n");
        printf("  <name>.kicad_sch    Blank schematic\n");
        printf("  sym-lib-table       Symbol library table\n");
        printf("  fp-lib-table        Footprint library table\n");
        printf("  libs/               Local component libraries\n");
        printf("  .kicli.toml         kicli project config\n");
        return 0;
    }

    const char *name = argv[1];
    const char *outdir = (argc >= 3) ? argv[2] : name;

    /* Basic name validation */
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            fprintf(stderr, CLR_RED "error:" CLR_RESET
                    " name must not contain '/', '\\' or ':'\n");
            return 1;
        }
    }

    char sym_lib[256], fp_lib[256], fp_pretty[1024];
    snprintf(sym_lib,   sizeof(sym_lib),   "%s-symbols",    name);
    snprintf(fp_lib,    sizeof(fp_lib),    "%s-footprints", name);
    snprintf(fp_pretty, sizeof(fp_pretty), "%s/libs/footprints/%s.pretty", outdir, fp_lib);

    printf(CLR_BOLD "Creating KiCad 10 project '%s'...\n" CLR_RESET, name);

    /* Directories */
    char d[1024];
#define D(fmt, ...) snprintf(d,sizeof(d),fmt,##__VA_ARGS__); if(ensure_dir(d)!=0) return 1
    D("%s",                 outdir);
    D("%s/libs",            outdir);
    D("%s/libs/symbols",    outdir);
    D("%s/libs/footprints", outdir);
    D("%s/libs/3dmodels",   outdir);
    D("%s",                 fp_pretty);
#undef D

    /* Files */
    if (write_pro(outdir, name)                  != 0) return 1;
    if (write_sch(outdir, name)                  != 0) return 1;
    if (write_sym_lib_table(outdir, sym_lib)     != 0) return 1;
    if (write_fp_lib_table(outdir, fp_lib)       != 0) return 1;
    if (write_kicli_toml(outdir, name)           != 0) return 1;

    printf("\n" CLR_GREEN CLR_BOLD "Done!" CLR_RESET " Project '%s' is ready.\n\n", name);
    printf("  Open:   " CLR_CYAN "kicad %s/%s.kicad_pro\n" CLR_RESET, outdir, name);
    printf("  Fetch:  " CLR_CYAN "cd %s && kicli fetch C2040\n" CLR_RESET, outdir);
    return 0;
}
