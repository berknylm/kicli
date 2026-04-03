/*
 * digikey.c — DigiKey source stub
 *
 * DigiKey requires OAuth2 authentication which is not yet implemented.
 * This stub registers the source and returns a descriptive error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kicli/fetch.h"
#include "kicli/error.h"
#include "kicli/config.h"

static kicli_err_t digikey_fetch(const char *id, const kicli_config_t *cfg,
                                   kicli_component_assets_t **out) {
    (void)id;
    (void)cfg;
    (void)out;
    KICLI_FAIL(KICLI_ERR_UNSUPPORTED,
        "DigiKey source requires OAuth2 API key — "
        "set digikey key in config or KICLI_DIGIKEY_API_KEY env var");
}

static kicli_err_t digikey_search(const char *query, const kicli_config_t *cfg,
                                    char **out_json) {
    (void)query;
    (void)cfg;
    (void)out_json;
    KICLI_FAIL(KICLI_ERR_UNSUPPORTED,
        "DigiKey source requires OAuth2 API key — "
        "set digikey key in config or KICLI_DIGIKEY_API_KEY env var");
}

static kicli_err_t digikey_info(const char *id, const kicli_config_t *cfg,
                                  char **out_json) {
    (void)id;
    (void)cfg;
    (void)out_json;
    KICLI_FAIL(KICLI_ERR_UNSUPPORTED,
        "DigiKey source requires OAuth2 API key — "
        "set digikey key in config or KICLI_DIGIKEY_API_KEY env var");
}

const kicli_source_t digikey_source = {
    .name   = "digikey",
    .fetch  = digikey_fetch,
    .search = digikey_search,
    .info   = digikey_info,
};
