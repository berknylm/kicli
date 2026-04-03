#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "kicli/config.h"
#include "kicli/error.h"
#include "toml.h"

static kicli_config_t _global_cfg;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Copy a toml string datum into a fixed-size buffer and free the heap copy. */
static void apply_str(char *dst, size_t dstsz, toml_datum_t d) {
    if (!d.ok) return;
    strncpy(dst, d.u.s, dstsz - 1);
    dst[dstsz - 1] = '\0';
    free(d.u.s);
}

/* Copy a toml bool datum into a bool field. */
static void apply_bool(bool *dst, toml_datum_t d) {
    if (!d.ok) return;
    *dst = (bool)d.u.b;
}

/* ------------------------------------------------------------------ */
/* Per-section merge helpers (parsed values override dst)              */
/* ------------------------------------------------------------------ */

static void merge_project(kicli_config_t *dst, const toml_table_t *root) {
    const toml_table_t *t = toml_table_in(root, "project");
    if (!t) return;
    apply_str(dst->project.kicad_version, sizeof(dst->project.kicad_version),
              toml_string_in(t, "kicad_version"));
    apply_str(dst->project.lib_dir, sizeof(dst->project.lib_dir),
              toml_string_in(t, "lib_dir"));
}

static void merge_fetch(kicli_config_t *dst, const toml_table_t *root) {
    const toml_table_t *t = toml_table_in(root, "fetch");
    if (!t) return;
    apply_str(dst->fetch.default_source, sizeof(dst->fetch.default_source),
              toml_string_in(t, "default_source"));
    apply_bool(&dst->fetch.auto_3d_models, toml_bool_in(t, "auto_3d_models"));
    apply_str(dst->fetch.symbol_lib, sizeof(dst->fetch.symbol_lib),
              toml_string_in(t, "symbol_lib"));
    apply_str(dst->fetch.footprint_lib, sizeof(dst->fetch.footprint_lib),
              toml_string_in(t, "footprint_lib"));
}

static void merge_stock(kicli_config_t *dst, const toml_table_t *root) {
    const toml_table_t *t = toml_table_in(root, "stock");
    if (!t) return;

    apply_str(dst->stock.currency, sizeof(dst->stock.currency),
              toml_string_in(t, "currency"));
    apply_str(dst->stock.cache_ttl, sizeof(dst->stock.cache_ttl),
              toml_string_in(t, "cache_ttl"));

    /* suppliers = ["lcsc", "digikey", ...] */
    const toml_array_t *sup = toml_array_in(t, "suppliers");
    if (sup) {
        int n = toml_array_nelem(sup);
        if (n > 8) n = 8;
        dst->stock.num_suppliers = n;
        for (int i = 0; i < n; i++) {
            apply_str(dst->stock.suppliers[i],
                      sizeof(dst->stock.suppliers[i]),
                      toml_string_at(sup, i));
        }
    }

    /* [stock.alerts] */
    const toml_table_t *alerts = toml_table_in(t, "alerts");
    if (alerts) {
        apply_str(dst->stock.alerts.notify, sizeof(dst->stock.alerts.notify),
                  toml_string_in(alerts, "notify"));
        apply_str(dst->stock.alerts.webhook_url,
                  sizeof(dst->stock.alerts.webhook_url),
                  toml_string_in(alerts, "webhook_url"));
    }

    /* [stock.api_keys] */
    const toml_table_t *keys = toml_table_in(t, "api_keys");
    if (keys) {
        apply_str(dst->stock.api_keys.lcsc, sizeof(dst->stock.api_keys.lcsc),
                  toml_string_in(keys, "lcsc"));
        apply_str(dst->stock.api_keys.digikey,
                  sizeof(dst->stock.api_keys.digikey),
                  toml_string_in(keys, "digikey"));
        apply_str(dst->stock.api_keys.mouser,
                  sizeof(dst->stock.api_keys.mouser),
                  toml_string_in(keys, "mouser"));
    }
}

