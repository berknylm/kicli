#pragma once
#ifndef KICLI_SCH_H
#define KICLI_SCH_H

#include <stdbool.h>
#include <stddef.h>
#include "kicli/error.h"

/* ── S-expression node ───────────────────────────────────────────────────── */

typedef enum { SEXPR_ATOM, SEXPR_STR, SEXPR_LIST } sexpr_type_t;

typedef struct sexpr_s {
    sexpr_type_t type;
    char        *value;          /* ATOM or STR: string content */
    struct sexpr_s **children;   /* LIST: child nodes           */
    size_t        num_children;
} sexpr_t;

sexpr_t    *sexpr_parse(const char *input, char *err_out, size_t err_sz);
void        sexpr_free(sexpr_t *node);
sexpr_t    *sexpr_get(const sexpr_t *list, const char *key);    /* first child list starting with key */
sexpr_t   **sexpr_get_all(const sexpr_t *list, const char *key, size_t *count_out);
const char *sexpr_atom_value(const sexpr_t *list, const char *key); /* first atom of named child */

/* ── S-expression builders ───────────────────────────────────────────────── */
sexpr_t *sexpr_make_atom(const char *val);          /* bare atom   */
sexpr_t *sexpr_make_str(const char *val);           /* "quoted"    */
sexpr_t *sexpr_make_list(void);                     /* empty list  */
int      sexpr_list_append(sexpr_t *list, sexpr_t *child); /* 1=ok */

/* ── S-expression serializer ─────────────────────────────────────────────── */
int sexpr_write_file(const sexpr_t *root, const char *path); /* 0=ok */

/* ── Geometry ────────────────────────────────────────────────────────────── */

typedef struct { double x, y, angle; } kicli_pos_t;
typedef struct { double x, y; }        kicli_pt_t;

/* ── Pin types ───────────────────────────────────────────────────────────── */

typedef enum {
    PIN_INPUT, PIN_OUTPUT, PIN_BIDI, PIN_TRISTATE, PIN_PASSIVE,
    PIN_POWER_IN, PIN_POWER_OUT, PIN_OPEN_COLL, PIN_OPEN_EMIT,
    PIN_NO_CONNECT, PIN_UNSPECIFIED
} kicli_pin_type_t;

/* ── Library symbol (embedded definition) ───────────────────────────────── */

typedef struct {
    char            number[64];
    char            name[128];
    kicli_pin_type_t pin_type;
    kicli_pos_t     at;
    double          length;
    double          angle;
} kicli_pin_t;

typedef struct {
    char         name[256];
    kicli_pin_t *pins;
    size_t       num_pins;
    char         extends[256]; /* empty if not an alias */
} kicli_lib_symbol_t;

/* ── Property ────────────────────────────────────────────────────────────── */

typedef struct {
    char        key[128];
    char       *value;       /* heap-allocated */
    bool        has_at;
    kicli_pos_t at;
    bool        hidden;
} kicli_property_t;

/* ── Pin reference (placed symbol) ──────────────────────────────────────── */

typedef struct {
    char number[64];
    char net[128];           /* empty if unconnected */
} kicli_pin_ref_t;

/* ── Placed symbol ───────────────────────────────────────────────────────── */

typedef struct {
    char              lib_id[256];
    char              reference[64];
    char             *value;        /* heap */
    char             *footprint;    /* heap */
    char             *datasheet;    /* heap */
    kicli_pos_t       at;
    unsigned int      unit;
    bool              in_bom;
    bool              on_board;
    kicli_property_t *properties;
    size_t            num_properties;
    kicli_pin_ref_t  *pins;
    size_t            num_pins;
} kicli_symbol_t;

/* ── Wire / Junction / Label ─────────────────────────────────────────────── */

typedef struct { kicli_pt_t start, end; } kicli_wire_t;
typedef struct { kicli_pt_t at; double diameter; } kicli_junction_t;
typedef struct { char text[256]; kicli_pos_t at; } kicli_label_t;
typedef struct { char text[256]; char shape[64]; kicli_pos_t at; } kicli_global_label_t;

/* ── Hierarchical sheet ─────────────────────────────────────────────────── */

typedef struct {
    char name[64];      /* pin label (e.g. "CAN_TX") */
    char direction[16]; /* input / output / bidirectional / passive */
} kicli_sheet_pin_t;

typedef struct {
    char              sheetname[128];
    char              sheetfile[256];
    kicli_sheet_pin_t *pins;
    size_t             num_pins;
} kicli_sheet_t;

/* ── Hierarchical label ─────────────────────────────────────────────────── */

typedef struct {
    char        text[256];
    char        shape[64];   /* input / output / bidirectional / passive */
    kicli_pos_t at;
} kicli_hier_label_t;

/* ── Title block ─────────────────────────────────────────────────────────── */

