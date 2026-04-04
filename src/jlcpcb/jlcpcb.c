/* jlcpcb.c — JLCPCB integration: part lookup + BOM generation */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#  define strcasecmp _stricmp
#  include <windows.h>
#  ifndef S_ISDIR
#    define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#  endif
#else
#  include <dirent.h>
#endif
#include <curl/curl.h>
#include "kicli/jlcpcb.h"
#include "kicli/sch.h"
#include "kicli/error.h"

/* ── curl write callback ─────────────────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t len;
} buf_t;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userp)
{
    size_t n = size * nmemb;
    buf_t *b = userp;
    char *tmp = realloc(b->data, b->len + n + 1);
    if (!tmp) return 0;
    b->data = tmp;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

/* ── fetch part detail from JLCPCB ───────────────────────────────────────── */

static void jlcpcb_print(char* str){
    char* ls[][2] = {
        {"componentBrandEn","Brand"},
        {"componentModelEn","Model"},
        {"componentCode","Code"},
        {"dataManualUrl","Datasheet"},
        {"overseasStockCount","Count"},
        {"productPrice","Price"},
        {"componentSpecificationEn","Package"}
    };

    for (int i = 0; i < 7; i++){
        char* f = strstr(str, ls[i][0]);
        if (f){
            char* start = strchr(f, ':');
            if (!start) continue;
            start++;

            while (*start == ' ' || *start == '\"') start++;

            char* end = start;
            while (*end && *end != '\"' && *end != ',' && *end != '}') end++;

            printf("%s: %.*s\n", ls[i][1], (int)(end - start), start);
        }
    }
}

static int jlcpcb_part(const char *lcsc_id)
{
    CURL *curl = curl_easy_init();
    if (!curl) { fprintf(stderr, "error: curl init failed\n"); return 1; }

    /* build URL */
    char url[256];
    snprintf(url, sizeof(url),
        "https://cart.jlcpcb.com/shoppingCart/smtGood/getComponentDetail"
        "?componentCode=%s", lcsc_id);

    buf_t buf = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "kicli/0.2.0");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "error: %s\n", curl_easy_strerror(res));
        free(buf.data);
        return 1;
    }

    /* just print the JSON — let the agent/user pipe through jq */
    if (buf.data) {
        jlcpcb_print(buf.data);
        free(buf.data);
    }
    return 0;
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

/* truncate s to at most max_chars UTF-8 characters, writing into buf */
static void utf8_truncate(char *buf, size_t bufsz, const char *s, int max_chars)
{
    int chars = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && chars < max_chars) {
        /* determine byte length of this codepoint */
        int bytes = (*p < 0x80) ? 1 :
                    (*p < 0xE0) ? 2 :
                    (*p < 0xF0) ? 3 : 4;
        if ((size_t)((p - (const unsigned char *)s) + bytes) >= bufsz) break;
        p += bytes;
        chars++;
    }
    size_t len = (size_t)(p - (const unsigned char *)s);
    memcpy(buf, s, len);
    buf[len] = '\0';
}

/* ── csv_field ───────────────────────────────────────────────────────────── */

/* CSV-quote a field (wrap in quotes if it contains comma or quote) */
static void csv_field(FILE *f, const char *s)
{
    if (!s) s = "";
    int needs_quote = (strchr(s, ',') || strchr(s, '"') || strchr(s, '\n'));
    if (needs_quote) {
        fputc('"', f);
        for (; *s; s++) { if (*s == '"') fputc('"', f); fputc(*s, f); }
        fputc('"', f);
    } else {
        fputs(s, f);
    }
}

/* ── search ──────────────────────────────────────────────────────────────── */

/* extract a JSON string field: returns pointer to start, writes length to *len */
static const char *json_str(const char *json, const char *key, int *len)
{
    *len = 0;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') {
        p++;
        const char *start = p;
        while (*p && *p != '"') p++;
        *len = (int)(p - start);
        return start;
    }
    /* numeric */
    const char *start = p;
    while (*p && *p != ',' && *p != '}') p++;
    *len = (int)(p - start);
    return start;
}

typedef struct {
    const char *keyword;
    int         page_size;
    const char *type;       /* "base" or "expand", NULL = any */
    const char *package;    /* e.g. "0402", NULL = any */
    int         in_stock;   /* 1 = only in-stock parts */
} jlcpcb_search_opts_t;

