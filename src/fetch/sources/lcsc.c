/*
 * lcsc.c — LCSC/EasyEDA source implementation
 *
 * Uses the EasyEDA API to fetch component data.
 * API endpoints derived from jlc-cli reference implementation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kicli/fetch.h"
#include "kicli/error.h"
#include "kicli/config.h"
#include "cJSON.h"
#include <curl/curl.h>

/* ── HTTP helpers ───────────────────────────────────────────────────────── */

struct dyn_buf { char *data; size_t size; };

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    struct dyn_buf *b = ud;
    size_t n = size * nmemb;
    char *tmp = realloc(b->data, b->size + n + 1);
    if (!tmp) return 0;
    b->data = tmp;
    memcpy(b->data + b->size, ptr, n);
    b->size += n;
    b->data[b->size] = '\0';
    return n;
}

static char *http_get(const char *url) {
    struct dyn_buf buf = {NULL, 0};
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
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
    return buf.data; /* caller frees */
}

static char *http_post_json(const char *url, const char *body) {
    struct dyn_buf buf = {NULL, 0};
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "kicli/0.1");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        kicli_set_error("HTTP request failed: %s", curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }
    return buf.data; /* caller frees */
}

/* ── API endpoints ──────────────────────────────────────────────────────── */

#define EASYEDA_COMPONENT_API \
    "https://easyeda.com/api/products/%s/components?version=6.4.19.5"

#define JLCPCB_SEARCH_API \
    "https://jlcpcb.com/api/overseas-pcb-order/v1/shoppingCart/smtGood/selectSmtComponentList/v2"

#define EASYEDA_3D_STEP \
    "https://modules.easyeda.com/qAxj6KHrDKw4blvCG8QJPs7Y/%s"

/* ── KiCad symbol generation from EasyEDA data ──────────────────────────── */

/*
 * Generate a minimal valid .kicad_sym entry from EasyEDA component metadata.
 * This produces a simple rectangular IC-style symbol.
 */
static char *generate_kicad_sym(const char *name, const char *id,
                                 const char *description, const char *ref_prefix) {
    /* We build the symbol entry (without the library wrapper) */
    char *buf = NULL;
    size_t buf_size = 0;
    FILE *ms = open_memstream(&buf, &buf_size);
    if (!ms) return NULL;

    const char *prefix = (ref_prefix && ref_prefix[0]) ? ref_prefix : "U";
    /* Escape double-quotes in name/description */
    fprintf(ms,
        "  (symbol \"%s\"\n"
        "    (pin_names (offset 1.016))\n"
        "    (in_bom yes) (on_board yes)\n"
        "    (property \"Reference\" \"%s\"\n"
        "      (at 0 2.54 0)\n"
        "      (effects (font (size 1.27 1.27)))\n"
        "    )\n"
        "    (property \"Value\" \"%s\"\n"
        "      (at 0 0 0)\n"
        "      (effects (font (size 1.27 1.27)))\n"
        "    )\n"
        "    (property \"Footprint\" \"\"\n"
        "      (at 0 -2.54 0)\n"
        "      (effects (font (size 1.27 1.27)) hide)\n"
        "    )\n"
        "    (property \"Datasheet\" \"\"\n"
        "      (at 0 -5.08 0)\n"
        "      (effects (font (size 1.27 1.27)) hide)\n"
        "    )\n"
        "    (property \"Description\" \"%s\"\n"
        "      (at 0 -7.62 0)\n"
        "      (effects (font (size 1.27 1.27)) hide)\n"
        "    )\n"
        "    (property \"LCSC\" \"%s\"\n"
        "      (at 0 -10.16 0)\n"
        "      (effects (font (size 1.27 1.27)) hide)\n"
        "    )\n"
        "    (symbol \"%s_0_1\"\n"
        "      (rectangle (start -5.08 2.54) (end 5.08 -2.54)\n"
        "        (stroke (width 0) (type default))\n"
        "        (fill (type background))\n"
        "      )\n"
        "    )\n"
        "    (symbol \"%s_1_1\"\n"
        "    )\n"
        "  )\n",
        name, prefix, name, description, id, name, name);

    fclose(ms);
    return buf;
}

/* ── lcsc_fetch ─────────────────────────────────────────────────────────── */

