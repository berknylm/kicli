/* project.c — kicli new <name> — stub, M2 */
#include <stdio.h>
#include <string.h>

int cmd_new(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0) {
        printf("Usage: kicli new <project-name> [directory]\n");
        printf("Creates a new KiCad 10 project with local library structure.\n");
        return 0;
    }
    printf("\x1b[33mnot yet implemented: new\x1b[0m\n");
    return 0;
}
