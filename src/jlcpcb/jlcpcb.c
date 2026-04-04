/* jlcpcb.c — JLCPCB integration: part lookup + BOM generation */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#  define strcasecmp _stricmp
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

static int jlcpcb_search(const char *keyword, int page_size)
{
    CURL *curl = curl_easy_init();
    if (!curl) { fprintf(stderr, "error: curl init failed\n"); return 1; }

    char url[512];
    /* URL-encode keyword manually (spaces → +) */
    char kw_enc[256] = {0};
    for (int i = 0, j = 0; keyword[i] && j < (int)sizeof(kw_enc) - 3; i++) {
        if (keyword[i] == ' ') kw_enc[j++] = '+';
        else                   kw_enc[j++] = keyword[i];
    }

    snprintf(url, sizeof(url),
        "https://jlcpcb.com/api/overseas-pcb-order/v1/shoppingCart/smtGood"
        "/selectSmtComponentList");

    char body[512];
    snprintf(body, sizeof(body),
        "{\"keyword\":\"%s\",\"currentPage\":1,\"pageSize\":%d}",
        kw_enc, page_size);

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

    /* print each result: componentCode  componentModelEn  componentSpecificationEn  stockCount */
    printf("%-10s  %-30s  %-20s  %s\n", "LCSC", "Model", "Package", "Stock");
    printf("%-10s  %-30s  %-20s  %s\n", "----", "-----", "-------", "-----");

    const char *p = buf.data;
    while ((p = strstr(p, "\"componentCode\"")) != NULL) {
        int l; const char *v;
        char code[32]="", model[64]="", pkg[32]="", stock[32]="";

        v = json_str(p, "componentCode",           &l); if (v) snprintf(code,  sizeof(code),  "%.*s", l, v);
        v = json_str(p, "componentModelEn",         &l); if (v) snprintf(model, sizeof(model), "%.*s", l, v);
        v = json_str(p, "componentSpecificationEn", &l); if (v) snprintf(pkg,   sizeof(pkg),   "%.*s", l, v);
        v = json_str(p, "stockCount",               &l); if (v) snprintf(stock, sizeof(stock), "%.*s", l, v);

        printf("%-10s  %-30s  %-20s  %s\n", code, model, pkg, stock);
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

static int jlcpcb_bom(const char *sch_path, const char *out_path)
{
    kicli_schematic_t *sch = NULL;
    if (kicli_sch_read(sch_path, &sch) != KICLI_OK) {
        fprintf(stderr, "error: %s\n", kicli_last_error());
        return 1;
    }

    FILE *f = out_path ? fopen(out_path, "w") : stdout;
    if (!f) { fprintf(stderr, "error: cannot open %s\n", out_path); kicli_sch_free(sch); return 1; }

    fprintf(f, "Comment,Designator,Footprint,LCSC\n");

    /* visited flags for grouping */
    int *seen = calloc(sch->num_symbols, sizeof(int));

    for (size_t i = 0; i < sch->num_symbols; i++) {
        const kicli_symbol_t *a = &sch->symbols[i];
        if (seen[i] || !a->in_bom || a->reference[0] == '#') continue;

        const char *lcsc_a = find_lcsc(a);
        const char *fp_a   = a->footprint ? a->footprint : "";
        const char *val_a  = a->value     ? a->value     : "";

        /* collect matching designators */
        char desig[4096] = "";
        strncat(desig, a->reference, sizeof(desig) - 1);
        seen[i] = 1;

        for (size_t j = i + 1; j < sch->num_symbols; j++) {
            const kicli_symbol_t *b = &sch->symbols[j];
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
    }

    free(seen);
    if (out_path) fclose(f);
    kicli_sch_free(sch);
    return 0;
}

/* ── entry point ─────────────────────────────────────────────────────────── */

int cmd_jlcpcb(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "--help") == 0) {
        printf("Usage: kicli jlcpcb <command> [args]\n\n");
        printf("  part   <LCSC_ID>              Get component detail from JLCPCB\n");
        printf("  search <query> [-n N]          Search JLCPCB parts catalog (default 10)\n");
        printf("  bom    <sch> [-o out.csv]      Generate JLCPCB BOM CSV from schematic\n");
        printf("\nExamples:\n");
        printf("  kicli jlcpcb part C2040\n");
        printf("  kicli jlcpcb search \"100nF 0402\"\n");
        printf("  kicli jlcpcb search \"RP2040\" -n 5\n");
        printf("  kicli jlcpcb bom board.kicad_sch\n");
        printf("  kicli jlcpcb bom board.kicad_sch -o jlcpcb_bom.csv\n");
        printf("\nWorkflow (add missing LCSC numbers):\n");
        printf("  kicli jlcpcb bom board.kicad_sch          # find empty LCSC column\n");
        printf("  kicli jlcpcb search \"100nF 0402\"           # find the part\n");
        printf("  kicli sch board.kicad_sch set C1 LCSC C14663  # write it back\n");
        return 0;
    }

    if (strcmp(argv[1], "part") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: kicli jlcpcb part <LCSC_ID>\n"); return 1; }
        return jlcpcb_part(argv[2]);
    }

    if (strcmp(argv[1], "search") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: kicli jlcpcb search <query> [-n N]\n"); return 1; }
        int n = 10;
        for (int i = 3; i < argc - 1; i++)
            if (strcmp(argv[i], "-n") == 0) n = atoi(argv[i + 1]);
        return jlcpcb_search(argv[2], n);
    }

    if (strcmp(argv[1], "bom") == 0) {
        if (argc < 3) { fprintf(stderr, "Usage: kicli jlcpcb bom <sch_file> [-o out.csv]\n"); return 1; }
        const char *out = NULL;
        for (int i = 3; i < argc - 1; i++)
            if (strcmp(argv[i], "-o") == 0) out = argv[i + 1];
        return jlcpcb_bom(argv[2], out);
    }

    fprintf(stderr, "error: unknown jlcpcb command '%s'\n", argv[1]);
    return 1;
}