static int jlcpcb_search(const jlcpcb_search_opts_t *opts)
{
    CURL *curl = curl_easy_init();
    if (!curl) { fprintf(stderr, "error: curl init failed\n"); return 1; }

    char url[512];
    snprintf(url, sizeof(url),
        "https://jlcpcb.com/api/overseas-pcb-order/v1/shoppingCart/smtGood"
        "/selectSmtComponentList");

    /* JSON body — keyword goes as-is (spaces are valid in JSON strings) */
    char body[1024];
    int n = snprintf(body, sizeof(body),
        "{\"keyword\":\"%s\",\"currentPage\":1,\"pageSize\":%d",
        opts->keyword, opts->page_size);
    if (opts->type)
        n += snprintf(body + n, sizeof(body) - n,
            ",\"componentLibraryType\":\"%s\"", opts->type);
    if (opts->package)
        n += snprintf(body + n, sizeof(body) - n,
            ",\"componentSpecification\":\"%s\"", opts->package);
    if (opts->in_stock)
        n += snprintf(body + n, sizeof(body) - n, ",\"stockFlag\":true");
    /* sort by stock descending for most useful results */
    n += snprintf(body + n, sizeof(body) - n,
        ",\"sortField\":\"stockCount\",\"sortType\":\"desc\"");
    snprintf(body + n, sizeof(body) - n, "}");

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    buf_t buf = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "kicli/0.2.0");

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "error: %s\n", curl_easy_strerror(res));
        free(buf.data);
        return 1;
    }

    if (!buf.data) return 1;

    /* print each result */
    printf("LCSC,Type,Model,Package,Stock,Price,Description\n");

    const char *p = buf.data;
    /* anchor on componentId which appears at the start of each item object,
       before componentPrices and componentCode */
    while ((p = strstr(p, "\"componentId\"")) != NULL) {
        int l; const char *v;
        char code[32]="", model[64]="", pkg[32]="", stock[32]="", libtype[16]="", desc[256]="";

        v = json_str(p, "componentLibraryType",    &l); if (v) snprintf(libtype, sizeof(libtype), "%.*s", l, v);
        v = json_str(p, "stockCount",              &l); if (v) snprintf(stock,   sizeof(stock),   "%.*s", l, v);

        /* unit price: first productPrice inside componentPrices array (comes before componentCode) */
        char price[16] = "";
        const char *prices_start = strstr(p, "\"componentPrices\"");
        if (prices_start) {
            v = json_str(prices_start, "productPrice", &l);
            if (v) {
                char raw[16] = "";
                snprintf(raw, sizeof(raw), "%.*s", l, v);
                snprintf(price, sizeof(price), "$%.4f", atof(raw));
            }
        }

        v = json_str(p, "componentCode",           &l); 
        if (v) snprintf(code,    sizeof(code),    "%.*s", l, v);
        
        v = json_str(p, "componentModelEn",        &l); 
        if (v) snprintf(model,   sizeof(model),   "%.*s", l, v);
        
        v = json_str(p, "componentSpecificationEn",&l); 
        if (v) snprintf(pkg,     sizeof(pkg),     "%.*s", l, v);
        
        v = json_str(p, "describe",                &l);
        if (v) {
            char tmp[256] = "";
            snprintf(tmp, sizeof(tmp), "%.*s", l, v);
            utf8_truncate(desc, sizeof(desc), tmp, 64);
        }

        csv_field(stdout, code);    fputc(',', stdout);
        csv_field(stdout, libtype); fputc(',', stdout);
        csv_field(stdout, model);   fputc(',', stdout);
        csv_field(stdout, pkg);     fputc(',', stdout);
        csv_field(stdout, stock);   fputc(',', stdout);
        csv_field(stdout, price);   fputc(',', stdout);
        csv_field(stdout, desc);    fputc('\n', stdout);
        p++;
    }

    free(buf.data);
    return 0;
}

/* ── BOM generation ──────────────────────────────────────────────────────── */

/* find LCSC part number from symbol properties */
static const char *find_lcsc(const kicli_symbol_t *s)
{
    static const char *keys[] = { "LCSC", "lcsc", "JLC_PART", "JLCPCB#", NULL };
    for (int k = 0; keys[k]; k++) {
        for (size_t i = 0; i < s->num_properties; i++) {
            if (strcasecmp(s->properties[i].key, keys[k]) == 0)
                return s->properties[i].value ? s->properties[i].value : "";
        }
    }
    return "";
}

