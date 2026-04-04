# CLAUDE.md

Pipe-friendly CLI for KiCad 10. C11, CMake, libcurl.

## Build

```bash
cmake --preset debug && cmake --build build/debug
cmake --preset release && cmake --build build/release
```

Binary: `build/{debug,release}/bin/kicli`

## Structure

- `src/main.c` — top-level command dispatch
- `src/sch/sch.c` — sch subcommand router + list/info
- `src/sch/dump.c` — 3-pass pin+net view: lib_symbols → kicad-cli netlist → union-find wire tracing
- `src/sch/ops/set.c` — sch set / set-all (s-expression in-place edit)
- `src/sch/parser/` — KiCad s-expression parser (sexpr.c, model.c)
- `src/jlcpcb/jlcpcb.c` — jlcpcb part/search/bom (JLCPCB API via libcurl)
- `src/kicad/` — kicad-cli wrapper (export, erc, netlist)
- `src/project/project.c` — kicli new (project scaffolding)
- `src/fetch/fetch.c` — stub, functionality covered by jlcpcb part
- `src/stock/stock.c` — stub, functionality covered by jlcpcb part (Count field)
- `include/kicli/` — public headers

## Commands (current state)

### Implemented
- `sch list [--all]` — tab-separated: REF VALUE LIB FOOTPRINT PartNo
- `sch info <REF>` — all properties + position + PartNo status (shows "(unset)")
- `sch view [-o file]` — full pin+net connectivity table (grep-friendly)
- `sch set <REF> <FIELD> <VALUE>` — edit one component property
- `sch set-all <VALUE> <FIELD> <NEW> [--footprint glob] [--dry-run]` — bulk edit with footprint filter
- `sch export pdf|svg|netlist|bom [-o]` — kicad-cli passthrough
- `sch erc [-o]` — kicad-cli passthrough
- `jlcpcb part <LCSC_ID>` �� brand, model, package, stock, price, datasheet URL
- `jlcpcb search <query>` — CSV output with filters: -n, --basic, --extended, --in-stock, --package
- `jlcpcb bom <file|dir> [-o csv]` — JLCPCB-ready BOM, merges all .kicad_sch in directory, shows missing count
- `new <name>` — create KiCad 10 project
- `config kicad-path [path]` / `kicad-path` / `kicad-version`
- `skills` — print SKILLS.md agent guide (embedded in binary)

### Agent guide
- `SKILLS.md` — comprehensive agent skill document with piping recipes and workflows

## Rules

- Language: C11, cross-platform (macOS/Linux/Windows)
- Build: CMake 3.19+, Ninja, presets: `debug` / `release`
- Dependency: libcurl
- CI: `.github/workflows/release.yml`
- All agent-facing output: no ANSI color codes, machine-parseable
- Exit codes: 0=OK, 1=error, 2=NOT_FOUND, 3=IO, 4=PARSE

## Strategy

kicli is a data layer for AI agents. The agent reads schematics, looks up
datasheets, and makes design decisions. kicli never automates component
selection — that requires circuit context (voltage, temp, physical constraints).

Agent schematic review flow:
1. `view` → pin/net connectivity
2. `jlcpcb part` → datasheet URL
3. Agent reads PDF → design requirements
4. Agent compares view vs datasheet → critiques schematic
