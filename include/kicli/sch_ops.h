/*
 * sch_ops.h — internal header for src/sch/ops files
 *
 * Shared sexpr builders, schematic-tree walkers, load/save scaffolding,
 * and the auto-#PWR counter. Only included by files under src/sch/ops/.
 *
 * Naming: bare verbs (mk_at, find_placed_by_ref, …) — these names live
 * in their own translation-unit family; not exposed in the public API.
 *
 * Why this header exists: keep DRY. Three command files (draw, check,
 * future ones) navigate the same KiCad 10 s-expression tree. Without
 * a shared module the helpers duplicate, drift, or get bolted on with
 * static-suppression hacks.
 */
#ifndef KICLI_SCH_OPS_H
#define KICLI_SCH_OPS_H

#include "kicli/sch.h"
#include <stddef.h>

/* ── tiny sexpr builders ─────────────────────────────────────────────────── */

sexpr_t *make_atom (const char *v);
sexpr_t *make_str  (const char *v);
void     fmt_num   (char *out, size_t sz, double v);
sexpr_t *mk_at         (double x, double y, double a);
sexpr_t *mk_named_atom (const char *tag, const char *val);
sexpr_t *mk_named_str  (const char *tag, const char *val);
sexpr_t *mk_effects_default(void);
sexpr_t *mk_uuid_node      (void);
sexpr_t *mk_property(const char *key, const char *value,
                     double x, double y, double angle, int hide);

/* ── label / port / no_connect builders ──────────────────────────────────── */

sexpr_t *mk_label       (const char *text, double x, double y, double angle);
sexpr_t *mk_global_label(const char *text, const char *shape,
                         double x, double y, double angle);
sexpr_t *mk_hier_label  (const char *text, const char *shape,
                         double x, double y, double angle);
sexpr_t *mk_no_connect  (double x, double y);

sexpr_t *mk_placed_symbol(const char *lib_id, const char *ref,
                          const char *value,
                          double x, double y, double angle,
                          const sexpr_t *lib_def, const sexpr_t *root,
                          const char *root_uuid, const char *project_name);

sexpr_t *mk_power_port(const char *rail, double x, double y, double angle,
                       const char *root_uuid, const char *project_name);

void seed_pwr_counter(const sexpr_t *root);

/* ── geometry / detection ────────────────────────────────────────────────── */

double snap_grid(double v, double step);
int    detect_power(const char *net_name, char *canon_out, size_t sz);

/* ── tree walkers ────────────────────────────────────────────────────────── */

const char *get_root_uuid     (const sexpr_t *root);
void        project_name_for  (const char *sch_path, char *out, size_t sz);
sexpr_t    *get_or_create_lib_symbols(sexpr_t *root);
int         lib_symbols_has   (const sexpr_t *ls, const char *lib_id);
int         ensure_lib_symbol_in_sheet(sexpr_t *root, const char *lib_id);
sexpr_t    *find_lib_symbol_node(const sexpr_t *root, const char *lib_id);
size_t      count_placed_symbols(const sexpr_t *root);
void        auto_slot         (size_t idx, double *x, double *y);
sexpr_t    *find_placed_by_ref(const sexpr_t *root, const char *ref);
const char *placed_lib_id     (const sexpr_t *sym);
int         placed_at         (const sexpr_t *sym, double *x, double *y, double *a);
int         placed_mirror_x   (const sexpr_t *sym);
int         world_pin_pos     (sexpr_t *root, const char *ref, const char *pin_num,
                               double *wx, double *wy, double *wangle);
/* Look up a (sheet) by Sheetname property and return the (pin "name" …)
 * coordinate. Used so `kicli sch parent net VIN Buck5V:VIN` works the
 * same way as `R1:1` for placed symbols. Returns 0 on success. */
int         sheet_pin_pos     (const sexpr_t *root, const char *sheet_name,
                               const char *pin_name,
                               double *wx, double *wy, double *wangle);
const char *lib_default_property(const sexpr_t *lib_def, const sexpr_t *root,
                                 const char *lib_id, const char *key);

/* placed-symbol property/uuid extractors used by check + place */
const char *placed_property(const sexpr_t *sym, const char *field);
const char *placed_uuid    (const sexpr_t *sym);

/* ── load / save scaffolding ─────────────────────────────────────────────── */

sexpr_t *load_sch(const char *path);
int      save_sch(sexpr_t *root, const char *path);

#endif /* KICLI_SCH_OPS_H */