/* write BOM rows from a flat symbol array, grouping by value+footprint */
static void bom_write_symbols(FILE *f, const kicli_symbol_t *syms, size_t count,
                              int *total_rows, int *missing_lcsc)
{
    int *seen = calloc(count, sizeof(int));
    if (!seen) return;

    for (size_t i = 0; i < count; i++) {
        const kicli_symbol_t *a = &syms[i];
        if (seen[i] || !a->in_bom || a->reference[0] == '#') continue;

        const char *lcsc_a = find_lcsc(a);
        const char *fp_a   = a->footprint ? a->footprint : "";
        const char *val_a  = a->value     ? a->value     : "";

        char desig[4096] = "";
        strncat(desig, a->reference, sizeof(desig) - 1);
        seen[i] = 1;

        for (size_t j = i + 1; j < count; j++) {
            const kicli_symbol_t *b = &syms[j];
            if (seen[j] || !b->in_bom || b->reference[0] == '#') continue;
            const char *val_b = b->value     ? b->value     : "";
            const char *fp_b  = b->footprint ? b->footprint : "";
            if (strcmp(val_a, val_b) != 0 || strcmp(fp_a, fp_b) != 0) continue;
            strncat(desig, " ", sizeof(desig) - strlen(desig) - 1);
            strncat(desig, b->reference, sizeof(desig) - strlen(desig) - 1);
            seen[j] = 1;
        }

        csv_field(f, val_a);  fputc(',', f);
        csv_field(f, desig);  fputc(',', f);
        csv_field(f, fp_a);   fputc(',', f);
        csv_field(f, lcsc_a); fputc('\n', f);
        (*total_rows)++;
        if (!lcsc_a[0]) (*missing_lcsc)++;
    }

    free(seen);
}

/* load one .kicad_sch, append its symbols to a growing array */
static int bom_load_file(const char *path,
                         kicli_symbol_t **all, size_t *count, size_t *cap,
                         kicli_schematic_t ***to_free, size_t *free_count, size_t *free_cap)
{
    kicli_schematic_t *sch = NULL;
    if (kicli_sch_read(path, &sch) != KICLI_OK) {
        fprintf(stderr, "warning: %s: %s (skipped)\n", path, kicli_last_error());
        return 0;
    }

    /* keep sch alive so symbol pointers remain valid */
    if (*free_count >= *free_cap) {
        *free_cap = *free_cap ? *free_cap * 2 : 8;
        kicli_schematic_t **tmp = realloc(*to_free, *free_cap * sizeof(kicli_schematic_t *));
        if (!tmp) { kicli_sch_free(sch); return 0; }
        *to_free = tmp;
    }
    (*to_free)[(*free_count)++] = sch;

    for (size_t i = 0; i < sch->num_symbols; i++) {
        if (*count >= *cap) {
            *cap = *cap ? *cap * 2 : 256;
            kicli_symbol_t *tmp = realloc(*all, *cap * sizeof(kicli_symbol_t));
            if (!tmp) return 0;
            *all = tmp;
        }
        (*all)[(*count)++] = sch->symbols[i];
    }
    return 0;
}

static int jlcpcb_bom(const char *path, const char *out_path)
{
    kicli_symbol_t *all = NULL;
    size_t count = 0, cap = 0;
    kicli_schematic_t **to_free = NULL;
    size_t free_count = 0, free_cap = 0;

    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "error: cannot stat '%s': %s\n", path, strerror(errno));
        return 1;
    }

    if (S_ISDIR(st.st_mode)) {
        /* scan directory for *.kicad_sch */
#ifdef _WIN32
        char pattern[1024];
        snprintf(pattern, sizeof(pattern), "%s\\*.kicad_sch", path);
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(pattern, &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                char fpath[1024];
                snprintf(fpath, sizeof(fpath), "%s\\%s", path, fd.cFileName);
                bom_load_file(fpath, &all, &count, &cap, &to_free, &free_count, &free_cap);
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
#else
        DIR *dir = opendir(path);
        if (!dir) { fprintf(stderr, "error: cannot open dir '%s'\n", path); return 1; }
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            size_t nl = strlen(ent->d_name);
            if (nl < 10 || strcmp(ent->d_name + nl - 10, ".kicad_sch") != 0) continue;
            char fpath[1024];
            snprintf(fpath, sizeof(fpath), "%s/%s", path, ent->d_name);
            bom_load_file(fpath, &all, &count, &cap, &to_free, &free_count, &free_cap);
        }
        closedir(dir);
#endif
    } else {
        bom_load_file(path, &all, &count, &cap, &to_free, &free_count, &free_cap);
    }

    if (count == 0) {
        fprintf(stderr, "error: no BOM components found\n");
        free(all);
        for (size_t i = 0; i < free_count; i++) kicli_sch_free(to_free[i]);
        free(to_free);
        return 1;
    }

    FILE *f = out_path ? fopen(out_path, "w") : stdout;
    if (!f) {
        fprintf(stderr, "error: cannot open %s\n", out_path);
        free(all);
        for (size_t i = 0; i < free_count; i++) kicli_sch_free(to_free[i]);
        free(to_free);
        return 1;
    }

    fprintf(f, "Comment,Designator,Footprint,LCSC\n");

    int total_rows = 0, missing_lcsc = 0;
    bom_write_symbols(f, all, count, &total_rows, &missing_lcsc);

    if (out_path) fclose(f);
    free(all);
    for (size_t i = 0; i < free_count; i++) kicli_sch_free(to_free[i]);
    free(to_free);

    if (total_rows > 0)
        fprintf(stderr, "%d row(s), %d missing PartNo\n", total_rows, missing_lcsc);

    return 0;
}

