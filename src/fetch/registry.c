/*
 * registry.c — JSON component registry at .kicli/registry.json
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "kicli/fetch.h"
#include "kicli/error.h"
#include "cJSON.h"

#define REGISTRY_DIR  ".kicli"
#define REGISTRY_FILE ".kicli/registry.json"

/* ── Directory helpers ──────────────────────────────────────────────────── */

static int ensure_dir(const char *path) {
#ifdef _WIN32
    int r = _mkdir(path);
#else
    int r = mkdir(path, 0755);
#endif
    if (r != 0 && errno != EEXIST) return -1;
    return 0;
}

/* ── Read the registry JSON array from disk ─────────────────────────────── */

static cJSON *registry_read(void) {
    FILE *f = fopen(REGISTRY_FILE, "r");
    if (!f) {
        /* File doesn't exist yet — return empty array */
        return cJSON_CreateArray();
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz <= 0) {
        fclose(f);
        return cJSON_CreateArray();
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return cJSON_CreateArray();
    }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    buf[nread] = '\0';
    fclose(f);

    cJSON *arr = cJSON_Parse(buf);
    free(buf);
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return cJSON_CreateArray();
    }
    return arr;
}

/* ── Write the JSON array back to disk ──────────────────────────────────── */

static kicli_err_t registry_write(const cJSON *arr) {
    if (ensure_dir(REGISTRY_DIR) != 0) {
        KICLI_FAIL(KICLI_ERR_IO, "cannot create directory: %s", REGISTRY_DIR);
    }

    char *text = cJSON_Print(arr);
    if (!text) {
        KICLI_FAIL(KICLI_ERR_OOM, "out of memory");
    }

    FILE *f = fopen(REGISTRY_FILE, "w");
    if (!f) {
        free(text);
        KICLI_FAIL(KICLI_ERR_IO, "cannot write registry: %s", REGISTRY_FILE);
    }
    fputs(text, f);
    fclose(f);
    free(text);
    return KICLI_OK;
}

/* ── kicli_registry_add ─────────────────────────────────────────────────── */

kicli_err_t kicli_registry_add(const kicli_registry_entry_t *entry) {
    if (ensure_dir(REGISTRY_DIR) != 0) {
        KICLI_FAIL(KICLI_ERR_IO, "cannot create directory: %s", REGISTRY_DIR);
    }

    cJSON *arr = registry_read();

    /* Build entry object */
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id",         entry->id);
    cJSON_AddStringToObject(obj, "source",     entry->source);
    cJSON_AddStringToObject(obj, "name",       entry->name);
    cJSON_AddStringToObject(obj, "lib_name",   entry->lib_name);
    cJSON_AddStringToObject(obj, "fetched_at", entry->fetched_at);

    cJSON_AddItemToArray(arr, obj);

    kicli_err_t rc = registry_write(arr);
    cJSON_Delete(arr);
    return rc;
}

/* ── kicli_registry_remove ──────────────────────────────────────────────── */

kicli_err_t kicli_registry_remove(const char *id, const char *source) {
    cJSON *arr = registry_read();
    cJSON *new_arr = cJSON_CreateArray();

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        cJSON *id_j  = cJSON_GetObjectItemCaseSensitive(item, "id");
        cJSON *src_j = cJSON_GetObjectItemCaseSensitive(item, "source");

        int id_match  = id_j  && cJSON_IsString(id_j)  && strcmp(id_j->valuestring, id) == 0;
        int src_match = src_j && cJSON_IsString(src_j) && strcmp(src_j->valuestring, source) == 0;

        if (!(id_match && src_match)) {
            cJSON_AddItemToArray(new_arr, cJSON_Duplicate(item, 1));
        }
    }

    cJSON_Delete(arr);
    kicli_err_t rc = registry_write(new_arr);
    cJSON_Delete(new_arr);
    return rc;
}

/* ── kicli_registry_list ────────────────────────────────────────────────── */

kicli_err_t kicli_registry_list(kicli_registry_entry_t **out, size_t *count_out) {
    cJSON *arr = registry_read();
    int n = cJSON_GetArraySize(arr);

    if (n == 0) {
        cJSON_Delete(arr);
        *out = NULL;
        *count_out = 0;
        return KICLI_OK;
    }

    kicli_registry_entry_t *list = calloc((size_t)n, sizeof(*list));
    if (!list) {
        cJSON_Delete(arr);
        KICLI_FAIL(KICLI_ERR_OOM, "out of memory");
    }

    int i = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        kicli_registry_entry_t *e = &list[i++];

        cJSON *j;
        j = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (j && cJSON_IsString(j))
            strncpy(e->id, j->valuestring, sizeof(e->id) - 1);

        j = cJSON_GetObjectItemCaseSensitive(item, "source");
        if (j && cJSON_IsString(j))
            strncpy(e->source, j->valuestring, sizeof(e->source) - 1);

        j = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (j && cJSON_IsString(j))
            strncpy(e->name, j->valuestring, sizeof(e->name) - 1);

        j = cJSON_GetObjectItemCaseSensitive(item, "lib_name");
        if (j && cJSON_IsString(j))
            strncpy(e->lib_name, j->valuestring, sizeof(e->lib_name) - 1);

        j = cJSON_GetObjectItemCaseSensitive(item, "fetched_at");
        if (j && cJSON_IsString(j))
            strncpy(e->fetched_at, j->valuestring, sizeof(e->fetched_at) - 1);
    }

    cJSON_Delete(arr);
    *out = list;
    *count_out = (size_t)n;
    return KICLI_OK;
}

/* ── kicli_registry_free_list ───────────────────────────────────────────── */

void kicli_registry_free_list(kicli_registry_entry_t *list, size_t count) {
    (void)count;
    free(list);
}
