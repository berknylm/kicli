#pragma once
#ifndef KICLI_STOCK_H
#define KICLI_STOCK_H

#include <stdbool.h>
#include <stddef.h>
#include "kicli/error.h"
#include "kicli/config.h"

/* Price break at a given quantity */
typedef struct {
    unsigned int quantity;
    double       unit_price;
    char         currency[8];
} kicli_price_break_t;

/* Stock result for one part from one supplier */
typedef struct {
    char               supplier[64];
    char               part_number[128];
    char               mfr_part[128];
    long               quantity_available;
    char               lead_time[64];     /* e.g. "In stock", "3 days" */
    kicli_price_break_t *price_breaks;
    size_t              num_price_breaks;
} kicli_stock_result_t;

void kicli_stock_result_free(kicli_stock_result_t *r);

/* Aggregated report for one part across all suppliers */
typedef struct {
    char                part_number[128];
    kicli_stock_result_t *results;  /* one per supplier */
    size_t               num_results;
} kicli_stock_report_t;

void kicli_stock_report_free(kicli_stock_report_t *r);

/* ── Supplier interface ──────────────────────────────────────────────────── */

typedef kicli_err_t (*kicli_supplier_check_fn)(
    const char *part_number, const kicli_config_t *cfg, kicli_stock_result_t **out);

typedef struct {
    const char               *name;
    kicli_supplier_check_fn   check;
} kicli_supplier_t;

/* ── BOM ─────────────────────────────────────────────────────────────────── */

/* One row from a BOM CSV */
typedef struct {
    char reference[64];
    char value[256];
    char part_number[128];  /* LCSC / Digikey / Mouser part number */
    char footprint[256];
    int  quantity;
} kicli_bom_row_t;

kicli_err_t kicli_bom_parse(const char *path, kicli_bom_row_t **rows_out, size_t *count_out);
void        kicli_bom_free(kicli_bom_row_t *rows, size_t count);

/* ── High-level commands ─────────────────────────────────────────────────── */

/* Check stock for an array of part numbers across all configured suppliers */
kicli_err_t kicli_stock_check(const char **part_numbers, size_t count,
                               const kicli_config_t *cfg,
                               kicli_stock_report_t **reports_out, size_t *reports_count_out);

/* Print a formatted table of stock results */
void kicli_stock_print_table(const kicli_stock_report_t *reports, size_t count);

/* Check all parts in a BOM */
kicli_err_t kicli_stock_check_bom(const char *bom_path, const kicli_config_t *cfg);

/* Compare one part across all suppliers and print a comparison table */
kicli_err_t kicli_stock_compare(const char *part_number, const kicli_config_t *cfg);

/* Watch parts and alert when quantity drops below threshold */
kicli_err_t kicli_stock_watch(const char **part_numbers, size_t count,
                               long below_threshold, const char *notify_channel,
                               const kicli_config_t *cfg);

/* Export BOM with stock/pricing data to CSV */
kicli_err_t kicli_stock_export(const char *bom_path, const char *format,
                                const int *quantities, int num_quantities,
                                const kicli_config_t *cfg);

#endif /* KICLI_STOCK_H */
