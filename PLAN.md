# kicli — Implementation Plan

## What is kicli?

An agent-friendly CLI layer for KiCad 10. It does two things:

1. **Wraps `kicad-cli`** — makes KiCad's own CLI usable from scripts and AI agents
2. **Adds what kicad-cli can't do** — create projects, fetch components, check stock

Designed for pipe-friendly, grep-able output:
```bash
kicli sch board.kicad_sch list | grep "^U"       # all ICs
kicli sch board.kicad_sch list | grep TPS        # find TPS parts
kicli stock C2040 C14663                          # check stock
```

## Distribution goal

User runs one of:
```bash
# macOS
brew install kicli

# macOS / Linux (manual)
curl -fsSL https://github.com/you/kicli/releases/latest/download/install.sh | sh

# Windows (PowerShell)
irm https://github.com/you/kicli/releases/latest/download/install.ps1 | iex

# From source
git clone https://github.com/you/kicli && cd kicli
cmake --preset release && cmake --install build/release
```

## Tech stack

- **Language:** C11
- **Build:** CMake 3.19+, Ninja
- **HTTP:** libcurl (system)
- **JSON:** cJSON (vendored)
- **ZIP:** kuba--/zip (vendored)
- **Config:** tomlc99 (vendored)
- **Args:** optparse (vendored)
- **Platforms:** macOS (arm64 + x86_64), Windows (x64), Linux (x64)

---

## Milestones

### ✅ M0 — Project scaffold
- [x] CMake build system
- [x] Vendor libraries (cJSON, zip, tomlc99, optparse, tinycthread)
- [x] src/error.c — error handling
- [x] src/config.c — .kicli.toml loading

---

### 🔲 M1 — kicad-cli discovery (current focus)

**Goal:** `kicli` can find `kicad-cli` on any platform and run it.

Files:
```
include/kicli/kicad_cli.h
src/kicad/kicad_cli.c
```

Features:
- [ ] Find kicad-cli: check `$KICAD_CLI_PATH` env → platform default paths → `$PATH`
- [ ] Platform default paths:
  - macOS: `/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli`
  - Windows: `C:\Program Files\KiCad\bin\kicad-cli.exe`
  - Linux: `/usr/bin/kicad-cli`, `/usr/local/bin/kicad-cli`
- [ ] `kicli kicad-path` — print where kicad-cli was found (or error)
- [ ] `kicli kicad-version` — print KiCad version

---

### 🔲 M2 — Project creation

**Goal:** `kicli new myboard` creates a complete, openable KiCad 10 project.

Files:
```
include/kicli/project.h
src/project/project.c
```

Features:
- [ ] `kicli new <name> [path]`
- Creates:
  ```
  <name>/
    <name>.kicad_pro      ← KiCad 10 project JSON
    <name>.kicad_sch      ← blank schematic
    sym-lib-table         ← wired to libs/symbols/ via ${KIPRJMOD}
    fp-lib-table          ← wired to libs/footprints/ via ${KIPRJMOD}
    libs/symbols/
    libs/footprints/
    libs/3dmodels/
    .kicli.toml           ← project config
  ```

---

### 🔲 M3 — Schematic read (own parser)

**Goal:** `kicli sch FILE list` — grep-able component list.

Files:
```
src/sch/sexpr.c     ← ✅ done (287 lines)
src/sch/model.c     ← parse sexpr into data model
src/sch/sch.c       ← CLI dispatcher
```

Commands (file is always second argument):
```bash
kicli sch <file> list                    # one component per line: REF VALUE LIB_ID
kicli sch <file> info <REF>              # detailed component info
kicli sch <file> nets                    # list all nets and connected pins
kicli sch <file> tree                    # hierarchy tree
kicli sch <file> stats                   # component count summary
```

Output format (grep-friendly):
```
R1  10k        Device:R           Resistor_SMD:R_0402
C1  100nF      Device:C           Capacitor_SMD:C_0402
U1  STM32F405  MCU_ST:STM32F405   Package_LQFP:LQFP-64
```

---

### 🔲 M4 — kicad-cli passthrough

**Goal:** `kicli sch FILE export pdf` → delegates to `kicad-cli`.

```bash
kicli sch <file> export pdf [--output out.pdf]
kicli sch <file> export svg [--output out.svg]
kicli sch <file> export netlist [--output net.xml]
kicli sch <file> export bom [--output bom.csv]
kicli sch <file> erc [--output report.txt]
kicli sch <file> upgrade
```

---

### 🔲 M5 — Schematic write

**Goal:** Agents can modify schematics from the CLI.

Files: `src/sch/writer.c`, `src/sch/ops.c`

```bash
kicli sch <file> set <REF> <FIELD> <VALUE>   # e.g. set R1 value 4.7k
kicli sch <file> add <LIB:SYM> --ref R5 --value 10k
kicli sch <file> remove <REF>
kicli sch <file> move <REF> <X> <Y>
kicli sch <file> rename <OLD> <NEW>
kicli sch <file> connect <PIN_A> <PIN_B>
```

---

### 🔲 M6 — Component fetch

**Goal:** `kicli fetch C2040` downloads and installs a component into the project.

Files: `src/fetch/` (partially implemented)

```bash
kicli fetch <LCSC_ID>                    # fetch by LCSC part number
kicli fetch search "USB Type-C"          # search components
kicli fetch info C2040                   # show component details
kicli fetch list                         # list fetched components
kicli fetch remove C2040                 # remove component
```

---

### 🔲 M7 — Stock check

**Goal:** `kicli stock C2040` shows real-time availability.

```bash
kicli stock <PART> [PART...]             # check stock
kicli stock bom <bom.csv>               # check all BOM parts
kicli stock compare <PART>              # compare across suppliers
```

---

### 🔲 M8 — Distribution

**Goal:** Single binary, no runtime deps, installable on any machine.

- [ ] GitHub Actions: build matrix (macOS arm64, macOS x86_64, Windows x64, Linux x64)
- [ ] Static linking (no .dll/.dylib dependencies)
- [ ] `install.sh` for macOS/Linux
- [ ] `install.ps1` for Windows
- [ ] Homebrew formula
- [ ] GitHub Releases with pre-built binaries

---

## Command reference (final)

```
kicli new <name>                          Create a new KiCad 10 project
kicli kicad-path                          Show path to kicad-cli
kicli kicad-version                       Show KiCad version

kicli sch <file> list                     List all components
kicli sch <file> info <ref>               Component details
kicli sch <file> nets                     List all nets
kicli sch <file> tree                     Schematic hierarchy
kicli sch <file> stats                    Count summary
kicli sch <file> export <format>          Export (pdf/svg/netlist/bom)
kicli sch <file> erc                      Electrical rules check
kicli sch <file> upgrade                  Upgrade to latest format
kicli sch <file> set <ref> <field> <val>  Edit a field
kicli sch <file> add <lib:sym>            Add a component
kicli sch <file> remove <ref>             Remove a component
kicli sch <file> connect <a> <b>          Wire two pins
kicli sch <file> diff <file2>             Diff two schematics

kicli fetch <id>                          Fetch component from LCSC
kicli fetch search <query>               Search components
kicli fetch list                          List fetched components

kicli stock <part...>                     Check stock & price
kicli stock bom <file>                    Check BOM stock
kicli stock compare <part>               Compare suppliers
```
