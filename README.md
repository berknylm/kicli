## kicli

Pipe-friendly CLI for KiCad 10. Designed for AI agents and automation.

```bash
kicli sch board.kicad_sch list              # list all components
kicli sch board.kicad_sch list | grep "^U"  # find ICs
kicli jlcpcb search "100nF 0402"            # search JLCPCB catalog
kicli sch project/ set-all "100nF" LCSC C1525  # assign LCSC part numbers
kicli jlcpcb bom board.kicad_sch            # export JLCPCB-ready BOM
```

---

## Install

**Build from source** (requires CMake 3.19+, C11 compiler, libcurl, Ninja):

```bash
git clone https://github.com/berknylm/kicli.git && cd kicli
cmake --preset release && cmake --build build/release
sudo cmake --install build/release
```

Pre-built binaries: [Releases](https://github.com/berknylm/kicli/releases/latest)

> Requires **KiCad 10** — [kicad.org/download](https://www.kicad.org/download/)

---

## Commands

### Project

```bash
kicli new myboard                    # create KiCad 10 project with local libs
kicli config kicad-path              # show kicad-cli path
kicli config kicad-path /usr/bin/kicad-cli  # set it manually
kicli kicad-version                  # → 10.0.0
```

### Schematic — Read

```bash
kicli sch <file> list [--all]
# R1        10k       Device:R        Resistor_SMD:R_0402_1005Metric
# C1        100nF     Device:C        Capacitor_SMD:C_0402_1005Metric

kicli sch <file> info R1
# R1
#   lib_id:     Device:R
#   value:      10k
#   footprint:  Resistor_SMD:R_0402_1005Metric
#   in_bom:     yes
#   ...

kicli sch <file> nets         # all net labels
kicli sch <file> stats        # component/wire/net counts
kicli sch <file> dump [-o out.kisch]  # full pin+net table
```

### Schematic — Write

```bash
kicli sch <file> set <REF> <FIELD> <VALUE>
# kicli sch board.kicad_sch set R1 LCSC C25744
# → set R1.LCSC = C25744

kicli sch <file|dir> set-all <VALUE_MATCH> <FIELD> <NEW_VALUE>
# kicli sch project/ set-all "100nF" LCSC C1525
# → sets LCSC=C1525 on all components with value "100nF"
#   across every .kicad_sch file in the directory
```

### Schematic — Export (kicad-cli passthrough)

```bash
kicli sch <file> export pdf          # schematic PDF
kicli sch <file> export svg          # schematic SVG
kicli sch <file> export netlist      # netlist
kicli sch <file> export bom          # KiCad BOM
kicli sch <file> erc                 # electrical rules check
```

### JLCPCB

```bash
kicli jlcpcb part <LCSC_ID>
# kicli jlcpcb part C2040
# Brand:    Raspberry Pi
# Model:    RP2040
# Package:  LQFN-56(7x7)
# Count:    127263
# Price:    0.7079

kicli jlcpcb search <query> [-n N]
# kicli jlcpcb search "100nF 0402" -n 5
# LCSC        Model                  Package   Stock
# C1525       CL05B104KO5NNNC        0402      41526899
# C1581       0402F104M500NT          0402      3373705
# ...

kicli jlcpcb bom <file> [-o out.csv]
# kicli jlcpcb bom board.kicad_sch
# Comment,Designator,Footprint,LCSC
# 100nF,C1 C2 C3,Capacitor_SMD:C_0402_1005Metric,C1525
# 10k,R1 R2,Resistor_SMD:R_0402_1005Metric,C25744
```

**Typical workflow** — assign LCSC part numbers to a project:

```bash
kicli jlcpcb bom board.kicad_sch            # see what's missing
kicli jlcpcb search "100nF 0402"            # find the LCSC code
kicli sch project/ set-all "100nF" LCSC C1525  # write it to all schematics
kicli jlcpcb bom board.kicad_sch -o bom.csv # export final BOM for JLCPCB
```

---

## Configuration

```bash
~/.config/kicli/config.toml    # global (auto-created)
.kicli.toml                    # project-level (created by kicli new)
```

Override kicad-cli path: `KICAD_CLI_PATH=/path/to/kicad-cli`

---

## License

MIT
