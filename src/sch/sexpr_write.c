/*
 * sexpr_write.c — s-expression node builders and file serializer
 */

#include "kicli/sch.h"
#include "kicli/portable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#endif

/* ── Node builders ────────────────────────────────────────────────────────── */

sexpr_t *sexpr_make_atom(const char *val)
{
    sexpr_t *n = (sexpr_t *)calloc(1, sizeof(sexpr_t));
    if (!n) return NULL;
    n->type  = SEXPR_ATOM;
    n->value = strdup(val ? val : "");
    return n;
}

sexpr_t *sexpr_make_str(const char *val)
{
    sexpr_t *n = (sexpr_t *)calloc(1, sizeof(sexpr_t));
    if (!n) return NULL;
    n->type  = SEXPR_STR;
    n->value = strdup(val ? val : "");
    return n;
}

sexpr_t *sexpr_make_list(void)
{
    sexpr_t *n = (sexpr_t *)calloc(1, sizeof(sexpr_t));
    if (!n) return NULL;
    n->type = SEXPR_LIST;
    return n;
}

sexpr_t *sexpr_clone(const sexpr_t *src)
{
    if (!src) return NULL;
    sexpr_t *dst = (sexpr_t *)calloc(1, sizeof(sexpr_t));
    if (!dst) return NULL;
    dst->type = src->type;
    if (src->value) {
        dst->value = strdup(src->value);
        if (!dst->value) { free(dst); return NULL; }
    }
    if (src->num_children > 0) {
        dst->children = (sexpr_t **)malloc(src->num_children * sizeof(sexpr_t *));
        if (!dst->children) { free(dst->value); free(dst); return NULL; }
        dst->num_children = src->num_children;
        for (size_t i = 0; i < src->num_children; i++) {
            dst->children[i] = sexpr_clone(src->children[i]);
        }
    }
    return dst;
}

int sexpr_list_append(sexpr_t *list, sexpr_t *child)
{
    if (!list || list->type != SEXPR_LIST || !child) return 0;
    size_t new_count = list->num_children + 1;
    sexpr_t **tmp = (sexpr_t **)realloc(list->children,
                                         new_count * sizeof(sexpr_t *));
    if (!tmp) return 0;
    list->children = tmp;
    list->children[list->num_children++] = child;
    return 1;
}

/* ── Serializer ───────────────────────────────────────────────────────────── */

/*
 * "Simple" list: all children are ATOM or STR (no nested lists).
 * These are written on a single line: (at 100 100 0)
 */
static int is_simple_list(const sexpr_t *node)
{
    for (size_t i = 0; i < node->num_children; i++) {
        if (node->children[i]->type == SEXPR_LIST) return 0;
    }
    return 1;
}

static void write_node(FILE *f, const sexpr_t *node, int depth)
{
    if (!node) return;

    if (node->type == SEXPR_ATOM) {
        fputs(node->value, f);
        return;
    }

    if (node->type == SEXPR_STR) {
        fputc('"', f);
        for (const char *p = node->value; *p; p++) {
            if (*p == '"' || *p == '\\') fputc('\\', f);
            else if (*p == '\n') { fputc('\\', f); fputc('n', f); continue; }
            fputc(*p, f);
        }
        fputc('"', f);
        return;
    }

    /* LIST */
    fputc('(', f);

    if (node->num_children == 0) {
        fputc(')', f);
        return;
    }

    if (is_simple_list(node)) {
        /* one line */
        for (size_t i = 0; i < node->num_children; i++) {
            if (i > 0) fputc(' ', f);
            write_node(f, node->children[i], depth);
        }
        fputc(')', f);
        return;
    }

    /* multi-line: first child on same line, rest indented */
    write_node(f, node->children[0], depth + 1);

    for (size_t i = 1; i < node->num_children; i++) {
        fputc('\n', f);
        for (int d = 0; d <= depth; d++) fputc('\t', f);
        write_node(f, node->children[i], depth + 1);
    }
    fputc('\n', f);
    for (int d = 0; d < depth; d++) fputc('\t', f);
    fputc(')', f);
}

int sexpr_write_file(const sexpr_t *root, const char *path)
{
    /* Atomic write: stage to <path>.kicli.<pid>.tmp, then rename over the
     * target. This keeps the original file intact if the process dies
     * mid-write (crash, power loss, SIGKILL, etc). */
    char tmp[KICLI_PATH_MAX];
#ifdef _WIN32
    unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    int n = snprintf(tmp, sizeof(tmp), "%s.kicli.%lu.tmp", path, pid);
    if (n < 0 || (size_t)n >= sizeof(tmp)) return -1;

    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;

    write_node(f, root, 0);
    fputc('\n', f);

    if (fflush(f) != 0 || ferror(f)) {
        fclose(f);
        kicli_unlink(tmp);
        return -1;
    }

#ifndef _WIN32
    int fd = fileno(f);
    if (fd >= 0) (void)fsync(fd);
#endif

    if (fclose(f) != 0) {
        kicli_unlink(tmp);
        return -1;
    }

    if (kicli_rename(tmp, path) != 0) {
        kicli_unlink(tmp);
        return -1;
    }
    return 0;
}
