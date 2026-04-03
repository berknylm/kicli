#pragma once
#ifndef KICLI_FETCH_H
#define KICLI_FETCH_H

#include <stdbool.h>
#include "kicli/error.h"
#include "kicli/config.h"

/* Component assets downloaded from a source */
typedef struct {
    char *symbol_kicad_sym;  /* .kicad_sym file contents (heap, may be NULL) */
    char *footprint_mod;     /* .kicad_mod file contents (heap, may be NULL) */
    char *model_step_path;   /* path to downloaded .step file (heap, may be NULL) */
    char  component_id[128];
    char  name[256];
    char  description[512];
    char  source[64];        /* "lcsc", "digikey", "snapeda" */
} kicli_component_assets_t;

void kicli_component_assets_free(kicli_component_assets_t *a);

/* ── Source interface ────────────────────────────────────────────────────── */

typedef kicli_err_t (*kicli_source_fetch_fn)(const char *id, const kicli_config_t *cfg,
                                              kicli_component_assets_t **out);
typedef kicli_err_t (*kicli_source_search_fn)(const char *query, const kicli_config_t *cfg,
                                               char **out_json);
typedef kicli_err_t (*kicli_source_info_fn)(const char *id, const kicli_config_t *cfg,
                                             char **out_json);

typedef struct {
    const char           *name;
    kicli_source_fetch_fn  fetch;
    kicli_source_search_fn search;
    kicli_source_info_fn   info;
} kicli_source_t;

/* Returns registered source by name, or NULL */
const kicli_source_t *kicli_source_get(const char *name);
/* Print all registered source names */
void kicli_sources_list(void);

/* ── Importer ────────────────────────────────────────────────────────────── */

/* Write assets into KiCad library files under lib_dir.
   symbol_lib / footprint_lib override config values when non-NULL. */
kicli_err_t kicli_import_component(const kicli_component_assets_t *assets,
                                    const kicli_config_t *cfg,
                                    const char *lib_name_override); /* NULL → use config */

/* ── Registry ────────────────────────────────────────────────────────────── */

/* A record of a fetched component stored in .kicli/registry.json */
typedef struct {
    char id[128];
    char source[64];
    char name[256];
    char lib_name[256];
    char fetched_at[32]; /* ISO-8601 */
} kicli_registry_entry_t;

kicli_err_t kicli_registry_add(const kicli_registry_entry_t *entry);
kicli_err_t kicli_registry_remove(const char *id, const char *source);
kicli_err_t kicli_registry_list(kicli_registry_entry_t **out, size_t *count_out);
void        kicli_registry_free_list(kicli_registry_entry_t *list, size_t count);

/* ── High-level commands ─────────────────────────────────────────────────── */

kicli_err_t kicli_fetch(const char *id, const char *source_name,
                         const char *lib_override, const kicli_config_t *cfg);
kicli_err_t kicli_fetch_from_csv(const char *csv_path, const char *source_name,
                                   const kicli_config_t *cfg);
kicli_err_t kicli_fetch_sync(bool dry_run, const kicli_config_t *cfg);

#endif /* KICLI_FETCH_H */