/* ── entry point ─────────────────────────────────────────────────────────── */

int cmd_jlcpcb(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        printf("Usage: kicli jlcpcb <command> [args]\n\n");
        printf("Commands:\n");
        printf("  part   <LCSC_ID>             Lookup part detail (brand, model, package, stock, price)\n");
        printf("                               Output: key-value lines (Brand: ..., Model: ..., etc.)\n");
        printf("  search <query> [options]     Search JLCPCB catalog (sorted by stock)\n");
        printf("    -n N                       Max results (default 10)\n");
        printf("    --basic                    Only basic parts (no extra assembly fee)\n");
        printf("    --extended                 Only extended parts\n");
        printf("    --in-stock                 Only parts currently in stock\n");
        printf("    --package <PKG>            Filter by package (e.g. 0402, 0603, LQFP-48)\n");
        printf("                               Output: LCSC, Type, Model, Package, Stock, Description\n");
        printf("                               Type: base = basic part (no extra fee), expand = extended\n");
        printf("  bom    <sch|dir> [-o out.csv] Generate JLCPCB-ready BOM CSV\n");
        printf("                               Accepts file or directory (merges all .kicad_sch)\n");
        printf("                               Columns: Comment,Designator,Footprint,LCSC\n");
        printf("                               Groups by value+footprint, shows missing PartNo count\n");
        printf("\nExamples:\n");
        printf("  kicli jlcpcb part C2040\n");
        printf("  kicli jlcpcb search \"100nF 0402\"\n");
        printf("  kicli jlcpcb search \"100nF\" --basic --in-stock --package 0402\n");
        printf("  kicli jlcpcb search \"RP2040\" --in-stock -n 5\n");
        printf("  kicli jlcpcb bom board.kicad_sch -o bom.csv\n");
        printf("\nWorkflow — assign missing part numbers:\n");
        printf("  1. kicli jlcpcb bom board.kicad_sch           # find rows with empty PartNo\n");
        printf("  2. kicli jlcpcb search \"<value> <package>\"     # find LCSC code\n");
        printf("  3. kicli sch board.kicad_sch set C1 LCSC C1525 # assign to one component\n");
        printf("     or: kicli sch proj/ set-all \"100nF\" LCSC C1525  # bulk assign by value\n");
        printf("  4. kicli jlcpcb bom board.kicad_sch -o bom.csv    # export final BOM\n");
        return 0;
    }

    if (strcmp(argv[1], "part") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: kicli jlcpcb part <LCSC_ID>\n"); return 1; }
        return jlcpcb_part(argv[2]);
    }

    if (strcmp(argv[1], "search") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: kicli jlcpcb search <query> [--basic|--extended] [--in-stock] [--package PKG] [-n N]\n"); return 1; }
        jlcpcb_search_opts_t opts = { .keyword = argv[2], .page_size = 10 };
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) opts.page_size = atoi(argv[++i]);
            else if (strcmp(argv[i], "--basic") == 0)        opts.type = "base";
            else if (strcmp(argv[i], "--extended") == 0)      opts.type = "expand";
            else if (strcmp(argv[i], "--in-stock") == 0)      opts.in_stock = 1;
            else if (strcmp(argv[i], "--package") == 0 && i + 1 < argc) opts.package = argv[++i];
        }
        return jlcpcb_search(&opts);
    }

    if (strcmp(argv[1], "bom") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: kicli jlcpcb bom <sch|dir> [-o out.csv]\n"); return 1; }
        const char *out = NULL;
        for (int i = 3; i < argc - 1; i++)
            if (strcmp(argv[i], "-o") == 0) out = argv[i + 1];
        return jlcpcb_bom(argv[2], out);
    }

    fprintf(stderr, "error: unknown jlcpcb command '%s'\n", argv[1]);
    return 1;
}
