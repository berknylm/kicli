<p align="center">
  <img src="assets/kicli-logo.svg" alt="kicli" width="120" />
</p>

<h1 align="center">kicli</h1>

<p align="center">
  <strong>A modular command-line toolkit for KiCad — manage components, check stock, and model schematics without ever leaving your terminal.</strong>
</p>

<p align="center">
  <a href="#installation">Installation</a> •
  <a href="#commands">Commands</a> •
  <a href="#usage">Usage</a> •
  <a href="#configuration">Configuration</a> •
  <a href="#contributing">Contributing</a> •
  <a href="#license">License</a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License" />
  <img src="https://img.shields.io/badge/KiCad-7%20%7C%208%20%7C%209-brightgreen" alt="KiCad version" />
  <img src="https://img.shields.io/badge/platform-linux%20%7C%20macos%20%7C%20windows-lightgrey" alt="Platform" />
</p>

---

## The Problem

Working with KiCad projects means constantly juggling between:

- **Manually downloading** symbol, footprint, and 3D model files from various sources and placing them into the right library paths
- **Checking stock availability** on LCSC, Digikey and other suppliers — usually through a browser, one part at a time
- **Editing schematics** only through the GUI, with no way to script, automate, or batch-process changes

**kicli** solves all three. One tool. One terminal. Full control over your KiCad workflow.

---

## Commands

kicli is organized into three core subcommand groups:

### `kicli fetch` — Component Library Manager

Automatically imports external components (symbols, footprints, 3D models) into your KiCad project libraries.

```
kicli fetch <COMPONENT_ID>         Import a component by ID (LCSC, Digikey, etc.)
kicli fetch search <QUERY>         Search components across supported sources
kicli fetch info <COMPONENT_ID>    Show component details before importing
kicli fetch sync                   Re-sync all previously fetched components (update libs)
kicli fetch list                   List all fetched components in the current project
kicli fetch remove <COMPONENT_ID>  Remove a component from project libraries
kicli fetch sources                List supported component sources
```

**Examples:**

```bash
# Import a component from LCSC into your project
kicli fetch C2040

# Search for USB-C connectors
kicli fetch search "USB Type-C 16pin"

# Import from a specific source
kicli fetch --source digikey 296-1395-1-ND

# Import and assign to a specific library
kicli fetch C2040 --lib my-project-parts

# Bulk import from a CSV/BOM file
kicli fetch --from bom.csv

# Sync all fetched components to latest versions
kicli fetch sync --dry-run
```

---

### `kicli stock` — Supplier Stock Checker

Real-time stock availability and pricing from LCSC, Digikey, Mouser, and more.

```
kicli stock check <PART_NUMBERS...>   Check stock for one or more parts
kicli stock bom <BOM_FILE>            Check stock for all parts in a BOM file
kicli stock watch <PART_NUMBERS...>   Watch parts and notify on stock changes
kicli stock compare <PART_NUMBER>     Compare price/stock across suppliers
kicli stock export <BOM_FILE>         Export BOM with stock & pricing data
```

**Examples:**

```bash
# Quick stock check
kicli stock check C2040 C14663

# Check all parts in your BOM
kicli stock bom project.csv

# Compare a part across all suppliers
kicli stock compare C2040
# ┌──────────┬────────┬────────────┬───────────┐
# │ Supplier │ Stock  │ Unit Price │ Lead Time │
# ├──────────┼────────┼────────────┼───────────┤
# │ LCSC     │ 45,200 │ $0.0215    │ 3 days    │
# │ Digikey  │ 12,800 │ $0.0280    │ In stock  │
# │ Mouser   │  8,400 │ $0.0265    │ In stock  │
# └──────────┴────────┴────────────┴───────────┘

# Watch stock and get notified when it drops below threshold
kicli stock watch C2040 --below 1000 --notify slack

# Export enriched BOM with pricing
kicli stock export bom.csv --format xlsx --quantities 100,500,1000
```

---

### `kicli sch` — Schematic Modeling & Manipulation

Read, modify, and script KiCad schematics entirely from the CLI. A complete schematic modeling tool.

```
kicli sch read <FILE>                     Parse and display schematic structure
kicli sch tree <FILE>                     Show hierarchical schematic tree
kicli sch dump <FILE>                     Dump schematic in structured format (JSON/YAML)
kicli sch add <FILE> <COMPONENT>          Add a component to the schematic
kicli sch remove <FILE> <REFERENCE>       Remove a component by reference
kicli sch move <FILE> <REFERENCE> <X> <Y> Move a component to coordinates
kicli sch connect <FILE> <A> <B>          Create a wire between two pins
kicli sch disconnect <FILE> <A> <B>       Remove a connection
kicli sch rename <FILE> <OLD> <NEW>       Rename a component reference
kicli sch set <FILE> <REF> <FIELD> <VAL>  Set a field value on a component
kicli sch validate <FILE>                 Run ERC (Electrical Rules Check)
kicli sch diff <FILE_A> <FILE_B>          Diff two schematics
kicli sch export <FILE> --format <FMT>    Export schematic (pdf, svg, png, netlist)
```

**Examples:**