typedef struct {
    char title[256];
    char revision[64];
    char company[256];
    char date[64];
} kicli_title_block_t;

/* ── Schematic ───────────────────────────────────────────────────────────── */

typedef struct {
    unsigned int          version;
    char                  generator[64];
    kicli_lib_symbol_t   *lib_symbols;
    size_t                num_lib_symbols;
    kicli_symbol_t       *symbols;       /* non-power placed symbols */
    size_t                num_symbols;
    kicli_symbol_t       *power_symbols;
    size_t                num_power_symbols;
    kicli_wire_t         *wires;
    size_t                num_wires;
    kicli_junction_t     *junctions;
    size_t                num_junctions;
    kicli_label_t        *labels;
    size_t                num_labels;
    kicli_global_label_t *global_labels;
    size_t                num_global_labels;
    kicli_hier_label_t   *hier_labels;
    size_t                num_hier_labels;
    kicli_sheet_t        *sheets;
    size_t                num_sheets;
    bool                  has_title_block;
    kicli_title_block_t   title_block;
} kicli_schematic_t;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

kicli_err_t kicli_sch_read(const char *path, kicli_schematic_t **out);
void        kicli_sch_free(kicli_schematic_t *sch);

/* ── Write back ──────────────────────────────────────────────────────────── */

kicli_err_t kicli_sch_write(const kicli_schematic_t *sch, const char *path, bool backup);

/* ── Lookup helpers ──────────────────────────────────────────────────────── */

kicli_symbol_t    *kicli_sch_symbol_by_ref(kicli_schematic_t *sch, const char *ref);
kicli_lib_symbol_t *kicli_sch_find_lib_symbol(kicli_schematic_t *sch, const char *lib_id);
bool kicli_sch_pin_position(const kicli_schematic_t *sch, const char *ref,
                             const char *pin_number, kicli_pt_t *out);

/* ── Dump ────────────────────────────────────────────────────────────────── */

kicli_err_t kicli_sch_dump_json(const kicli_schematic_t *sch, char **out_json); /* caller frees */

/* ── Operations ──────────────────────────────────────────────────────────── */

kicli_err_t kicli_sch_add_symbol(kicli_schematic_t *sch, const char *lib_id,
                                  const char *ref, const char *value, kicli_pos_t at);
kicli_err_t kicli_sch_remove_symbol(kicli_schematic_t *sch, const char *ref);
kicli_err_t kicli_sch_move_symbol(kicli_schematic_t *sch, const char *ref, double x, double y);
kicli_err_t kicli_sch_connect(kicli_schematic_t *sch, const char *pin_a, const char *pin_b);
kicli_err_t kicli_sch_disconnect(kicli_schematic_t *sch, const char *pin_a, const char *pin_b);
kicli_err_t kicli_sch_rename(kicli_schematic_t *sch, const char *old_ref, const char *new_ref);
kicli_err_t kicli_sch_set_field(kicli_schematic_t *sch, const char *ref,
                                  const char *field, const char *value);

/* ── Diff ────────────────────────────────────────────────────────────────── */

typedef struct {
    char  reference[64];
    char  field[128];
    char *old_value; /* heap */
    char *new_value; /* heap */
} kicli_sch_field_change_t;

typedef struct {
    char                   **added;         /* references added in b   */
    size_t                   num_added;
    char                   **removed;       /* references removed in b  */
    size_t                   num_removed;
    kicli_sch_field_change_t *changes;      /* field-level changes      */
    size_t                   num_changes;
    size_t                   wires_added;
    size_t                   wires_removed;
} kicli_sch_diff_t;

kicli_sch_diff_t *kicli_sch_diff(const kicli_schematic_t *a, const kicli_schematic_t *b);
void              kicli_sch_diff_print(const kicli_sch_diff_t *d);
void              kicli_sch_diff_free(kicli_sch_diff_t *d);

/* ── Validate (ERC) ──────────────────────────────────────────────────────── */

typedef struct { char message[512]; char reference[64]; } kicli_erc_issue_t;

typedef struct {
    kicli_erc_issue_t *errors;
    size_t             num_errors;
    kicli_erc_issue_t *warnings;
    size_t             num_warnings;
} kicli_validation_t;

kicli_validation_t *kicli_sch_validate(const kicli_schematic_t *sch);
void                kicli_validation_print(const kicli_validation_t *v);
void                kicli_validation_free(kicli_validation_t *v);

/* ── Export ──────────────────────────────────────────────────────────────── */

kicli_err_t kicli_sch_export(const char *sch_path, const char *format, const char *output_path);

/* ── Print helpers ───────────────────────────────────────────────────────── */

void kicli_sch_print_tree(const kicli_schematic_t *sch);

#endif /* KICLI_SCH_H */
