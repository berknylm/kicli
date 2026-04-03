# kicli — Similar Projects Research

> This file surveys GitHub repos that overlap with kicli's three pillars:
> component fetching, stock checking, and schematic manipulation/scripting.
> Sorted by relevance to kicli's goals.

---

## 1. kicad-happy
**Repo:** https://github.com/aklofas/kicad-happy  
**Language:** Python (Claude Code / AI agent skills)  
**Stars:** Active, recently maintained  

### What it does
AI-powered skill set for KiCad that integrates directly into Claude Code and OpenAI Codex. Covers the full design workflow: schematic analysis, PCB layout review, BOM management (multi-distributor), Gerber prep, SPICE simulation, DRC/ERC, and datasheet fetching.

### Overlap with kicli
- BOM enrichment and per-supplier order file export (DigiKey, Mouser, LCSC, element14, PCBWay, JLCPCB)
- KiCad schematic + PCB parsing and analysis
- Component sourcing and part matching

### Key difference
Not a standalone CLI — it's a set of Claude Code agent skills. Requires an AI runtime. No stock watching or batch LCSC fetch by part ID.

### Verdict
**High overlap on fetch/stock side.** Would be a strong reference for BOM field detection logic and distributor API integration.

---

## 2. kandle
**Repo:** https://github.com/HarveyBates/kandle  
**Language:** C / CMake  
**Stars:** Moderate  

### What it does
CLI that auto-imports external components (symbols, footprints, 3D models) into a KiCad project. Takes a downloaded `.zip` from a component vendor, places files into a defined directory structure, converts KiCad v4 symbols to v6, and links symbols to footprints automatically.

```bash
kandle -f Downloads/LM358.zip -l operational_amplifier
```

### Overlap with kicli
- Direct equivalent of `kicli fetch` — this is the original inspiration
- Same directory structure concept for `libs/`
- Handles 3D model placement

### Key difference
Manual workflow — you download the ZIP yourself, then run kandle. No API calls to LCSC/DigiKey. No stock checking. No schematic manipulation. C, not Rust.

### Verdict
**Closest structural analog to kicli fetch.** Read its source for library layout conventions. kicli improves on it by automating the download step.

---

## 3. kicad-skip
**Repo:** https://github.com/psychogenic/kicad-skip  
**PyPI:** `pip install kicad-skip`  
**Language:** Python  
**Stars:** Active  

### What it does
Python library that parses KiCad 7+ `.kicad_sch` (and PCB) s-expression files into a friendly object model. Lets you add, modify, clone, and wire components programmatically. Supports searching by location, type, or connection.

```python
sch = skip.Schematic('main.kicad_sch')
for sym in sch.symbol:
    print(sym.property.Reference.value)
```

### Overlap with kicli
- Direct equivalent of `kicli sch` — schematic read/parse/modify
- Handles wire connections, labels, properties
- Can be used as the parser backend under kicli sch

### Key difference
Python library, not a CLI. No built-in commands — you write scripts against its API. No ERC, no diff, no stock.

### Verdict
**Best schematic parser available.** Strongly consider wrapping or porting its s-expression parsing logic for `kicli sch`. Could save months of parser work.

---

## 4. SKiDL
**Repo:** https://github.com/devbisme/skidl  
**Docs:** https://devbisme.github.io/skidl/  
**PyPI:** `pip install skidl`  
**Language:** Python  
**Stars:** Well-established (~1.5k stars)  

### What it does
Design circuits entirely in Python code — instantiate parts, connect pins with nets, run ERC, then generate a KiCad netlist (`.net`), BOM, SVG schematic, or even SPICE simulation.

```python
from skidl import *
vcc, gnd = Net('VCC'), Net('GND')
r1 = Part('Device', 'R', footprint='R_0402')
r1['~,~'] += vcc, gnd
generate_netlist()
```

### Overlap with kicli
- Code-first schematic creation (different angle from `kicli sch add`)
- BOM generation
- ERC check

### Key difference
Inverts the workflow — you write Python to *create* a schematic, rather than manipulating an existing `.kicad_sch` file. Not a CLI for existing projects. No stock checking. No component library fetching.

### Verdict
**Low direct overlap, high conceptual value.** Useful if kicli ever grows a scripting DSL (`kicli sch apply --script`). Not a competitor for day-to-day use.

---

## 5. KiKit
**Repo:** https://github.com/yaqwsx/KiKit  
**Docs:** https://yaqwsx.github.io/KiKit/  
**Language:** Python  
**Stars:** Very popular (~2k stars)  

### What it does
Full CLI automation toolkit focused on panelization, Gerber export, DRC, and fab-house preset exports (JLCPCB, PCBWay, etc.).

```bash
kikit fab jlcpcb --assembly board.kicad_pcb gerbers/
kikit panelize grid --rows 2 --cols 3 board.kicad_pcb panel.kicad_pcb
```

### Overlap with kicli
- `kicli sch export` → KiKit's `export` subcommand
- DRC from CLI → KiKit's `drc` subcommand
- Gerber / manufacturing file generation

### Key difference
PCB-layout focused (`.kicad_pcb`), not schematic focused (`.kicad_sch`). No component fetching or stock checking. Python, not Rust.

