/*
 * sexpr.c — KiCad s-expression parser
 *
 * Handles: whitespace, ; line comments, (…) lists,
 * quoted strings (\" \\ \n escapes), bare atoms.
 */

#include "kicli/sch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── internal parser state ─────────────────────────────────────────────── */

typedef struct {
    const char *src;
    size_t      pos;
    size_t      len;
    char       *err;
    size_t      err_sz;
} parser_t;

static void parser_init(parser_t *p, const char *input,
                        char *err_out, size_t err_sz)
{
    p->src    = input;
    p->pos    = 0;
    p->len    = strlen(input);
    p->err    = err_out;
    p->err_sz = err_sz;
    if (err_out && err_sz > 0) err_out[0] = '\0';
}

static void parser_error(parser_t *p, const char *msg)
{
    if (p->err && p->err_sz > 0)
        snprintf(p->err, p->err_sz, "%s (at offset %zu)", msg, p->pos);
}

/* skip whitespace and ; comments */
static void skip_ws(parser_t *p)
{
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ';') {
            while (p->pos < p->len && p->src[p->pos] != '\n')
                p->pos++;
        } else if (isspace((unsigned char)c)) {
            p->pos++;
        } else {
            break;
        }
    }
}

/* allocate a new node */
static sexpr_t *node_new(sexpr_type_t type)
{
    sexpr_t *n = (sexpr_t *)calloc(1, sizeof(sexpr_t));
    return n;
    (void)type;
}

static sexpr_t *node_new_typed(sexpr_type_t type)
{
    sexpr_t *n = node_new(type);
    if (n) n->type = type;
    return n;
}

static int list_append(sexpr_t *list, sexpr_t *child)
{
    size_t new_count = list->num_children + 1;
    sexpr_t **tmp = (sexpr_t **)realloc(list->children,
                                         new_count * sizeof(sexpr_t *));
    if (!tmp) return 0;
    list->children = tmp;
    list->children[list->num_children] = child;
    list->num_children = new_count;
    return 1;
}

static sexpr_t *parse_expr(parser_t *p);

/* parse a quoted string: current pos is at opening " */
static sexpr_t *parse_string(parser_t *p)
{
    p->pos++; /* skip " */
    size_t cap = 64;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { parser_error(p, "out of memory"); return NULL; }

    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == '"') {
            p->pos++; /* skip closing " */
            buf[len] = '\0';
            sexpr_t *n = node_new_typed(SEXPR_STR);
            if (!n) { free(buf); return NULL; }
            n->value = buf;
            return n;
        }
        if (c == '\\') {
            p->pos++;
            if (p->pos >= p->len) break;
            char esc = p->src[p->pos];
            switch (esc) {
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case 'n':  c = '\n'; break;
                default:   c = esc;  break;
            }
        }
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = (char *)realloc(buf, cap);
            if (!tmp) { free(buf); parser_error(p, "out of memory"); return NULL; }
            buf = tmp;
        }
        buf[len++] = c;
        p->pos++;
    }

    free(buf);
    parser_error(p, "unterminated string");
    return NULL;
}

/* parse a bare atom (number, keyword, symbol name, etc.) */
static sexpr_t *parse_atom(parser_t *p)
{
    size_t start = p->pos;
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == '(' || c == ')' || c == '"' || isspace((unsigned char)c) || c == ';')
            break;
        p->pos++;
    }
    size_t slen = p->pos - start;
    if (slen == 0) { parser_error(p, "expected atom"); return NULL; }

    sexpr_t *n = node_new_typed(SEXPR_ATOM);
    if (!n) return NULL;
    n->value = (char *)malloc(slen + 1);
    if (!n->value) { free(n); return NULL; }
    memcpy(n->value, p->src + start, slen);
    n->value[slen] = '\0';
    return n;
}

