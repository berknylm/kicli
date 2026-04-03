/*
 * sexpr_write.c — s-expression node builders and file serializer
 */

#include "kicli/sch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    write_node(f, root, 0);
    fputc('\n', f);
    fclose(f);
    return 0;
}
