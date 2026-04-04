/* jlcpcb.c — JLCPCB component lookup via LCSC API */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <curl/curl.h>
#include "kicli/jlcpcb.h"

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

/* ── entry point ─────────────────────────────────────────────────────────── */

int cmd_jlcpcb(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "--help") == 0) {
        printf("Usage: kicli jlcpcb part <LCSC_ID>   Get component detail from JLCPCB\n");
        printf("\nExample:\n");
        printf("  kicli jlcpcb part C2040\n");
        printf("  kicli jlcpcb part C2040 | jq .data\n");
        return 0;
    }

    if (strcmp(argv[1], "part") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: kicli jlcpcb part <LCSC_ID>\n");
            return 1;
        }
        return jlcpcb_part(argv[2]);
    }

    fprintf(stderr, "error: unknown jlcpcb command '%s'\n", argv[1]);
    return 1;
}
