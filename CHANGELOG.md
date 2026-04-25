# Changelog

All notable changes to **kicli** are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
adheres to [Semantic Versioning](https://semver.org/).

## [0.11.3] — 2026-04-25

### Changed
- **CCM7-style placement for power ports and sheet-pin labels**:
  matching how a human draws KiCad schematics, kicli now offsets
  power ports and sheet-pin labels by 2.54 mm in the OUTWARD direction
  of the connecting pin and emits a wire stub between them. Eliminates
  text overlap with component bodies / pin numbers / sheet pin names.
- **Outward-reading label rotation**: plain / hierarchical / global
  labels now rotate so their text reads AWAY from the connecting
  symbol (was reading INTO the symbol body). KiCad label angle =
  pin's outward direction.
- New helpers in `sch_ops.h`: `mk_wire(x1,y1,x2,y2)` and
  `offset_outward(into_angle, step, dx, dy)`.

## [0.11.2] — 2026-04-25

### Changed
- **Power-port orientation**: `mk_power_port` now reads the power lib's
  natural pin angle and rotates the placement so the symbol body always
  lands AWAY from the connecting wire. GND triangle hangs below a
  downward-facing pin, +5V/+3V3 arrow points up from an upward-facing
  pin, side-facing pins get 90°/270° placements — matches the visual
  convention real KiCad designs use (CCM7 reference).
- **Wider auto-grid**: `auto_slot` switched from 10 cols × 25 mm to 6
  cols × 38.1 mm spacing (and 30.48 mm row height). Both dimensions are
  1.27 mm grid multiples so pins still snap, but labels at adjacent
  symbols no longer collide.

## [0.11.1] — 2026-04-25

### Added
- `sch net <name> <SheetName>:<PinName>` — `cmd_sch_net` now accepts a
  hierarchical sheet pin in place of `<ref>:<pin>`. The label is dropped
  at the sheet pin's exact world coordinate so the parent net splices to
  the child's hierarchical_label by name. Closes the connectivity gap
  ERC was reporting between parent labels and sheet pins.

## [0.11.0] — 2026-04-25

### Added
- `sch sheet <name> <child-file> [--at X,Y] [--size W,H] [--pins NAME[:type],…]`
  — adds a hierarchical sheet to the parent, creating the child `.kicad_sch`
  with a blank template if it doesn't exist. Sheet pins auto-distribute on
  the left edge.
- `sch net … --as hier|hierarchical` — emits `hierarchical_label` so a
  child sheet's net splices into a parent sheet pin by name.
- `sch net … --as global` — emits `global_label` for cross-sheet rails
  outside the standard power-rail heuristic.

### Changed
- **DRY refactor** of `src/sch/ops/`: 1700-line `draw.c` split into three
  focused files behind `include/kicli/sch_ops.h`:
  - `sch_common.c` — sexpr builders, schematic walkers, load/save
  - `draw.c`       — `cmd_sch_place / net / nc / sheet`
  - `check.c`      — `cmd_sch_check`
  Removed the placeholder helpers (`append_atoms`, `mklist_atoms`) and the
  `unused_hush_()` workaround that kept them linked.

## [0.10.2] — 2026-04-25

### Added
- `sch check [--no-erc]` — single-shot layout-readiness validator. Reports
  duplicate references (incl. duplicate `#PWRNNNN`), missing footprints,
  duplicate UUIDs, and ERC violations. Exits 0 only when the schematic
  is layout-ready.
- `sch place <lib_id> ? <value>` — `?` auto-annotates the next free
  `<PREFIX><N>` (prefix derived from `lib_id`: `Device:R` → `R`).

### Fixed
- `mk_power_port` no longer collides with existing `#PWRNNNN` references.
  The auto-counter is now seeded from the schematic on every
  `cmd_sch_net` invocation, eliminating the duplicate-PWR annotation
  errors KiCad reported when adding multiple power-rail ports.

## [0.10.1] — 2026-04-23

### Fixed
- `sch place` now inherits the library symbol's default `Footprint`,
  `Datasheet`, and `Description` properties (with `(extends "Base")`
  alias resolution), matching how KiCad's own palette places symbols.

## [0.10.0] — 2026-04-23

### Added
- Schematic drawing primitives — label-first authoring, no 2-D wire routing:
  - `sch place <lib_id> <ref> [<value>]` — drops a symbol from the bundled
    KiCad catalog. Auto-grids when `--at` is omitted; `--footprint`,
    `--mirror`, `--angle` overrides supported.
  - `sch net <name> <ref:pin> [<ref:pin>…]` — attaches a label (or a
    `power:` port for known rails like `GND`, `+3V3`, `+5V`) at each
    pin's world position.
  - `sch nc <ref:pin> [<ref:pin>…]` — places a `(no_connect)` marker.

## [0.9.0] — 2026-04-22

### Added
- Symbol + footprint catalog discovery: `sym list/search/info`,
  `fp list/search/info`. Walks bundled KiCad libraries plus project-local
  `libs/symbols` and `libs/footprints/*.pretty`.
- `sch set-all --only-empty` — bulk-edit only fields that are currently
  unset (won't overwrite existing values).
- 34-assertion `scripts/compat-test.sh` 3rd-party-compatibility gate.

### Removed
- `sch export bom` — superseded by `jlcpcb bom` (JLCPCB-ready format).

## [0.8.0] — 2026-04-21

### Added
- `jlcpcb check <file|dir>` — side-by-side comparison of every component's
  schematic value/footprint against the JLCPCB API record for its `LCSC`
  PartNo. 9-column tab output with substring-match heuristic.

## [0.7.2] — 2026-04-20

### Fixed
- Atomic schematic writes: stage to `<path>.kicli.<pid>.tmp` then `rename`
  over the target. Original file stays intact on crash / power loss.
- `kicli new` refuses to overwrite an existing project unless `--force`.
- Honest exit codes (0=OK, 1=error, 2=NOT_FOUND, 3=IO, 4=PARSE).
- Removed stub functions that returned `KICLI_ERR_NOT_IMPLEMENTED` from
  the public header.

## [0.7.1] — 2026-04-20

### Fixed
- `view` output disambiguates the bare `~` (KiCad "no value") marker
  from a literal tilde in user-supplied net names.

## [0.7.0] — 2026-04-19

### Added
- `info <REF> --pins` — full pin table (`NUM NAME TYPE NET`).
- `view --net <NET>` — flat per-pin table for every pin on a given net.
- `erc --format json` — machine-parseable ERC output.

## [0.6.0] — 2026-04-18

### Added
- Multi-sheet schematic read across hierarchical projects (dir mode for
  `list`, `view`, `set-all`).

## [0.5.1] — 2026-04-17

### Added
- Cross-platform `kicli/portable.h` shim layer (mkdir/p, exists, rename,
  unlink, atomic write, opendir/readdir, uuid4, strcasecmp). DRY removal
  of every per-OS `#ifdef` previously scattered across the tree.

## Earlier

Initial drops focused on the read pipeline (`sch list`, `sch info`,
`sch view`), `kicad-cli` passthrough commands (`erc`, `export`), and the
JLCPCB part / search / bom integration.
