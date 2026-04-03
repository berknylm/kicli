/* sch.c — stub dispatcher */
#include <stdio.h>
#include <string.h>
#include "kicli/sch.h"
#include "kicli/config.h"
#define OPTPARSE_IMPLEMENTATION
#define OPTPARSE_API static
#include "optparse.h"

int cmd_sch(int argc, char **argv, const kicli_config_t *cfg) {
    (void)cfg;
    if (argc < 2 || strcmp(argv[1], "--help") == 0) {
        printf("Usage: kicli sch read <FILE>\n");
        printf("       kicli sch tree <FILE>\n");
        printf("       kicli sch dump <FILE> [--format json|yaml|tree]\n");
        printf("       kicli sch add <FILE> <LIB:SYM> --ref R1 --value 10k\n");
        printf("       kicli sch remove <FILE> <REF>\n");
        printf("       kicli sch move <FILE> <REF> <X> <Y>\n");
        printf("       kicli sch connect <FILE> <PIN_A> <PIN_B>\n");
        printf("       kicli sch validate <FILE>\n");
        printf("       kicli sch diff <FILE_A> <FILE_B>\n");
        printf("       kicli sch export <FILE> --format pdf\n");
        return 0;
    }
    printf("\x1b[33mnot yet implemented: sch\x1b[0m\n");
    return 0;
}
