# kicli — Agent Skills

Pipe-friendly CLI for KiCad 10. All output is plain text for grep/awk/cut.

## Commands

  kicli sch <file|dir> list [--all]     Tab-separated: REF\tVALUE\tLIB\tFOOTPRINT\tPartNo
                                        --all includes power/virtual symbols
                                        On a dir: appends a 6th column — SHEET
                                        (Missing PartNo renders as "(unset)")
  kicli sch <file|dir> info <REF> [--pins]
                                        All properties + PartNo status.
                                        --pins adds the full pin table
                                        (NUM \t NAME \t TYPE \t NET).
                                        With a directory, walks every sheet
                                        and stops at the first match.
  kicli sch <file|dir> view [-o FILE]   Full pin+net connectivity table.
                                        With a dir: walks every .kicad_sch
                                        and resolves net names across sheets
                                        via the root schematic's netlist
                                        (sheet pins ↔ hierarchical labels
                                        bridge correctly — e.g. child's P3V3
                                        shows as parent's +3.3V).
  kicli sch <file> set <REF> <FIELD> <VALUE>
  kicli sch <file|dir> set-all <VALUE> <FIELD> <NEW> [--footprint <glob>] [--dry-run]
  kicli sch <file> export pdf|svg|netlist|bom [-o FILE]
  kicli sch <file> erc [-o FILE|-]      Use -o - to stream the report to stdout
  kicli jlcpcb part <LCSC_ID>       Detail, stock, price, datasheet URL
  kicli jlcpcb search <query> [-n N] [--basic|--extended] [--in-stock] [--package PKG]
  kicli jlcpcb bom <file|dir> [-o CSV]  JLCPCB-ready BOM (merges all .kicad_sch in dir)
  kicli import <file.zip> [-l LIB] [--project DIR]   Import vendor ZIP (symbol+footprint+3D)
  kicli import --list [--project DIR]                 List imported libraries

## Schematic recipes

  # find all ICs (single sheet)
  kicli sch board.kicad_sch list | grep '^U'

  # find all ICs across every sheet in a project
  kicli sch project/ list | grep '^U'

  # components missing part numbers (PartNo column = (unset))
  kicli sch project/ list | awk -F'\t' '$5 == "(unset)"'

  # per-sheet component counts
  kicli sch project/ list | awk -F'\t' '{print $6}' | sort | uniq -c | sort -rn

  # count components by prefix
  kicli sch board.kicad_sch list | cut -c1 | sort | uniq -c | sort -rn

  # floating pins (ERC candidates) — across the whole project
  kicli sch project/ view | grep '→ ~'

  # every pin on a specific net, across every sheet
  kicli sch project/ view | grep '→ +3.3V'

  # all power pins of U1 (single-sheet query still works)
  kicli sch board.kicad_sch view | grep '^U1:' | grep 'pwrin'

  # nets connected to U1, unique
  kicli sch project/ view | grep '/U1:\|^U1:' | awk '{print $4}' | sort -u

  # diff two schematics
  diff <(kicli sch old.kicad_sch list) <(kicli sch new.kicad_sch list)

  # full pin dump for a specific IC (NUM NAME TYPE NET)
  kicli sch project/ info U1 --pins

  # stream ERC report to stdout for piping
  kicli sch board.kicad_sch erc -o - | grep -E '^\[|violations'

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

  1. kicli sch project/  list                    # inventory + PartNo status
                                                   (points at a directory to
                                                    merge every sheet)
  2. kicli sch project/  view                    # full cross-sheet connectivity
                                                   (resolves sheet-pin ↔
                                                    hierarchical-label bridges)
  3. view | grep '→ ~'                           # floating pins (all sheets)
  4. view | grep '/U1:\|^U1:' | grep 'pwrin'     # power pins of U1 regardless
                                                   of which sheet it lives on
  5. kicli sch project/ info U1 --pins           # full NUM / NAME / TYPE / NET
                                                   dump for the IC
  6. kicli jlcpcb part <LCSC> → datasheet URL    # read the datasheet
  7. Compare: every VDD pin has bypass cap, pull-ups match, crystal caps match
  8. kicli sch project/top.kicad_sch erc -o -    # stream ERC violations to
                                                   stdout for grep/triage

## BOM preparation workflow

  1. kicli jlcpcb bom project/                   # see empty PartNo rows + summary
  2. For each missing row:
     a. kicli jlcpcb search "<value> <package>" --basic --in-stock
     b. Pick part considering: voltage, temp, tolerance, circuit context
     c. kicli jlcpcb part <LCSC_ID>             # verify before assigning
     d. kicli sch proj/ set-all "<value>" LCSC <code> --footprint "*<pkg>*"
  3. kicli jlcpcb bom project/                   # confirm all filled
  4. kicli jlcpcb bom project/ -o bom.csv        # export for JLCPCB upload

## Component library import workflow

  # import vendor ZIP (SnapEDA, Ultra Librarian, CSE) into project
  kicli import ~/Downloads/LM358.zip -l op_amps

  # import from Ultra Librarian (ul_ prefix auto-stripped)
  kicli import ~/Downloads/ul_RP2040.zip -l microcontrollers

  # import from CSE (LIB_ prefix auto-stripped)
  kicli import ~/Downloads/LIB_PESD_0402.zip

  # auto-derived library name (from zip filename)
  kicli import ~/Downloads/LM358.zip

  # list all imported libraries
  kicli import --list

  # after import: symbol available as op_amps:LM358 in KiCad

## Reference

  Pin types: in, out, inout, pass, pwrin, pwrout, tri, oc, oe, free, nc
  Net symbols: ~ = floating (no wire), NC = intentional no-connect
  PartNo column: "(unset)" when the LCSC/PartNo field is missing
  JLCPCB types: base = basic (no extra fee), expand = extended
  Directory arg on list / view / info / set-all / jlcpcb bom walks every .kicad_sch
  view on a dir resolves nets via the root schematic's kicad-cli netlist,
  so cross-sheet bridges (sheet pin ↔ hierarchical label) show under one name
  Part selection needs circuit context: voltage, temperature, tolerance, package