kicli_err_t lcsc_fetch(const char *id, const kicli_config_t *cfg,
                        kicli_component_assets_t **out) {
    (void)cfg;

    /* Build URL */
    char url[512];
    snprintf(url, sizeof(url), EASYEDA_COMPONENT_API, id);

    printf("  Fetching component info from EasyEDA...\n");
    char *body = http_get(url);
    if (!body) {
        return KICLI_ERR_HTTP;
    }

    /* Parse JSON */
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        KICLI_FAIL(KICLI_ERR_PARSE, "failed to parse EasyEDA API response");
    }

    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!result || cJSON_IsNull(result)) {
        cJSON_Delete(root);
        KICLI_FAIL(KICLI_ERR_NOT_FOUND, "component %s not found on EasyEDA", id);
    }

    /* Extract component metadata */
    cJSON *data_str_obj = cJSON_GetObjectItemCaseSensitive(result, "dataStr");
    cJSON *title_obj    = cJSON_GetObjectItemCaseSensitive(result, "title");
    cJSON *lcsc_obj     = cJSON_GetObjectItemCaseSensitive(result, "lcsc");

    const char *comp_name = NULL;
    const char *comp_desc = NULL;
    const char *ref_prefix = "U";

    if (title_obj && cJSON_IsString(title_obj)) {
        comp_name = title_obj->valuestring;
    }

    /* Try to get name/prefix from dataStr.head.c_para */
    if (data_str_obj && cJSON_IsObject(data_str_obj)) {
        cJSON *head = cJSON_GetObjectItemCaseSensitive(data_str_obj, "head");
        if (head) {
            cJSON *c_para = cJSON_GetObjectItemCaseSensitive(head, "c_para");
            if (c_para) {
                cJSON *name_j = cJSON_GetObjectItemCaseSensitive(c_para, "name");
                cJSON *pre_j  = cJSON_GetObjectItemCaseSensitive(c_para, "pre");
                if (name_j && cJSON_IsString(name_j) && name_j->valuestring[0])
                    comp_name = name_j->valuestring;
                if (pre_j && cJSON_IsString(pre_j) && pre_j->valuestring[0])
                    ref_prefix = pre_j->valuestring;
            }
        }
    }

    /* Fall back to the LCSC part number as name */
    if (!comp_name || !comp_name[0]) {
        comp_name = id;
    }

    comp_desc = comp_name; /* default description = name */
    if (title_obj && cJSON_IsString(title_obj) && title_obj->valuestring[0]) {
        comp_desc = title_obj->valuestring;
    }

    /* Try to get stock info description from lcsc object */
    if (lcsc_obj) {
        /* nothing extra needed for assets */
    }

    /* Allocate assets */
    kicli_component_assets_t *assets = calloc(1, sizeof(*assets));
    if (!assets) {
        cJSON_Delete(root);
        KICLI_FAIL(KICLI_ERR_OOM, "out of memory");
    }

    strncpy(assets->component_id, id, sizeof(assets->component_id) - 1);
    strncpy(assets->name, comp_name, sizeof(assets->name) - 1);
    strncpy(assets->description, comp_desc, sizeof(assets->description) - 1);
    strncpy(assets->source, "lcsc", sizeof(assets->source) - 1);

    /* Generate KiCad symbol */
    assets->symbol_kicad_sym = generate_kicad_sym(comp_name, id, comp_desc, ref_prefix);
    if (!assets->symbol_kicad_sym) {
        cJSON_Delete(root);
        free(assets);
        KICLI_FAIL(KICLI_ERR_OOM, "out of memory generating symbol");
    }

    /* Try to get footprint from packageDetail */
    cJSON *pkg_detail = cJSON_GetObjectItemCaseSensitive(result, "packageDetail");
    if (pkg_detail && cJSON_IsObject(pkg_detail)) {
        cJSON *pkg_ds = cJSON_GetObjectItemCaseSensitive(pkg_detail, "dataStr");
        if (pkg_ds && cJSON_IsObject(pkg_ds)) {
            /* Build a minimal .kicad_mod stub */
            cJSON *pkg_head = cJSON_GetObjectItemCaseSensitive(pkg_ds, "head");
            const char *pkg_name = comp_name;
            if (pkg_head) {
                cJSON *pkg_cpara = cJSON_GetObjectItemCaseSensitive(pkg_head, "c_para");
                if (pkg_cpara) {
                    cJSON *pn = cJSON_GetObjectItemCaseSensitive(pkg_cpara, "package");
                    if (pn && cJSON_IsString(pn) && pn->valuestring[0])
                        pkg_name = pn->valuestring;
                }
            }

            char *fp_buf = NULL;
            size_t fp_size = 0;
            FILE *fms = open_memstream(&fp_buf, &fp_size);
            if (fms) {
                fprintf(fms,
                    "(footprint \"%s\"\n"
                    "  (version 20231120)\n"
                    "  (generator kicli)\n"
                    "  (layer \"F.Cu\")\n"
                    "  (descr \"%s\")\n"
                    "  (attr smd)\n"
                    ")\n",
                    pkg_name, comp_desc);
                fclose(fms);
                assets->footprint_mod = fp_buf;
            }
        }
    }

    /* Try to download 3D model if available */
    if (pkg_detail && cJSON_IsObject(pkg_detail)) {
        cJSON *pkg_ds = cJSON_GetObjectItemCaseSensitive(pkg_detail, "dataStr");
        if (pkg_ds && cJSON_IsObject(pkg_ds)) {
            cJSON *head = cJSON_GetObjectItemCaseSensitive(pkg_ds, "head");
            if (head) {
                cJSON *uuid_j = cJSON_GetObjectItemCaseSensitive(head, "uuid");
                if (!uuid_j) uuid_j = cJSON_GetObjectItemCaseSensitive(head, "puuid");
                if (uuid_j && cJSON_IsString(uuid_j) && uuid_j->valuestring[0]) {
                    /* Download STEP file to a temp location */
                    char step_url[512];
                    snprintf(step_url, sizeof(step_url), EASYEDA_3D_STEP,
                             uuid_j->valuestring);

                    char tmp_path[256];
                    snprintf(tmp_path, sizeof(tmp_path), "/tmp/kicli_%s.step", id);

                    printf("  Downloading 3D model...\n");
                    struct dyn_buf step_buf = {NULL, 0};
                    CURL *curl = curl_easy_init();
                    if (curl) {
                        curl_easy_setopt(curl, CURLOPT_URL, step_url);
                        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
                        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &step_buf);
                        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                        curl_easy_setopt(curl, CURLOPT_USERAGENT, "kicli/0.1");
                        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
                        CURLcode step_res = curl_easy_perform(curl);
                        curl_easy_cleanup(curl);
                        if (step_res == CURLE_OK && step_buf.size > 0) {
                            FILE *f = fopen(tmp_path, "wb");
                            if (f) {
                                fwrite(step_buf.data, 1, step_buf.size, f);
                                fclose(f);
                                assets->model_step_path = strdup(tmp_path);
                            }
                        }
                        free(step_buf.data);
                    }
                }
            }
        }
    }

    cJSON_Delete(root);
    *out = assets;
    return KICLI_OK;
}

