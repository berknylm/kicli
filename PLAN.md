# kicli Implementation Plan

## Overview

kicli is a Rust CLI toolkit for KiCad with three core subcommand groups:
- `kicli fetch` — component library manager (download symbols, footprints, 3D models)
- `kicli stock` — real-time supplier stock/pricing checker
- `kicli sch` — schematic parser, manipulator, and exporter

---

## Phase 0: Project Scaffold

**Goal:** Compiling binary with CLI skeleton and config loading.

- [ ] Initialize `Cargo.toml` with dependencies: `clap`, `serde`/`serde_json`/`toml`, `anyhow`/`thiserror`, `tokio`, `reqwest`
- [ ] `src/main.rs` — top-level `clap` command with `fetch`, `stock`, `sch` subcommands
- [ ] `src/error.rs` — shared error types via `thiserror`
- [ ] `src/config.rs` — load `.kicli.toml` (project) then `~/.config/kicli/config.toml` (global), merge and expose typed config struct
- [ ] `src/lib.rs` — re-export public API
- [ ] Verify `cargo build` succeeds and `kicli --help` shows three subcommand groups

---

## Phase 1: `kicli fetch`

**Goal:** Import LCSC components (symbols + footprints + 3D models) into local KiCad libraries.

### 1.1 Source abstraction
- [ ] `src/fetch/sources/mod.rs` — define `ComponentSource` trait: `search`, `info`, `download`
- [ ] `src/fetch/sources/lcsc.rs` — implement LCSC via EasyEDA/LCSC API; parse response into `ComponentAssets` (symbol KiCad `.kicad_sym`, footprint `.kicad_mod`, 3D `.step`/`.wrl`)

### 1.2 Importer
- [ ] `src/fetch/importer.rs` — write downloaded assets into `lib_dir/{symbol_lib}.kicad_sym` and `{footprint_lib}.pretty/`, merge without duplicating existing entries

### 1.3 Registry
- [ ] `src/fetch/registry.rs` — persist fetched component metadata to `.kicli/registry.json` (id, source, version, timestamp)

### 1.4 Subcommand wiring
- [ ] `src/fetch/mod.rs` — route `fetch <ID>`, `fetch search`, `fetch info`, `fetch list`, `fetch sync`, `fetch remove`, `fetch sources`
- [ ] Support `--source`, `--lib`, `--from <csv>` flags
- [ ] Implement `fetch sync --dry-run`

### 1.5 Additional sources (after LCSC works)
- [ ] `src/fetch/sources/digikey.rs`
- [ ] `src/fetch/sources/snapeda.rs`

---

## Phase 2: `kicli stock`

**Goal:** Query real-time stock and pricing from multiple suppliers.

### 2.1 Supplier abstraction
- [ ] `src/stock/suppliers/mod.rs` — define `Supplier` trait: `check_stock(part_numbers) -> Vec<StockResult>`
- [ ] `src/stock/suppliers/lcsc.rs` — LCSC stock API
- [ ] `src/stock/suppliers/digikey.rs` — Digikey product search API (OAuth2)
- [ ] `src/stock/suppliers/mouser.rs` — Mouser search API (API key)

### 2.2 Core logic
- [ ] `src/stock/checker.rs` — fan-out requests across configured suppliers concurrently (`tokio::join!`), aggregate into `StockReport`
- [ ] `src/stock/bom.rs` — parse CSV BOM, extract part numbers column, yield list for batch checking

### 2.3 Watch & alerts
- [ ] `src/stock/watcher.rs` — poll on interval, diff against last snapshot, fire alert when threshold crossed
- [ ] Alert channels: terminal (default), webhook (Slack-compatible)

### 2.4 Output formatting
- [ ] Table output for `stock check` / `stock compare` (use `comfy-table` or `tabled`)
- [ ] `stock export` — write enriched CSV or XLSX with pricing columns

