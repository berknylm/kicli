# kicli — Agent Skills

Pipe-friendly CLI for KiCad 10. All output is plain text for grep/awk/cut.

## Commands

  kicli sch <file> list [--all]     Tab-separated: REF VALUE LIB FOOTPRINT
  kicli sch <file> info <REF>       All properties of a component
  kicli sch <file> view             Pin+net connectivity (grep-friendly)
  kicli sch <file> set <REF> <FIELD> <VALUE>
  kicli sch <dir>  set-all <VALUE_MATCH> <FIELD> <NEW_VALUE>
  kicli sch <file> export pdf|svg|netlist|bom [-o FILE]
  kicli sch <file> erc [-o FILE]
  kicli jlcpcb part <LCSC_ID>       Detail, stock, price, datasheet URL
  kicli jlcpcb search <query> [-n N] [--basic|--extended] [--in-stock] [--package PKG]
  kicli jlcpcb bom <file> [-o CSV]  JLCPCB-ready BOM (Comment,Designator,Footprint,LCSC)

## Schematic recipes

  # find all ICs
  kicli sch board.kicad_sch list | grep '^U'

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

  # generate BOM and see what's missing
  kicli jlcpcb bom board.kicad_sch | grep ',$'

  # count how many BOM rows lack LCSC codes
  kicli jlcpcb bom board.kicad_sch | grep -c ',$'

  # search for a part, prefer basic (no extra JLCPCB assembly fee)
  kicli jlcpcb search "100nF 0402" --basic --in-stock -n 5

  # check part detail before assigning
  kicli jlcpcb part C1525

  # get datasheet URL for review
  kicli jlcpcb part C89418 | grep Datasheet | cut -d' ' -f2

  # assign LCSC code to one component
  kicli sch board.kicad_sch set C1 LCSC C1525

  # bulk-assign LCSC code to all components with same value
  kicli sch project/ set-all "100nF" LCSC C1525

  # export final BOM for JLCPCB
  kicli jlcpcb bom board.kicad_sch -o bom.csv

  # verify: all rows should now have LCSC codes
  kicli jlcpcb bom board.kicad_sch | grep ',$' && echo "INCOMPLETE" || echo "READY"

## Schematic review workflow

  1. kicli sch board.kicad_sch list              # inventory
  2. kicli sch board.kicad_sch view              # full connectivity
  3. view | grep '→ ~'                           # floating pins
  4. view | grep '^U1:' | grep 'pwrin'           # power pins per IC
  5. kicli jlcpcb part <LCSC> → datasheet URL    # read the datasheet
  6. Compare: every VDD pin has bypass cap, pull-ups match, crystal caps match

## BOM preparation workflow

  1. kicli jlcpcb bom board.kicad_sch            # see empty LCSC rows
  2. For each missing row:
     a. kicli jlcpcb search "<value> <package>" --basic --in-stock
     b. Pick part considering: voltage, temp, tolerance, circuit context
     c. kicli jlcpcb part <LCSC_ID>             # verify before assigning
     d. kicli sch proj/ set-all "<value>" LCSC <code>
  3. kicli jlcpcb bom board.kicad_sch            # confirm all filled
  4. kicli jlcpcb bom board.kicad_sch -o bom.csv # export for JLCPCB upload

## Reference

  Pin types: in, out, inout, pass, pwrin, pwrout, tri, oc, oe, free
  Net symbols: ~ = floating (no wire), NC = intentional no-connect
  JLCPCB types: base = basic (no extra fee), expand = extended
  set-all with directory path applies to all .kicad_sch files
  Part selection needs circuit context: voltage, temperature, tolerance, package
