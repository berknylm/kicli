## kicli

Pipe-friendly CLI for KiCad 10. Built for AI agents and shell automation.

All output is plain text — tab-separated, grep-friendly, pipe-ready.

```bash
kicli sch board.kicad_sch list                # list all components
kicli sch board.kicad_sch view                # full pin+net connectivity
kicli jlcpcb part C89418                      # datasheet URL, stock, price
kicli jlcpcb search "100nF 0402" --basic      # search JLCPCB catalog
kicli sch project/ set-all "100nF" LCSC C1525 # bulk-assign part numbers
kicli jlcpcb bom board.kicad_sch -o bom.csv   # JLCPCB-ready BOM
```

---

## Strategy

kicli is a **data layer** for AI agents working with hardware designs.
The agent does the thinking — kicli gives it clean, structured access to
schematics and component data.

**Agent workflow — schematic review:**

```
1. kicli sch board.kicad_sch view           → pin-level connectivity
2. kicli jlcpcb part <LCSC_ID>             → datasheet URL
3. Agent reads datasheet PDF               → understands IC requirements
4. Agent writes per-IC .md design notes    → bypass caps, pull-ups, constraints
5. Agent compares view vs notes            → critiques / fixes the schematic
```

**Agent workflow — JLCPCB BOM assembly:**

```
1. kicli sch board.kicad_sch list           → what's in the schematic
2. kicli jlcpcb bom board.kicad_sch         → which parts are missing LCSC codes
3. kicli jlcpcb search "100nF 0402" --basic → find candidates
4. Agent reads datasheet, picks the right part based on circuit context
5. kicli sch proj/ set-all "100nF" LCSC C1525
6. kicli jlcpcb bom board.kicad_sch -o bom.csv
```

Component selection cannot be automated — the agent must decide based on
voltage rating, temperature coefficient, package constraints, and circuit context.

---

## Install

**Build from source** (requires CMake 3.19+, C11 compiler, libcurl, Ninja):

```bash
git clone https://github.com/berknylm/kicli.git && cd kicli
cmake --preset release && cmake --build build/release
sudo cmake --install build/release
```

Pre-built binaries: [Releases](https://github.com/berknylm/kicli/releases/latest)

> Requires **KiCad 10** for export/erc commands — [kicad.org/download](https://www.kicad.org/download/)

---

## Commands

### Project

```bash
kicli new myboard                           # create KiCad 10 project with local libs
kicli config kicad-path                     # show kicad-cli path
kicli config kicad-path /usr/bin/kicad-cli  # set it manually
kicli kicad-version                         # → 10.0.0
```

### Schematic — Read

```bash
kicli sch <file> list [--all]
# REF       VALUE      LIB                FOOTPRINT
# R1        10k        Device:R           Resistor_SMD:R_0402_1005Metric
# C1        100nF      Device:C           Capacitor_SMD:C_0402_1005Metric
# --all includes power symbols (#PWR, #FLG)

kicli sch <file> info <REF>
# R1
#   lib_id:     Device:R
#   value:      10k
#   footprint:  Resistor_SMD:R_0402_1005Metric
#   position:   (100.000, 50.000) angle=0.0
#   Properties:
#     Reference  R1
#     Value      10k
#     LCSC       C25744

kicli sch <file> view [-o out.kisch]
# Full pin+net connectivity table
# [U1: STM32H743BIT6]
# U1:1    VDDA     pwrin   → VDDA_3V3
# U1:2    VSSA     pwrin   → GND
# U1:3    PA0      inout   → ADC_IN
# U1:4    PA1      inout   → ~          ← floating (no wire)
#
# grep patterns:
#   grep '^U1:'    → all U1 pins
#   grep '→ NC'    → no-connect markers
#   grep '→ ~'     → floating pins (possible ERC error)
#   grep 'pwrin'   → power input pins
```

### Schematic — Write

```bash
kicli sch <file> set <REF> <FIELD> <VALUE>
# kicli sch board.kicad_sch set R1 LCSC C25744
# → set R1.LCSC = C25744

kicli sch <file|dir> set-all <VALUE_MATCH> <FIELD> <NEW_VALUE>
# kicli sch project/ set-all "100nF" LCSC C1525
# → sets LCSC=C1525 on every component with value "100nF"
#   across all .kicad_sch files in the directory
```

### Schematic — Export (kicad-cli passthrough)

Requires KiCad 10 installed.

```bash
kicli sch <file> export pdf [-o out.pdf]
kicli sch <file> export svg [-o out.svg]
kicli sch <file> export netlist [-o out.net]
kicli sch <file> export bom [-o out.csv]
kicli sch <file> erc [-o out.json]
```

### JLCPCB

```bash
kicli jlcpcb part <LCSC_ID>
# Brand:     STMicroelectronics
# Model:     STM32H743BIT6
# Code:      C89418
# Datasheet: https://wmsc.lcsc.com/.../STM32H743BIT6_C89418.pdf
# Count:     130
# Price:     11.5779
# Package:   LQFP-208(28x28)

kicli jlcpcb search <query> [options]
# Options:
#   -n N            max results (default 10)
#   --basic         only basic parts (no extra assembly fee)
#   --extended      only extended parts
#   --in-stock      only parts currently in stock
#   --package PKG   filter by package (e.g. 0402, LQFP-48)
#
# Output: CSV (LCSC,Type,Model,Package,Stock,Price,Description)
# Type: base = basic (no extra fee), expand = extended

kicli jlcpcb bom <file> [-o out.csv]
# Generates JLCPCB-ready BOM CSV from schematic
# Comment,Designator,Footprint,LCSC
# 100nF,"C1 C2 C3",Capacitor_SMD:C_0402_1005Metric,C1525
# 10k,"R1 R2",Resistor_SMD:R_0402_1005Metric,C25744
# Empty LCSC column = part number not yet assigned
```

---

## Configuration

```
~/.config/kicli/config.toml    # global (auto-created)
.kicli.toml                    # project-level (created by kicli new)
```

Override kicad-cli path: `KICAD_CLI_PATH=/path/to/kicad-cli`

---

## Architecture

- S-expression parser (`src/sch/parser/`) — reads KiCad 10 `.kicad_sch` files directly
- 3-pass view engine (`src/sch/view.c`) — lib_symbols → kicad-cli netlist → union-find wire tracing
- JLCPCB API (`src/jlcpcb/`) — component search, part lookup, BOM generation
- No external dependencies beyond libcurl and a C11 compiler

---

## License

MIT