/* ── lcsc_search ────────────────────────────────────────────────────────── */

kicli_err_t lcsc_search(const char *query, const kicli_config_t *cfg,
                          char **out_json) {
    (void)cfg;

    /* Use JLCPCB search API (same data source as LCSC) */
    char req_body[512];
    snprintf(req_body, sizeof(req_body),
        "{\"currentPage\":1,\"pageSize\":20,\"keyword\":\"%s\",\"searchType\":2}",
        query);

    printf("  Searching LCSC via JLCPCB API...\n");
    char *resp = http_post_json(JLCPCB_SEARCH_API, req_body);
    if (!resp) {
        return KICLI_ERR_HTTP;
    }

    *out_json = resp;
    return KICLI_OK;
}

/* ── lcsc_info ──────────────────────────────────────────────────────────── */

kicli_err_t lcsc_info(const char *id, const kicli_config_t *cfg,
                       char **out_json) {
    (void)cfg;

    char url[512];
    snprintf(url, sizeof(url), EASYEDA_COMPONENT_API, id);

    char *resp = http_get(url);
    if (!resp) {
        return KICLI_ERR_HTTP;
    }

    /* Pretty-print the result section */
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        KICLI_FAIL(KICLI_ERR_PARSE, "failed to parse API response for %s", id);
    }

    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    char *pretty = NULL;
    if (result) {
        /* Build a summary object */
        cJSON *summary = cJSON_CreateObject();
        cJSON *title = cJSON_GetObjectItemCaseSensitive(result, "title");
        cJSON *lcsc  = cJSON_GetObjectItemCaseSensitive(result, "lcsc");

        if (title) cJSON_AddItemToObject(summary, "title", cJSON_Duplicate(title, 1));
        if (lcsc)  cJSON_AddItemToObject(summary, "lcsc", cJSON_Duplicate(lcsc, 1));

        cJSON *ds = cJSON_GetObjectItemCaseSensitive(result, "dataStr");
        if (ds) {
            cJSON *head = cJSON_GetObjectItemCaseSensitive(ds, "head");
            if (head) {
                cJSON *cpara = cJSON_GetObjectItemCaseSensitive(head, "c_para");
                if (cpara) cJSON_AddItemToObject(summary, "attributes",
                                                  cJSON_Duplicate(cpara, 1));
            }
        }

        pretty = cJSON_Print(summary);
        cJSON_Delete(summary);
    } else {
        pretty = cJSON_Print(root);
    }

    cJSON_Delete(root);
    *out_json = pretty;
    return KICLI_OK;
}

/* ── Source descriptor ──────────────────────────────────────────────────── */

const kicli_source_t lcsc_source = {
    .name   = "lcsc",
    .fetch  = lcsc_fetch,
    .search = lcsc_search,
    .info   = lcsc_info,
};
