/* fetch.c — stub dispatcher */
#include <stdio.h>
#include <string.h>
#include "kicli/fetch.h"
#define OPTPARSE_IMPLEMENTATION
#define OPTPARSE_API static
#include "optparse.h"

int cmd_fetch(int argc, char **argv, const kicli_config_t *cfg) {
    (void)cfg;
    if (argc < 2 || strcmp(argv[1], "--help") == 0) {
        printf("Usage: kicli fetch <ID> [--source lcsc] [--lib name]\n");
        printf("       kicli fetch search <QUERY>\n");
        printf("       kicli fetch info <ID>\n");
        printf("       kicli fetch sync [--dry-run]\n");
        printf("       kicli fetch list\n");
        printf("       kicli fetch remove <ID>\n");
        printf("       kicli fetch sources\n");
        return 0;
    }
    printf("\x1b[33mnot yet implemented: fetch\x1b[0m\n");
    return 0;
}
