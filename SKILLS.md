# kicli — Agent Skills

Pipe-friendly CLI for KiCad 10. All output is plain text for grep/awk/cut.

## Commands

  kicli sch <file> list [--all]     Tab-separated: REF VALUE LIB FOOTPRINT PartNo
  kicli sch <file> info <REF>       All properties + PartNo status
  kicli sch <file> view             Pin+net connectivity (grep-friendly)
  kicli sch <file> set <REF> <FIELD> <VALUE>
  kicli sch <dir>  set-all <VALUE> <FIELD> <NEW> [--footprint <glob>] [--dry-run]
  kicli sch <file> export pdf|svg|netlist|bom [-o FILE]
  kicli sch <file> erc [-o FILE]
  kicli jlcpcb part <LCSC_ID>       Detail, stock, price, datasheet URL
  kicli jlcpcb search <query> [-n N] [--basic|--extended] [--in-stock] [--package PKG]
  kicli jlcpcb bom <file|dir> [-o CSV]  JLCPCB-ready BOM (merges all .kicad_sch in dir)

## Schematic recipes

  # find all ICs
  kicli sch board.kicad_sch list | grep '^U'

  # components missing part numbers (5th column empty)
  kicli sch board.kicad_sch list | awk '$5 == ""'

  # count components by prefix
  kicli sch board.kicad_sch list | cut -c1 | sort | uniq -c | sort -rn

  # floating pins (ERC candidates)
  kicli sch board.kicad_sch view | grep '→ ~'

  # all power pins of U1
  kicli sch board.kicad_sch view | grep '^U1:' | grep 'pwrin'

  # nets connected to U1
  kicli sch board.kicad_sch view | grep '^U1:' | awk '{print $4}' | sort -u

  # everything on a specific net
  kicli sch board.kicad_sch view | grep '→ VDDA_3V3'

  # diff two schematics
  diff <(kicli sch old.kicad_sch list) <(kicli sch new.kicad_sch list)

## BOM recipes

  # generate BOM from all schematics in a project directory
  kicli jlcpcb bom project/ | grep ',$'

  # count missing part numbers (summary also printed to stderr)
  kicli jlcpcb bom board.kicad_sch 2>&1 >/dev/null

  # search for a part, prefer basic (no extra JLCPCB assembly fee)
  kicli jlcpcb search "100nF 0402" --basic --in-stock -n 5

  # check part detail + datasheet before assigning
  kicli jlcpcb part C1525

  # get datasheet URL
  kicli jlcpcb part C89418 | grep Datasheet | cut -d' ' -f2

  # assign part number to one component
  kicli sch board.kicad_sch set C1 LCSC C1525

  # bulk-assign with footprint filter (prevents wrong package assignment)
  kicli sch project/ set-all "100nF" LCSC C1525 --footprint "*0402*"

  # preview bulk changes before writing
  kicli sch project/ set-all "LED" LCSC C2286 --footprint "*0603*" --dry-run

  # export final BOM
  kicli jlcpcb bom project/ -o bom.csv

  # verify completeness
  kicli jlcpcb bom project/ | grep ',$' && echo "INCOMPLETE" || echo "READY"

## Schematic review workflow

  1. kicli sch board.kicad_sch list              # inventory + PartNo status
  2. kicli sch board.kicad_sch view              # full connectivity
  3. view | grep '→ ~'                           # floating pins
  4. view | grep '^U1:' | grep 'pwrin'           # power pins per IC
  5. kicli jlcpcb part <LCSC> → datasheet URL    # read the datasheet
  6. Compare: every VDD pin has bypass cap, pull-ups match, crystal caps match

## BOM preparation workflow

  1. kicli jlcpcb bom project/                   # see empty PartNo rows + summary
  2. For each missing row:
     a. kicli jlcpcb search "<value> <package>" --basic --in-stock
     b. Pick part considering: voltage, temp, tolerance, circuit context
     c. kicli jlcpcb part <LCSC_ID>             # verify before assigning
     d. kicli sch proj/ set-all "<value>" LCSC <code> --footprint "*<pkg>*"
  3. kicli jlcpcb bom project/                   # confirm all filled
  4. kicli jlcpcb bom project/ -o bom.csv        # export for JLCPCB upload

## Reference

  Pin types: in, out, inout, pass, pwrin, pwrout, tri, oc, oe, free
  Net symbols: ~ = floating (no wire), NC = intentional no-connect
  JLCPCB types: base = basic (no extra fee), expand = extended
  set-all with directory path applies to all .kicad_sch files
  Part selection needs circuit context: voltage, temperature, tolerance, package
