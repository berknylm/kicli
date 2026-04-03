/*
 * snapeda.c — SnapEDA source implementation
 *
 * Uses the SnapEDA public search API. Full downloads require authentication,
 * so fetch returns a descriptive message with the part URL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kicli/fetch.h"
#include "kicli/error.h"
#include "kicli/config.h"
#include "cJSON.h"
#include <curl/curl.h>

/* ── HTTP helper ────────────────────────────────────────────────────────── */

struct snapeda_dyn_buf { char *data; size_t size; };

static size_t snapeda_write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    struct snapeda_dyn_buf *b = ud;
    size_t n = size * nmemb;
    char *tmp = realloc(b->data, b->size + n + 1);
    if (!tmp) return 0;
    b->data = tmp;
    memcpy(b->data + b->size, ptr, n);
    b->size += n;
    b->data[b->size] = '\0';
    return n;
}

static char *snapeda_http_get(const char *url) {
    struct snapeda_dyn_buf buf = {NULL, 0};
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, snapeda_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "kicli/0.1");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        kicli_set_error("HTTP request failed: %s", curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

/* URL-encode a string (simple version for query params) */
static char *url_encode(const char *s) {
    CURL *curl = curl_easy_init();
    if (!curl) return strdup(s);
    char *enc = curl_easy_escape(curl, s, 0);
    char *result = enc ? strdup(enc) : strdup(s);
    if (enc) curl_free(enc);
    curl_easy_cleanup(curl);
    return result;
}

/* ── API endpoints ──────────────────────────────────────────────────────── */

#define SNAPEDA_SEARCH_API \
    "https://www.snapeda.com/api/v1/parts/search/?q=%s&has_symbol=true&has_footprint=true"

#define SNAPEDA_PART_URL \
    "https://www.snapeda.com/parts/search/?q=%s"

/* ── snapeda_fetch ──────────────────────────────────────────────────────── */

kicli_err_t snapeda_fetch(const char *id, const kicli_config_t *cfg,
                            kicli_component_assets_t **out) {
    (void)cfg;

    /* Search for the part to get a URL */
    char *enc = url_encode(id);
    char url[1024];
    snprintf(url, sizeof(url), SNAPEDA_SEARCH_API, enc);
    free(enc);

    printf("  Searching SnapEDA for %s...\n", id);
    char *resp = snapeda_http_get(url);

    char part_url[512];
    char *enc2 = url_encode(id);
    snprintf(part_url, sizeof(part_url), SNAPEDA_PART_URL, enc2);
    free(enc2);

    if (resp) {
        cJSON *root = cJSON_Parse(resp);
        free(resp);
        if (root) {
            cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
            if (results && cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) {
                cJSON *first = cJSON_GetArrayItem(results, 0);
                if (first) {
                    cJSON *snap_url = cJSON_GetObjectItemCaseSensitive(first, "url");
                    if (snap_url && cJSON_IsString(snap_url)) {
                        snprintf(part_url, sizeof(part_url), "%s", snap_url->valuestring);
                    }
                }
            }
            cJSON_Delete(root);
        }
    }

    printf("\x1b[33mSnapEDA found part. Download not yet supported without API key.\x1b[0m\n");
    printf("  Part URL: %s\n", part_url);
    printf("  To use this component:\n");
    printf("    1. Download the KiCad files manually from the URL above\n");
    printf("    2. Use 'kicli fetch --from zipfile <path>' (coming soon)\n");

    KICLI_FAIL(KICLI_ERR_UNSUPPORTED,
        "SnapEDA does not support automated download without an API key. "
        "Visit: %s", part_url);
}

/* ── snapeda_search ─────────────────────────────────────────────────────── */

kicli_err_t snapeda_search(const char *query, const kicli_config_t *cfg,
                             char **out_json) {
    (void)cfg;

    char *enc = url_encode(query);
    char url[1024];
    snprintf(url, sizeof(url), SNAPEDA_SEARCH_API, enc);
    free(enc);

    printf("  Searching SnapEDA...\n");
    char *resp = snapeda_http_get(url);
    if (!resp) {
        return KICLI_ERR_HTTP;
    }

    *out_json = resp;
    return KICLI_OK;
}

/* ── snapeda_info ───────────────────────────────────────────────────────── */

kicli_err_t snapeda_info(const char *id, const kicli_config_t *cfg,
                          char **out_json) {
    (void)cfg;

    char *enc = url_encode(id);
    char url[1024];
    snprintf(url, sizeof(url), SNAPEDA_SEARCH_API, enc);
    free(enc);

    char *resp = snapeda_http_get(url);
    if (!resp) {
        return KICLI_ERR_HTTP;
    }

    *out_json = resp;
    return KICLI_OK;
}

/* ── Source descriptor ──────────────────────────────────────────────────── */

const kicli_source_t snapeda_source = {
    .name   = "snapeda",
    .fetch  = snapeda_fetch,
    .search = snapeda_search,
    .info   = snapeda_info,
};
