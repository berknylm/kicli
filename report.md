# kicli — Build, Test & Bug Report

**Date:** 2026-04-04  
**Platform:** Windows 11 (x86_64)  
**Compiler:** GCC 15.2.0 (MinGW-w64, via scoop)  
**CMake:** 4.3.1 | **Ninja:** 1.13.2 | **libcurl:** 8.19.0

---

## 1. Environment Setup

Build tools were not installed. The following were installed via **scoop** (user-level, no admin required):

| Tool | Version | Source |
|------|---------|--------|
| cmake | 4.3.1 | scoop main |
| ninja | 1.13.2 | scoop main |
| mingw (gcc) | 15.2.0 | scoop main |
| curl (libcurl) | 8.19.0 | scoop main |

CMake configure command used:

```powershell
cmake -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=gcc \
  -DCURL_INCLUDE_DIR=~\scoop\apps\curl\current\include \
  -DCURL_LIBRARY=~\scoop\apps\curl\current\lib\libcurl.dll.a
```

---

## 2. Bugs Found & Fixed

### Bug 1 — Missing `#include <direct.h>` in `registry.c`

**File:** `src/fetch/registry.c:22`  
**Severity:** Compile error (build fails)

**Problem:** `_mkdir()` is declared in `<direct.h>` on MinGW/MSVC, but the file only included `<sys/stat.h>`, causing:

```
error: implicit declaration of function '_mkdir'; did you mean 'mkdir'?
```

**Fix:** Added `#include <direct.h>` inside the `#ifdef _WIN32` guard at the top of the file.

---

### Bug 2 — `open_memstream` not available on Windows in `lcsc.c`

**File:** `src/fetch/sources/lcsc.c:106` and `:266`  
**Severity:** Compile error (build fails)

**Problem:** `open_memstream()` is a POSIX extension (GNU libc) not available on Windows or MinGW. Used in two places:

1. `generate_kicad_sym()` — builds a `.kicad_sym` string
2. Footprint block in `lcsc_fetch()` — builds a `.kicad_mod` string

This caused:
```
error: implicit declaration of function 'open_memstream'
error: initialization of 'FILE *' from 'int' makes pointer from integer without a cast
```

**Fix:** Replaced both `open_memstream` / `fprintf` / `fclose` patterns with `malloc` + `snprintf`, sizing the buffer dynamically based on input string lengths plus a fixed overhead. The generated content is identical.

---

### Bug 3 — Hardcoded `/tmp/` path on Windows in `lcsc.c`

**File:** `src/fetch/sources/lcsc.c:299`  
**Severity:** Runtime bug (wrong temp file path on Windows)

**Problem:** 3D model (STEP) downloads were written to `/tmp/kicli_<ID>.step`, which is not a valid path on Windows.

**Fix:** Added a `#ifdef _WIN32` block that uses `%TEMP%` (falling back to `%TMP%`, then `C:\Temp`) to construct the temporary path:

```c
#ifdef _WIN32
    const char *tmp_dir = getenv("TEMP");
    if (!tmp_dir) tmp_dir = getenv("TMP");
    if (!tmp_dir) tmp_dir = "C:\\Temp";
    snprintf(tmp_path, sizeof(tmp_path), "%s\\kicli_%s.step", tmp_dir, id);
#else
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/kicli_%s.step", id);
#endif
```

---

### Bug 4 — Missing runtime DLL (`libcurl-x64.dll`)

**Severity:** Runtime error (binary crashes with exit code `0xC0000135` = `STATUS_DLL_NOT_FOUND`)

**Problem:** The binary links against `libcurl.dll.a` (import library), which requires `libcurl-x64.dll` at runtime. Without it on PATH, the process terminates immediately.

**Fix:** Copied `libcurl-x64.dll` from the scoop installation into `build/debug/bin/` alongside `kicli.exe`. For a release build this DLL must be bundled or users must have curl in PATH.

---

## 3. Warnings (Not Fixed)

These are non-fatal warnings from vendor code and stubs:

- **`vendor/optparse/optparse.h`** — `optparse_init`, `optparse_arg`, `optparse_long` defined but not used. These are static functions in a header-only library included in multiple translation units with `-Wall`. Not a bug; suppressing would require `-Wno-unused-function` for vendor code.
- **`kicad_cli.c:68`** — `%s` directive may truncate (format-truncation warning). The `KICAD_CLI_MAX_PATH` buffer (512 bytes) is smaller than the PATH search buffer (4096 bytes). Low real-world risk.
- **`snapeda.c:76`** — unused parameter `out`. This function is a stub.

---

## 4. Test Results

```
Test project C:/Users/ASUS/Desktop/Projeler/kicli/build/debug
    Start 1: test_sch    → Passed (0.07s)
    Start 2: test_fetch  → Passed (0.07s)
    Start 3: test_stock  → Passed (0.05s)

100% tests passed, 0 tests failed out of 3
```

**Note:** All three test executables are stubs (`printf("tests: not yet implemented\n"); return 0;`). No real functionality is tested by the test suite yet.

---

## 5. Functional Test Results

| Command | Result |
|---------|--------|
| `kicli --version` | `kicli 0.1.0` ✓ |
| `kicli --help` | Usage displayed ✓ |
| `kicli kicad-path` | Error: KiCad 10 not installed (expected) ✓ |
| `kicli kicad-version` | Error: KiCad 10 not installed (expected) ✓ |
| `kicli new testproject` | Project created with all files and directories ✓ |
| `kicli new` (no args) | Usage displayed ✓ |
| `kicli sch --help` | Usage displayed ✓ |
| `kicli fetch --help` | Usage displayed ✓ |
| `kicli stock --help` | Usage displayed ✓ |
| `kicli unknown-command` | Error message + exit code 1 ✓ |
| `kicli sch foo` | "not yet implemented: sch" ✓ |
| `kicli fetch C2040` | "not yet implemented: fetch" ✓ |
| `kicli stock C2040` | "not yet implemented: stock" ✓ |

`kicli new testproject` produced the correct project structure:
```
testproject/
  testproject.kicad_pro
  testproject.kicad_sch
  sym-lib-table
  fp-lib-table
  .kicli.toml
  libs/
    symbols/
    footprints/
      testproject-footprints.pretty/
    3dmodels/
```

---

## 6. Implementation Status

| Module | Status |
|--------|--------|
| `kicli new` | Fully implemented |
| `kicli kicad-path` / `kicad-version` | Implemented (requires KiCad) |
| `kicli sch <file> <cmd>` | Stub only |
| `kicli fetch <ID>` | Stub dispatcher; LCSC/EasyEDA backend implemented but not wired up |
| `kicli stock <part>` | Stub only |
| DigiKey / SnapEDA sources | Stub/partial |
| Test suite | Stubs only (3 placeholder executables) |

---

## 7. Recommendations

1. **Statically link libcurl** (or bundle the DLL) for Windows releases so the binary is self-contained.
2. **Wire up the `lcsc_fetch` / `lcsc_search` / `lcsc_info` implementations** in `fetch.c` — the backend logic is written but the dispatcher (`cmd_fetch`) still prints "not yet implemented".
3. **Write real unit tests** — the test suite is empty; at minimum test `cmd_new`, the s-expression parser, and the registry read/write logic.
4. **Guard `/tmp/` usages** — there may be other POSIX-only path assumptions in future code; establish a `kicli_tmpdir()` helper that returns the right temp directory per platform.
5. **Consider static linking for releases** — the GitHub Actions workflow already downloads a static curl build for CI, which avoids the DLL dependency issue entirely.
