<p align="center">
  <img src="assets/kicli-logo.svg" alt="kicli" width="120" />
</p>

<h1 align="center">kicli</h1>

<p align="center">
  <strong>Agent-friendly KiCad CLI — create projects, list components, fetch parts, check stock.</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/KiCad-10-brightgreen" alt="KiCad 10" />
  <img src="https://img.shields.io/badge/platform-macOS%20%7C%20Windows%20%7C%20Linux-lightgrey" />
  <img src="https://img.shields.io/badge/license-MIT-blue" />
</p>

---

## What is kicli?

`kicli` is a thin, pipe-friendly CLI wrapper around KiCad 10. It is designed for use by AI agents and automation scripts:

```bash
kicli sch board.kicad_sch list | grep "^U"       # all ICs
kicli sch board.kicad_sch list | grep TPS        # find TPS parts
kicli sch board.kicad_sch export pdf             # delegates to kicad-cli
kicli fetch C2040                                 # fetch from LCSC
kicli stock C2040 C14663                          # check stock & price
```

It finds `kicad-cli` automatically on any platform — no configuration needed.

---

## Installation

### Requirements

- **KiCad 10** installed ([kicad.org](https://www.kicad.org/download/))
- **CMake 3.19+**
- **A C11 compiler** (clang, gcc, or MSVC)
- **libcurl** (pre-installed on macOS; `apt install libcurl4-openssl-dev` on Linux; bundled with Windows)
- **Ninja** (optional but recommended: `brew install ninja` / `choco install ninja`)

### macOS

```bash
git clone https://github.com/berknylm/kicli.git
cd kicli
cmake --preset release
cmake --build build/release
sudo cmake --install build/release
```

Or without sudo (installs to `~/.local/bin`):

```bash
cmake --install build/release --prefix ~/.local
# Add ~/.local/bin to PATH if not already there
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.zshrc
```

### Windows

Open **Developer Command Prompt** (or PowerShell with MSVC):

```powershell
git clone https://github.com/berknylm/kicli.git
cd kicli
cmake -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --config Release
cmake --install build/release
```

> **Note:** libcurl is required. Install via vcpkg: `vcpkg install curl:x64-windows` and add `-DCMAKE_TOOLCHAIN_FILE=...` to the cmake configure step.

### Linux

```bash
# Install dependencies
sudo apt install cmake ninja-build libcurl4-openssl-dev  # Debian/Ubuntu
# or
sudo dnf install cmake ninja-build libcurl-devel         # Fedora

git clone https://github.com/berknylm/kicli.git
cd kicli
cmake --preset release
cmake --build build/release
sudo cmake --install build/release
```

### Verify

```bash
kicli kicad-path       # → /Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
kicli kicad-version    # → 10.0.0
kicli --version        # → kicli 0.1.0
```

---

## Quick start

```bash
# Create a new KiCad 10 project
kicli new myboard

# Open in KiCad
kicad myboard/myboard.kicad_pro

# Fetch a component from LCSC (e.g. 100nF 0402 cap)
cd myboard
kicli fetch C14663

# Check stock
kicli stock C14663

# List all components in the schematic
kicli sch myboard.kicad_sch list
```

---

## Commands

### `kicli new <name>`

Create a new KiCad 10 project with a full local library structure.

```
myboard/
  myboard.kicad_pro       KiCad project file
  myboard.kicad_sch       Blank schematic
  sym-lib-table           Symbol library → libs/symbols/
  fp-lib-table            Footprint library → libs/footprints/
  libs/symbols/
  libs/footprints/myboard-footprints.pretty/
  libs/3dmodels/
  .kicli.toml             kicli project config
```

### `kicli kicad-path` / `kicli kicad-version`

Show where `kicad-cli` was found and which KiCad version is installed.
Override the auto-detected path with `KICAD_CLI_PATH` environment variable.

### `kicli sch <file> <command>`

| Command | Description |
|---------|-------------|
| `list` | One component per line: `REF VALUE LIB_ID` |
| `info <ref>` | Detailed component info |
| `nets` | All nets and connected pins |
| `tree` | Schematic hierarchy |
| `export <fmt>` | Export via kicad-cli (pdf/svg/netlist/bom) |
| `erc` | Electrical rules check via kicad-cli |
| `set <ref> <field> <val>` | Edit a field value |
| `add <lib:sym>` | Add a component |
| `remove <ref>` | Remove a component |

### `kicli fetch <LCSC_ID>`

Download and install a component from LCSC into the local library.

```bash
kicli fetch C2040              # fetch by LCSC ID
kicli fetch search "USB-C"     # search
kicli fetch list               # list fetched components
```

### `kicli stock <part> [part...]`

Real-time stock and pricing from LCSC, Digikey, and Mouser.

```bash
kicli stock C2040 C14663
kicli stock bom bom.csv
kicli stock compare C2040
```

---

## Configuration

kicli loads config in this order (project overrides global):

1. `~/.config/kicli/config.toml` — global
2. `.kicli.toml` in the current directory — project

```toml
[project]
kicad_version = "10"
lib_dir = "libs"

[fetch]
default_source = "lcsc"
auto_3d_models = true
symbol_lib = "myboard-symbols"
footprint_lib = "myboard-footprints"

[stock]
suppliers = ["lcsc", "digikey", "mouser"]
currency = "USD"

[stock.api_keys]
lcsc = ""
digikey = ""
mouser = ""
```

---

## Environment variables

| Variable | Description |
|----------|-------------|
| `KICAD_CLI_PATH` | Override kicad-cli path (e.g. for non-standard installs) |
| `KICLI_LCSC_API_KEY` | LCSC API key |
| `KICLI_DIGIKEY_API_KEY` | DigiKey API key |
| `KICLI_MOUSER_API_KEY` | Mouser API key |

---

## Building from source

```bash
git clone https://github.com/berknylm/kicli.git
cd kicli

# Debug build
cmake --preset debug
cmake --build build/debug
./build/debug/bin/kicli --version

# Release build
cmake --preset release
cmake --build build/release
```

### CMake presets

| Preset | Description |
|--------|-------------|
| `debug` | Debug build in `build/debug/` |
| `release` | Optimized build in `build/release/` |

---

## License

MIT