### 2.5 Subcommand wiring
- [ ] `src/stock/mod.rs` — route `check`, `bom`, `watch`, `compare`, `export`
- [ ] Cache results to `~/.cache/kicli/stock/` with TTL from config (`cache_ttl`)

---

## Phase 3: `kicli sch`

**Goal:** Full read/write access to `.kicad_sch` files from the CLI.

### 3.1 Parser
- [ ] `src/sch/parser.rs` — parse KiCad s-expression (sexpr) format into `Schematic` model
  - Use `kicad_sexpr` crate if available, otherwise hand-roll a minimal s-expr tokenizer

### 3.2 Data model
- [ ] `src/sch/model.rs` — types: `Schematic`, `Symbol`, `Wire`, `Junction`, `Label`, `Pin`, `Property`

### 3.3 Writer
- [ ] `src/sch/writer.rs` — serialize `Schematic` back to valid `.kicad_sch` s-expression; create `.bak` when `backup_on_write = true`

### 3.4 Operations
- [ ] `src/sch/ops.rs`
  - `add_symbol` — place symbol instance at coordinates, assign reference/value
  - `remove_symbol` — delete by reference
  - `move_symbol` — update `at` field
  - `connect` — add wire segment between pin positions
  - `disconnect` — remove wire
  - `rename` — update reference across symbol and labels
  - `set_field` — update arbitrary property value

### 3.5 Diff
- [ ] `src/sch/diff.rs` — compare two `Schematic` instances, produce added/modified/removed lists

### 3.6 Validate
- [ ] `src/sch/validate.rs` — basic ERC: detect unconnected pins, duplicate references, missing power symbols

### 3.7 Export
- [ ] `src/sch/export.rs` — invoke KiCad CLI (`kicad-cli`) for PDF/SVG/PNG/netlist export; fall back to error if `kicad-cli` not on PATH

### 3.8 Subcommand wiring
- [ ] `src/sch/mod.rs` — route all subcommands; `dump` serializes via `serde_json`/`serde_yaml`; `tree` renders ASCII hierarchy; `read` shows abbreviated tree

---

## Phase 4: Testing

- [ ] `tests/fixtures/sample.kicad_sch` — minimal valid schematic fixture
- [ ] `tests/fixtures/sample_bom.csv` — sample BOM with LCSC part numbers
- [ ] `tests/sch_tests.rs` — round-trip parse → write → parse, op tests
- [ ] `tests/stock_tests.rs` — mock HTTP responses, verify aggregation logic
- [ ] `tests/fetch_tests.rs` — mock API, verify file output in temp dir

---

## Phase 5: Polish

- [ ] Progress bars / spinners via `indicatif` for slow network operations
- [ ] `--json` global flag for machine-readable output on any command
- [ ] Shell completion generation (`clap_complete`)
- [ ] Man page generation (`clap_mangen`)
- [ ] Interactive TUI mode (`ratatui`) — stretch goal

---

## Dependency Plan

```toml
[dependencies]
clap          = { version = "4", features = ["derive"] }
serde         = { version = "1", features = ["derive"] }
serde_json    = "1"
serde_yaml    = "0.9"
toml          = "0.8"
anyhow        = "1"
thiserror     = "1"
tokio         = { version = "1", features = ["full"] }
reqwest       = { version = "0.12", features = ["json"] }
comfy-table   = "7"
indicatif     = "0.17"
dirs          = "5"
chrono        = "0.4"
```

---

## Milestone Summary

| Phase | Deliverable                        | Priority  |
|-------|------------------------------------|-----------|
| 0     | Compiling scaffold + config        | Critical  |
| 1     | `kicli fetch` with LCSC            | High      |
| 2     | `kicli stock` check + BOM          | High      |
| 3     | `kicli sch` read/dump/validate     | High      |
| 3+    | sch add/remove/connect/diff        | Medium    |
| 1+    | Digikey, SnapEDA sources           | Medium    |
| 2+    | Stock watch + export               | Medium    |
| 4     | Tests                              | High      |
| 5     | TUI, completions, man pages        | Low       |