### Verdict
**Complementary, not competing.** Users will likely use KiKit alongside kicli. Consider noting KiKit in kicli's docs for fabrication steps.

---

## 6. kicad-mcp
**Repo:** https://github.com/lamaalrajih/kicad-mcp  
**Language:** Python  
**Stars:** Growing  

### What it does
Model Context Protocol (MCP) server that exposes KiCad project data to AI assistants (Claude Desktop, etc.). Capabilities include project listing, schematic analysis, netlist extraction, BOM management, and DRC.

### Overlap with kicli
- Schematic parsing and analysis
- BOM extraction
- Design rule checking

### Key difference
Server/protocol bridge for AI tools — not a user-facing CLI. Requires an MCP client. No component fetching or stock monitoring.

### Verdict
**Indirect overlap.** kicli could potentially expose an MCP interface in the future. Good reference for what AI clients expect from KiCad data.

---

## 7. jlc-cli
**Repo:** https://github.com/l3wi/jlc-cli  
**Language:** JavaScript/Node.js  
**Stars:** Small  

### What it does
Interactive TUI for searching JLCPCB/LCSC components, downloading them, and installing them into KiCad libraries. Closest CLI equivalent to `kicli fetch` from LCSC.

```bash
jlc-cli search "USB Type-C"
jlc-cli install C165948
```

### Overlap with kicli
- Direct LCSC component search and download
- Installs symbols + footprints into KiCad
- Interactive TUI mode

### Key difference
Node.js, not Rust. LCSC/JLCPCB only — no DigiKey, Mouser, SnapEDA. No stock watching. No schematic manipulation.

### Verdict
**Closest CLI equivalent to `kicli fetch --source lcsc`.** Study its LCSC API calls and library installation logic.

---

## 8. kicad-tools (rjwalters)
**Repo:** https://github.com/rjwalters/kicad-tools  
**Language:** Python  
**Updated:** January 2026  

### What it does
Standalone Python scripts for parsing and manipulating KiCad schematic and PCB files. Designed to be used by AI agents to programmatically read and modify KiCad files.

### Overlap with kicli
- Schematic file parsing
- Component/property manipulation

### Key difference
Script collection, not a cohesive CLI. AI-agent oriented. No stock or fetch functionality.

### Verdict
**Reference implementation for schematic parsing edge cases.** Worth reading before writing kicli's own parser.

---

## 9. kiri
**Repo:** https://github.com/leoheck/kiri  
**Language:** Shell / Python  
**Stars:** Moderate  

### What it does
Visual diff tool for KiCad projects stored in Git. Renders side-by-side schematic and PCB layout comparisons between commits.

### Overlap with kicli
- `kicli sch diff` — schematic comparison

### Key difference
Web/GUI output, not terminal diff. Git-aware (diffs between commits, not two arbitrary files). No fetch/stock.

### Verdict
**Narrow overlap with `kicli sch diff`.** Consider linking kiri in docs for visual diff use cases; kicli's diff is terminal/text-oriented.

---

## 10. kicad-automation-scripts (productize)
**Repo:** https://github.com/productize/kicad-automation-scripts  
**Language:** Python  

### What it does
Scripts that automate KiCad GUI operations via the PCBNew Python library and UI automation (xdotool, etc.). Exports Gerbers, runs ERC/DRC, generates PDFs — all without touching the GUI manually.

### Overlap with kicli
- `kicli sch export` (PDF, Gerber)
- ERC/DRC automation

### Key difference
UI automation approach (requires a running X display). Fragile — depends on KiCad GUI internals. The modern `kicad-cli` binary has made this largely obsolete.

### Verdict
**Historical reference only.** kicli's approach (direct file parsing + `kicad-cli` subprocess) is cleaner.

---

## Summary Table

| Project | Language | Fetch | Stock | Sch Manipulation | Active |
|---|---|:---:|:---:|:---:|:---:|
| **kicli** *(this)* | Rust | ✓ | ✓ | ✓ | ✓ |
| kicad-happy | Python/AI | ✓ | partial | ✓ | ✓ |
| kandle | C | ✓ | — | — | ~stale |
| kicad-skip | Python | — | — | ✓ | ✓ |
| SKiDL | Python | — | — | create-only | ✓ |
| KiKit | Python | — | — | PCB only | ✓ |
| kicad-mcp | Python | — | — | read/BOM | ✓ |
| jlc-cli | Node.js | LCSC only | — | — | ~ |
| kicad-tools | Python | — | — | ✓ | ✓ |
| kiri | Shell | — | — | diff/visual | ~ |

---

## Recommendations

1. **Parser:** Use or port `kicad-skip`'s s-expression parser for `kicli sch`. It's the most mature open implementation.
2. **LCSC fetch API:** Study `jlc-cli` and `kandle` for library directory layout conventions.
3. **BOM/supplier logic:** Study `kicad-happy`'s BOM skill for distributor field detection heuristics.
4. **Export/ERC:** Delegate to the official `kicad-cli` binary where possible (available since KiCad 7), rather than reimplementing.
5. **Differentiation:** No existing tool combines all three (fetch + stock + schematic) in one Rust CLI. kicli's niche is clear.