static void merge_sch(kicli_config_t *dst, const toml_table_t *root) {
    const toml_table_t *t = toml_table_in(root, "sch");
    if (!t) return;
    apply_str(dst->sch.default_format, sizeof(dst->sch.default_format),
              toml_string_in(t, "default_format"));
    apply_bool(&dst->sch.backup_on_write, toml_bool_in(t, "backup_on_write"));
}

/* ------------------------------------------------------------------ */
/* Merge an entire parsed TOML document into dst.                      */
/* ------------------------------------------------------------------ */

static void merge_toml(kicli_config_t *dst, const toml_table_t *root) {
    merge_project(dst, root);
    merge_fetch(dst, root);
    merge_stock(dst, root);
    merge_sch(dst, root);
}

/* ------------------------------------------------------------------ */
/* Default values                                                       */
/* ------------------------------------------------------------------ */

static void set_defaults(kicli_config_t *c) {
    memset(c, 0, sizeof(*c));

    strncpy(c->project.kicad_version, "8",
            sizeof(c->project.kicad_version) - 1);
    strncpy(c->project.lib_dir, "libs",
            sizeof(c->project.lib_dir) - 1);

    strncpy(c->fetch.default_source, "lcsc",
            sizeof(c->fetch.default_source) - 1);
    c->fetch.auto_3d_models = true;
    strncpy(c->fetch.symbol_lib, "project-symbols",
            sizeof(c->fetch.symbol_lib) - 1);
    strncpy(c->fetch.footprint_lib, "project-footprints",
            sizeof(c->fetch.footprint_lib) - 1);

    strncpy(c->stock.suppliers[0], "lcsc",
            sizeof(c->stock.suppliers[0]) - 1);
    strncpy(c->stock.suppliers[1], "digikey",
            sizeof(c->stock.suppliers[1]) - 1);
    strncpy(c->stock.suppliers[2], "mouser",
            sizeof(c->stock.suppliers[2]) - 1);
    c->stock.num_suppliers = 3;
    strncpy(c->stock.currency, "USD",
            sizeof(c->stock.currency) - 1);
    strncpy(c->stock.cache_ttl, "1h",
            sizeof(c->stock.cache_ttl) - 1);
    strncpy(c->stock.alerts.notify, "terminal",
            sizeof(c->stock.alerts.notify) - 1);

    strncpy(c->sch.default_format, "json",
            sizeof(c->sch.default_format) - 1);
    c->sch.backup_on_write = true;
}

/* ------------------------------------------------------------------ */
/* Load + parse one TOML file and merge into dst.                      */
/* Returns  1 if file was successfully parsed, 0 if skipped/failed.   */
/* ------------------------------------------------------------------ */

static int load_and_merge(kicli_config_t *dst, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        /* File absent — silently skip (not an error). */
        return 0;
    }

    char errbuf[256];
    toml_table_t *root = toml_parse_file(fp, errbuf, (int)sizeof(errbuf));
    fclose(fp);

    if (!root) {
        /* Parse error — record it but continue so the other file can still
           be applied; the caller decides whether to propagate this. */
        kicli_set_error("config parse error in %s: %s", path, errbuf);
        return -1;
    }

    merge_toml(dst, root);
    toml_free(root);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int kicli_config_load(kicli_config_t *out) {
    set_defaults(out);

    /* 1. Global config: ~/.config/kicli/config.toml */
    {
#ifdef _WIN32
        const char *home = getenv("USERPROFILE");
#else
        const char *home = getenv("HOME");
#endif
        if (home) {
            char global_path[1024];
            snprintf(global_path, sizeof(global_path),
                     "%s/.config/kicli/config.toml", home);
            (void)load_and_merge(out, global_path);
        }
    }

    /* 2. Project-local config: ./.kicli.toml (overrides global) */
    (void)load_and_merge(out, ".kicli.toml");

    _global_cfg = *out;
    return KICLI_OK;
}

const kicli_config_t *kicli_config_get(void) { return &_global_cfg; }
