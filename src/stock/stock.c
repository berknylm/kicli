/* stock.c — stub dispatcher */
#include <stdio.h>
#include <string.h>
#include "kicli/stock.h"
#define OPTPARSE_IMPLEMENTATION
#define OPTPARSE_API static
#include "optparse.h"

int cmd_stock(int argc, char **argv, const kicli_config_t *cfg) {
    (void)cfg;
    if (argc < 2 || strcmp(argv[1], "--help") == 0) {
        printf("Usage: kicli stock check <PART...>\n");
        printf("       kicli stock bom <BOM_FILE>\n");
        printf("       kicli stock watch <PART...> [--below N]\n");
        printf("       kicli stock compare <PART>\n");
        printf("       kicli stock export <BOM_FILE>\n");
        return 0;
    }
    printf("\x1b[33mnot yet implemented: stock\x1b[0m\n");
    return 0;
}
