#pragma once
#ifndef KICLI_CONFIG_H
#define KICLI_CONFIG_H

#include <stdbool.h>

/* Stock API keys */
typedef struct {
    char lcsc[256];
    char digikey[256];
    char mouser[256];
} kicli_api_keys_t;

/* Stock alerts */
typedef struct {
    char notify[64];       /* "terminal", "webhook" */
    char webhook_url[512];
} kicli_stock_alerts_t;

/* [stock] section */
typedef struct {
    char               suppliers[8][64]; /* up to 8 supplier names */
    int                num_suppliers;
    char               currency[8];
    char               cache_ttl[16];    /* e.g. "1h" */
    kicli_stock_alerts_t alerts;
    kicli_api_keys_t   api_keys;
} kicli_stock_cfg_t;

/* [fetch] section */
typedef struct {
    char default_source[64];
    bool auto_3d_models;
    char symbol_lib[256];
    char footprint_lib[256];
} kicli_fetch_cfg_t;

/* [project] section */
typedef struct {
    char kicad_version[16];
    char lib_dir[256];
} kicli_project_cfg_t;

/* [sch] section */
typedef struct {
    char default_format[16]; /* "json", "yaml", "tree" */
    bool backup_on_write;
} kicli_sch_cfg_t;

/* Top-level config */
typedef struct {
    kicli_project_cfg_t project;
    kicli_fetch_cfg_t   fetch;
    kicli_stock_cfg_t   stock;
    kicli_sch_cfg_t     sch;
} kicli_config_t;

/* Load config: global (~/.config/kicli/config.toml) merged with
   project-local (.kicli.toml). Returns 0 on success. */
int kicli_config_load(kicli_config_t *out);

/* Returns pointer to process-wide loaded config (call kicli_config_load first) */
const kicli_config_t *kicli_config_get(void);

#endif /* KICLI_CONFIG_H */