/* parse a list: current pos is at '(' */
static sexpr_t *parse_list(parser_t *p)
{
    p->pos++; /* skip '(' */
    sexpr_t *list = node_new_typed(SEXPR_LIST);
    if (!list) return NULL;

    while (1) {
        skip_ws(p);
        if (p->pos >= p->len) {
            parser_error(p, "unexpected end of input inside list");
            sexpr_free(list);
            return NULL;
        }
        if (p->src[p->pos] == ')') {
            p->pos++;
            return list;
        }
        sexpr_t *child = parse_expr(p);
        if (!child) {
            sexpr_free(list);
            return NULL;
        }
        if (!list_append(list, child)) {
            sexpr_free(child);
            sexpr_free(list);
            parser_error(p, "out of memory");
            return NULL;
        }
    }
}

static sexpr_t *parse_expr(parser_t *p)
{
    skip_ws(p);
    if (p->pos >= p->len) {
        parser_error(p, "unexpected end of input");
        return NULL;
    }
    char c = p->src[p->pos];
    if (c == '(')  return parse_list(p);
    if (c == '"')  return parse_string(p);
    if (c == ')')  { parser_error(p, "unexpected ')'"); return NULL; }
    return parse_atom(p);
}

/* ── public API ────────────────────────────────────────────────────────── */

sexpr_t *sexpr_parse(const char *input, char *err_out, size_t err_sz)
{
    if (!input) return NULL;
    parser_t p;
    parser_init(&p, input, err_out, err_sz);
    skip_ws(&p);
    if (p.pos >= p.len) return NULL;
    sexpr_t *root = parse_expr(&p);
    return root;
}

void sexpr_free(sexpr_t *node)
{
    if (!node) return;
    if (node->value) free(node->value);
    for (size_t i = 0; i < node->num_children; i++)
        sexpr_free(node->children[i]);
    free(node->children);
    free(node);
}

/*
 * sexpr_get: find first child of `list` that is itself a LIST
 * whose first child is an ATOM matching `key`.
 */
sexpr_t *sexpr_get(const sexpr_t *list, const char *key)
{
    if (!list || list->type != SEXPR_LIST || !key) return NULL;
    for (size_t i = 0; i < list->num_children; i++) {
        sexpr_t *child = list->children[i];
        if (!child || child->type != SEXPR_LIST) continue;
        if (child->num_children == 0) continue;
        sexpr_t *head = child->children[0];
        if (head && head->type == SEXPR_ATOM &&
            head->value && strcmp(head->value, key) == 0)
            return child;
    }
    return NULL;
}

/*
 * sexpr_get_all: return heap-allocated array of all child lists
 * starting with `key`. Caller frees the array (not the nodes).
 */
sexpr_t **sexpr_get_all(const sexpr_t *list, const char *key, size_t *count_out)
{
    if (count_out) *count_out = 0;
    if (!list || list->type != SEXPR_LIST || !key) return NULL;

    size_t cap = 8, count = 0;
    sexpr_t **arr = (sexpr_t **)malloc(cap * sizeof(sexpr_t *));
    if (!arr) return NULL;

    for (size_t i = 0; i < list->num_children; i++) {
        sexpr_t *child = list->children[i];
        if (!child || child->type != SEXPR_LIST) continue;
        if (child->num_children == 0) continue;
        sexpr_t *head = child->children[0];
        if (!head || head->type != SEXPR_ATOM || !head->value) continue;
        if (strcmp(head->value, key) != 0) continue;

        if (count >= cap) {
            cap *= 2;
            sexpr_t **tmp = (sexpr_t **)realloc(arr, cap * sizeof(sexpr_t *));
            if (!tmp) { free(arr); return NULL; }
            arr = tmp;
        }
        arr[count++] = child;
    }

    if (count_out) *count_out = count;
    if (count == 0) { free(arr); return NULL; }
    return arr;
}

/*
 * sexpr_atom_value: find child list `(key VALUE)`, return VALUE's value field.
 * Returns pointer directly into tree — do NOT free.
 */
const char *sexpr_atom_value(const sexpr_t *list, const char *key)
{
    sexpr_t *child = sexpr_get(list, key);
    if (!child || child->num_children < 2) return NULL;
    sexpr_t *val = child->children[1];
    if (!val) return NULL;
    return val->value;
}