```bash
# Read schematic structure
kicli sch read main.kicad_sch
# Schematic: main.kicad_sch
# ├── U1  — STM32F405RGT6  (IC_MCU)
# ├── C1  — 100nF           (C_0402)
# ├── C2  — 10µF            (C_0805)
# ├── R1  — 10kΩ            (R_0402)
# └── J1  — USB_C_16Pin     (USB_C_Receptacle)

# Dump as JSON for scripting
kicli sch dump main.kicad_sch --format json > schematic.json

# Add a decoupling cap near U1
kicli sch add main.kicad_sch C_0402 --ref C5 --value "100nF" --near U1

# Connect C5 to U1's VDD pin
kicli sch connect main.kicad_sch C5:1 U1:VDD

# Connect C5's other pin to GND
kicli sch connect main.kicad_sch C5:2 GND

# Batch operations from a script
kicli sch apply main.kicad_sch --script add_decoupling.ksh

# Validate the schematic
kicli sch validate main.kicad_sch
# ✓ No ERC errors
# ⚠ 2 warnings: unconnected pins on J1

# Diff between two versions
kicli sch diff main_v1.kicad_sch main_v2.kicad_sch
# + Added: C5 (100nF)
# ~ Modified: R1 value 10kΩ → 4.7kΩ
# - Removed: R3

# Export to PDF
kicli sch export main.kicad_sch --format pdf --output schematic.pdf
```

---

## Installation

### From source

```bash
git clone https://github.com/yourusername/kicli.git
cd kicli
cargo install --path .
```

### Via cargo

```bash
cargo install kicli
```

### Via Homebrew (macOS/Linux)

```bash
brew install kicli
```

---

## Configuration

kicli looks for configuration in the following order:

1. `.kicli.toml` in the current project directory
2. `~/.config/kicli/config.toml` global config

```toml
# .kicli.toml — project-level configuration

[project]
kicad_version = "8"
lib_dir = "libs"                     # local library directory

[fetch]
default_source = "lcsc"              # default component source
auto_3d_models = true                # download 3D models automatically
symbol_lib = "project-symbols"       # target symbol library name
footprint_lib = "project-footprints" # target footprint library name

[stock]
suppliers = ["lcsc", "digikey", "mouser"]
currency = "USD"
cache_ttl = "1h"                     # cache stock results for 1 hour

[stock.alerts]
notify = "terminal"                  # terminal | slack | email | webhook
webhook_url = ""

[stock.api_keys]
# Store API keys here or use environment variables:
# KICLI_LCSC_API_KEY, KICLI_DIGIKEY_API_KEY, etc.
lcsc = ""
digikey = ""
mouser = ""

[sch]
default_format = "json"              # json | yaml | tree
backup_on_write = true               # create .bak before modifying schematics
```

---

## Project Structure

```
kicli/
├── Cargo.toml
├── README.md
├── LICENSE
├── src/
│   ├── main.rs                # CLI entry point & argument parsing
│   ├── lib.rs                 # Public library API
│   ├── config.rs              # Configuration loading
│   ├── error.rs               # Error types
│   ├── fetch/
│   │   ├── mod.rs             # fetch subcommand router
│   │   ├── sources/
│   │   │   ├── mod.rs
│   │   │   ├── lcsc.rs        # LCSC component fetcher
│   │   │   ├── digikey.rs     # Digikey component fetcher
│   │   │   └── snapeda.rs     # SnapEDA component fetcher
│   │   ├── importer.rs        # Import into KiCad libraries
│   │   └── registry.rs        # Track fetched components
│   ├── stock/
│   │   ├── mod.rs             # stock subcommand router
│   │   ├── checker.rs         # Stock check logic
│   │   ├── watcher.rs         # Stock watch / alerts
│   │   ├── bom.rs             # BOM parsing
│   │   └── suppliers/
│   │       ├── mod.rs
│   │       ├── lcsc.rs
│   │       ├── digikey.rs
│   │       └── mouser.rs
│   └── sch/
│       ├── mod.rs             # sch subcommand router
│       ├── parser.rs          # KiCad schematic file parser
│       ├── model.rs           # Schematic data model (in-memory)
│       ├── writer.rs          # Write back to .kicad_sch format
│       ├── ops.rs             # Add, remove, move, connect operations
│       ├── diff.rs            # Schematic diffing
│       ├── validate.rs        # ERC validation
│       └── export.rs          # Export to PDF/SVG/netlist
└── tests/
    ├── fetch_tests.rs
    ├── stock_tests.rs
    ├── sch_tests.rs
    └── fixtures/
        ├── sample.kicad_sch
        └── sample_bom.csv
```

---

## Roadmap

- [x] Core CLI structure and config system
- [ ] `kicli fetch` — LCSC source
- [ ] `kicli fetch` — Digikey source
- [ ] `kicli fetch` — SnapEDA source
- [ ] `kicli stock check` — LCSC, Digikey, Mouser
- [ ] `kicli stock bom` — BOM parsing and batch check
- [ ] `kicli stock watch` — real-time stock alerts
- [ ] `kicli sch read` — schematic parser
- [ ] `kicli sch dump` — JSON/YAML export
- [ ] `kicli sch add/remove/move` — schematic manipulation
- [ ] `kicli sch connect` — wire routing
- [ ] `kicli sch validate` — ERC from CLI
- [ ] `kicli sch diff` — schematic comparison
- [ ] Plugin system for custom component sources
- [ ] KiCad 9 full support
- [ ] Interactive TUI mode

---

## Contributing

Contributions are welcome! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

```bash
git clone https://github.com/yourusername/kicli.git
cd kicli
cargo build
cargo test
```

---

## License

MIT © [Your Name](https://github.com/yourusername)
