/* stock.c — stub (M7: not yet implemented) */
#include <stdio.h>
#include <string.h>
#include "kicli/stock.h"

int cmd_stock(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0) {
        printf("Usage: kicli stock <PART> [PART...]   Check stock and pricing\n");
        printf("       kicli stock bom <BOM_FILE>      Check all BOM parts\n");
        return 0;
    }
    fprintf(stderr, "stock: not yet implemented (coming in M7)\n");
    return 1;
}
