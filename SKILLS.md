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
  kicli sch <file|dir> view [-o FILE] [--net NET]
                                        Full pin+net connectivity table.
                                        With a dir: walks every .kicad_sch
                                        and resolves net names across sheets
                                        via the root schematic's netlist
                                        (sheet pins ↔ hierarchical labels
                                        bridge correctly — e.g. child's P3V3
                                        shows as parent's +3.3V).
                                        --net NET prints a flat table of every
                                        pin connected to NET: REF:PIN\tNAME\t
                                        TYPE[\tSHEET]. Faster + pin-type aware
                                        than greping the full view.
  kicli sch <file> set <REF> <FIELD> <VALUE>
  kicli sch <file|dir> set-all <VALUE> <FIELD> <NEW> [--footprint <glob>] [--only-empty] [--dry-run]
                                        --only-empty writes only when the target field is currently empty
                                         (useful for first-time Footprint / LCSC assignment)

  kicli sch <file> place <lib_id> <ref|?> [<value>] [--at X,Y] [--angle 0|90|180|270]
                                                                 [--mirror x|y] [--footprint FP]
                                        Add a symbol from the bundled KiCad catalog.
                                        --at auto-grids when omitted (10 columns × N rows).
                                        Pass `?` as ref to auto-annotate the next free <PREFIX><N>
                                        (Device:R → R1, R2, …; IC libs → U1, U2, …).
                                        Examples:
                                          kicli sch board.kicad_sch place Device:R       R1 10k
                                          kicli sch board.kicad_sch place Device:C       ?  100nF
                                          kicli sch board.kicad_sch place Amplifier_Operational:LM358 U1 LM358
  kicli sch <file> net <net-name> <ref>:<pin> [<ref>:<pin> ...] [--as local|global|hier|power]
                                        Attaches a label at each pin's world position — no wires.
                                        Net name matched against power rails (GND, VCC, VDD, VSS, VEE,
                                        GNDA/AGND/PGND, +BATT, EARTH, any +<voltage>V pattern) → emits
                                        a power port from the bundled `power` library; otherwise a
                                        local label. Use --as to force:
                                          local | global | hier | power
                                        --as hier writes (hierarchical_label) — match it with a sheet
                                        pin in the parent (kicli sch parent sheet … --pins NAME:io).
  kicli sch <file> nc <ref>:<pin> [<ref>:<pin> ...]
                                        Places no_connect markers at pin positions.
  kicli sch <parent> sheet <name> <child-file> [--at X,Y] [--size W,H] [--pins NAME[:type],…]
                                        Adds a hierarchical sub-sheet to <parent>. Creates
                                        <child-file> with a blank schematic if missing
                                        (existing files are left alone). Sheet pins auto-distribute
                                        on the left edge.
                                        Pin types: passive | input | output | bidirectional
                                                 | tri_state | power_in | power_out
  kicli sch <file> check [--no-erc]
                                        One-shot layout-readiness validator. Reports duplicate
                                        references (incl. #PWR collisions), missing footprints,
                                        duplicate UUIDs, and ERC error-severity violations.
                                        Exit 0 only when the schematic is layout-ready.

  kicli sch <file> export pdf|svg|netlist [-o FILE]        (BOM removed in v0.9.0 — use jlcpcb bom)
  kicli sch <file> erc [-o FILE|-] [--format report|json]
                                        Use -o - to stream the report to stdout
                                        --format json emits KiCad's structured
                                        ERC JSON (https://schemas.kicad.org/erc.v1.json)
                                        for parsing / filtering / counting
  kicli jlcpcb part <LCSC_ID>       Detail, stock, price, datasheet URL
  kicli jlcpcb search <query> [-n N] [--basic|--extended] [--in-stock] [--package PKG]
  kicli jlcpcb bom <file|dir> [-o CSV]  JLCPCB-ready BOM (merges all .kicad_sch in dir)
  kicli jlcpcb check <file|dir> [-o F] [--json]
                                        Side-by-side cross-check per component.
                                        Tab cols: REF PartNo SchValue
                                                   JLCPCBBrand JLCPCBModel
                                                   SchFootprint JLCPCBPackage
                                                   Stock Match
                                        Match ∈ {yes, NO, ?} is a SUBSTRING
                                        HEURISTIC: treat Match=NO as "needs
                                        review", NOT as a verdict. Agent must
                                        verify BGA pitch, exposed-pad (-EP)
                                        variants, imperial/metric 0402 vs 0402,
                                        and vendor naming (SMD3225-4P vs
                                        Crystal_SMD_3225-4Pin) independently.
  kicli import <file.zip> [-l LIB] [--project DIR]   Import vendor ZIP (symbol+footprint+3D)
  kicli import --list [--project DIR]                 List imported libraries

  kicli sym search <q> [--pins N] [--lib L] [-n N]    Catalog symbol search; tab: lib:name\tpins\tdescription
  kicli sym info <lib:name> [--pins]                  One symbol's metadata; --pins dumps NUM\tNAME\tTYPE
  kicli sym list [lib]                                List all symbols, optionally restrict to one library
  kicli fp  search <q> [--pads N] [--lib L] [-n N]    Catalog footprint search; tab: lib:name\tpads\ttags
                                                      NOTE: pads = total pads INCLUDING thermal via splits
                                                       on footprints with exposed pad; may exceed pin count
  kicli fp  info <lib:name>                           One footprint's metadata + filesystem path
  kicli fp  list [lib]                                List all footprints, optionally restrict to one library

## Drawing a circuit from scratch (label-first)

  kicli never routes wires — every connection is a label at the pin's world
  position. Electrically identical to a wire-based sheet, visually scattered.
  Open in KiCad to rearrange if aesthetics matter.

  # 1. Scaffold a project
  kicli new opamp_demo
  cd opamp_demo

  # 2. Place components (auto-grid if --at omitted)
  kicli sch opamp_demo.kicad_sch place Amplifier_Operational:LM358 U1 LM358
  kicli sch opamp_demo.kicad_sch place Device:R                    R1 10k
  kicli sch opamp_demo.kicad_sch place Device:R                    R2 10k
  kicli sch opamp_demo.kicad_sch place Device:C                    C1 100nF

  # 3. Attach pins to nets — one command per net, list every pin on that net.
  #    Rails auto-become power ports:
  kicli sch opamp_demo.kicad_sch net VOUT  U1:1 R1:1
  kicli sch opamp_demo.kicad_sch net VINN  U1:2 R1:2 R2:1
  kicli sch opamp_demo.kicad_sch net VINP  U1:3
  kicli sch opamp_demo.kicad_sch net GND   U1:4 R2:2 C1:2    # → power:GND
  kicli sch opamp_demo.kicad_sch net +5V   U1:8 C1:1         # → power:+5V

  # 4. NC markers on any pins you deliberately leave floating.
  kicli sch opamp_demo.kicad_sch nc U1:5 U1:6 U1:7

  # 5. Verify + open in KiCad
  kicli sch opamp_demo.kicad_sch view         # netlist, ERC-clean
  kicli sch opamp_demo.kicad_sch check        # layout-readiness gate (dup refs, footprints, UUIDs, ERC)
  kicad opamp_demo.kicad_pro                  # drag to re-layout

## Hierarchical (multi-sheet) design

  Split a design into sub-sheets when one block has its own coherent boundary
  (power supply, MCU subsystem, RF front-end, etc.). Each sub-sheet is a
  separate .kicad_sch file referenced by the parent.

  # Parent declares a sub-sheet + its border pins:
  kicli sch design.kicad_sch sheet Power power.kicad_sch \
      --pins VIN:input,+5V:output,GND:passive

  # Inside the child, draw normally — but emit hierarchical labels instead
  # of plain labels for nets that cross into the parent. They splice by name.
  kicli sch power.kicad_sch place Regulator_Switching:TPS54302 U1 TPS54302
  kicli sch power.kicad_sch net VIN  U1:3 --as hier
  kicli sch power.kicad_sch net +5V  L1:2 --as hier   # + the rest of the +5V rail
  kicli sch power.kicad_sch net GND  U1:1 --as hier

  # `kicli sch <project-dir> view` walks every .kicad_sch and resolves
  # cross-sheet net names through the root netlist.

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
  kicli sch project/ view --net +3.3V          # flat table with pin types
  kicli sch project/ view | grep '→ +3.3V'      # quick grep alternative

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

  # structured ERC output for programmatic parsing
  kicli sch board.kicad_sch erc -o - --format json | jq '.sheets[].violations[]'

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
  1b. kicli jlcpcb check project/                # after any LCSC assignment, cross-
                                                 # check vs JLCPCB API. Read BOTH
                                                 # SchValue vs JLCPCBModel AND
                                                 # SchFootprint vs JLCPCBPackage.
                                                 # Match=NO is a hint; agent
                                                 # confirms before acting.
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
  NAME column: "-" = unnamed pin (e.g. passives)     (was "~" pre-v0.7.1)
  NET  column: "~" = floating net (no wire connected)
               "NC" = intentional no-connect marker
               "name1:pin1, ..." = unlabeled wire cluster (peer list)
  PartNo column: "(unset)" when the LCSC/PartNo field is missing
  JLCPCB types: base = basic (no extra fee), expand = extended
  Directory arg on list / view / info / set-all / jlcpcb bom walks every .kicad_sch
  view on a dir resolves nets via the root schematic's kicad-cli netlist,
  so cross-sheet bridges (sheet pin ↔ hierarchical label) show under one name
  Part selection needs circuit context: voltage, temperature, tolerance, package
