/* fetch.c — stub (M6: not yet implemented) */
#include <stdio.h>
#include <string.h>
#include "kicli/fetch.h"

int cmd_fetch(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0) {
        printf("Usage: kicli fetch <LCSC_ID>      Fetch component from LCSC\n");
        printf("       kicli fetch search <query>  Search components\n");
        printf("       kicli fetch list             List fetched components\n");
        return 0;
    }
    fprintf(stderr, "fetch: not yet implemented (coming in M6)\n");
    return 1;
}
