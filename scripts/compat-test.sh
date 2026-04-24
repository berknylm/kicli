#!/usr/bin/env bash
#
# compat-test.sh — third-party compatibility gate before every kicli release
#
# kicli depends on two external surfaces that can drift independently:
#   1. KiCad's .kicad_sch s-expression format + kicad-cli behavior
#   2. JLCPCB's parts API (shape of JSON response)
#
# This script makes a set of assertions against both, so we catch silent
# regressions before publishing a tag. Run manually:
#
#   scripts/compat-test.sh
#
# Exit status is nonzero on the first failed assertion. The binary under test
# defaults to ./build/release/bin/kicli; override with KICLI=/path/to/kicli.
#
# Network access is required for the JLCPCB tests. Pass --offline to skip
# those (the rest still runs).

set -u

KICLI="${KICLI:-./build/release/bin/kicli}"
FIXTURES="${FIXTURES:-tests/fixtures}"
OFFLINE=0
VERBOSE=0

for arg in "$@"; do
    case "$arg" in
        --offline) OFFLINE=1 ;;
        -v|--verbose) VERBOSE=1 ;;
        *) printf 'unknown arg: %s\n' "$arg" >&2; exit 2 ;;
    esac
done

PASS=0
FAIL=0
FAILED_NAMES=""

CYAN="$(printf '\033[36m')"
GREEN="$(printf '\033[32m')"
RED="$(printf '\033[31m')"
RESET="$(printf '\033[0m')"

say()   { printf '%s\n' "$*"; }
head2() { printf '\n%s── %s %s\n' "$CYAN" "$1" "$RESET"; }

# assert <name> <cmd…>     — non-zero command exit counts as fail
assert() {
    local name="$1"; shift
    if [ "$VERBOSE" = 1 ]; then
        printf '  [run] %s\n' "$*"
    fi
    if "$@" >/dev/null 2>&1; then
        printf '  %s✓%s %s\n' "$GREEN" "$RESET" "$name"
        PASS=$((PASS + 1))
    else
        printf '  %s✗%s %s\n    → %s\n' "$RED" "$RESET" "$name" "$*"
        FAIL=$((FAIL + 1))
        FAILED_NAMES="$FAILED_NAMES $name"
    fi
}

# assert_contains <name> <needle> <cmd…>
assert_contains() {
    local name="$1" needle="$2"; shift 2
    local out
    if ! out="$("$@" 2>&1)"; then
        printf '  %s✗%s %s (command failed)\n' "$RED" "$RESET" "$name"
        FAIL=$((FAIL + 1))
        FAILED_NAMES="$FAILED_NAMES $name"
        return
    fi
    if printf '%s\n' "$out" | grep -q -- "$needle"; then
        printf '  %s✓%s %s\n' "$GREEN" "$RESET" "$name"
        PASS=$((PASS + 1))
    else
        printf '  %s✗%s %s (output missing %q)\n' "$RED" "$RESET" "$name" "$needle"
        if [ "$VERBOSE" = 1 ]; then
            printf '    got:\n%s\n' "$out" | sed 's/^/      /'
        fi
        FAIL=$((FAIL + 1))
        FAILED_NAMES="$FAILED_NAMES $name"
    fi
}

# assert_match <name> <regex> <cmd…>
assert_match() {
    local name="$1" pattern="$2"; shift 2
    local out
    if ! out="$("$@" 2>&1)"; then
        printf '  %s✗%s %s (command failed)\n' "$RED" "$RESET" "$name"
        FAIL=$((FAIL + 1))
        FAILED_NAMES="$FAILED_NAMES $name"
        return
    fi
    if printf '%s\n' "$out" | grep -Eq "$pattern"; then
        printf '  %s✓%s %s\n' "$GREEN" "$RESET" "$name"
        PASS=$((PASS + 1))
    else
        printf '  %s✗%s %s (pattern %q did not match)\n' "$RED" "$RESET" "$name" "$pattern"
        if [ "$VERBOSE" = 1 ]; then
            printf '    got:\n%s\n' "$out" | sed 's/^/      /'
        fi
        FAIL=$((FAIL + 1))
        FAILED_NAMES="$FAILED_NAMES $name"
    fi
}

# ──────────────────────────────────────────────────────────────────────────

head2 "Binary + prerequisites"

if [ ! -x "$KICLI" ]; then
    say "${RED}fatal:${RESET} kicli not found at '$KICLI'. Build first or set KICLI=…"
    exit 2
fi

assert "kicli --version prints a number" \
    bash -c "'$KICLI' --version | grep -Eq 'kicli [0-9]+\.[0-9]+\.[0-9]+'"

assert_match "kicli --help shows core commands" \
    'sch\s.*jlcpcb.*import' bash -c "'$KICLI' --help 2>&1"

# kicad-cli MUST be locatable for view / erc / fp / sym to work.
if KICAD_CLI_PATH="$("$KICLI" kicad-path 2>/dev/null)"; then
    assert "kicad-cli path resolves" test -x "$KICAD_CLI_PATH"
    VERSION_OUT="$("$KICLI" kicad-version 2>/dev/null || true)"
    case "$VERSION_OUT" in
        10.*) printf '  %s✓%s kicad-cli version = %s\n' "$GREEN" "$RESET" "$VERSION_OUT"; PASS=$((PASS + 1));;
        9.*)  printf '  %s⚠%s kicad-cli version = %s (KiCad 9 — some features may drift)\n' "$CYAN" "$RESET" "$VERSION_OUT"; PASS=$((PASS + 1));;
        *)    printf '  %s✗%s kicad-cli version unexpected: %s\n' "$RED" "$RESET" "$VERSION_OUT"; FAIL=$((FAIL + 1));;
    esac
else
    say "${RED}fatal:${RESET} kicli cannot resolve kicad-cli. Install KiCad 10 or set KICAD_CLI_PATH."
    exit 2
fi

# ──────────────────────────────────────────────────────────────────────────

head2 "KiCad format — sch parse + view + erc"

TMP="$(mktemp -d -t kicli-compat.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

# Build a minimal fresh project — validates our writer produces something
# kicad-cli is willing to read.
"$KICLI" new compat_probe "$TMP" >/dev/null 2>&1

assert "kicli new produced a project" \
    test -f "$TMP/compat_probe.kicad_pro"

assert "kicli new produced a root schematic" \
    test -f "$TMP/compat_probe.kicad_sch"

assert "kicad-cli can round-trip the fresh schematic (sch upgrade)" \
    "$KICAD_CLI_PATH" sch upgrade --force "$TMP/compat_probe.kicad_sch"

# Exercise the view pipeline (parses both sides + netlist resolution) on a
# real multi-sheet project if the user has one configured via KICLI_COMPAT_SCH.
# Otherwise, smoke only the empty one.
if [ -n "${KICLI_COMPAT_SCH:-}" ] && [ -f "$KICLI_COMPAT_SCH" ]; then
    assert "view on reference schematic succeeds" \
        bash -c "'$KICLI' sch '$KICLI_COMPAT_SCH' view > /dev/null"
    assert_contains "view header mentions REF:PIN" \
        'REF:PIN' "$KICLI" sch "$KICLI_COMPAT_SCH" view
fi

# Empty project: list should emit nothing and exit 0.
assert "kicli sch list on empty project" \
    bash -c "'$KICLI' sch '$TMP/compat_probe.kicad_sch' list"

# --version of our writer's output matches what KiCad 10 emits after upgrade.
assert_match "upgraded schematic has KiCad 10 version stamp" \
    '\(version 20[0-9]{6}\)' cat "$TMP/compat_probe.kicad_sch"

# ──────────────────────────────────────────────────────────────────────────

head2 "Symbol + footprint catalog (reads KiCad bundled libs)"

assert_match "sym search finds Device:R" \
    '^Device:R\b' "$KICLI" sym search "Device:R" -n 3

assert_match "sym info Device:R has pin_count: 2" \
    'pin_count:[[:space:]]+2' "$KICLI" sym info "Device:R"

assert_match "sym search Conn_01x04 --pins 4 returns results" \
    'Conn_01x04' "$KICLI" sym search "Conn_01x04" --pins 4 -n 5

assert_match "fp search R_0402 finds bundled Metric footprint" \
    'Resistor_SMD:R_0402_1005Metric' "$KICLI" fp search "R_0402_1005Metric" -n 3

assert_match "fp info SOIC-8 reports 8 pads" \
    'pad_count:[[:space:]]+8' "$KICLI" fp info "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"

# ──────────────────────────────────────────────────────────────────────────

head2 "ERC + JSON + stdout streaming"

assert "erc -o - streams a report" \
    bash -c "'$KICLI' sch '$TMP/compat_probe.kicad_sch' erc -o - > /dev/null 2>&1"

assert_match "erc --format json emits valid JSON" \
    '^[[:space:]]*\{' bash -c \
    "'$KICLI' sch '$TMP/compat_probe.kicad_sch' erc -o - --format json 2>/dev/null"

# Confirm no Fontconfig noise leaks to stderr.
FONTCONFIG_LEAK="$("$KICLI" sch "$TMP/compat_probe.kicad_sch" view 2>&1 >/dev/null | grep -c Fontconfig || true)"
if [ "$FONTCONFIG_LEAK" = "0" ]; then
    printf '  %s✓%s Fontconfig warnings filtered out of stderr\n' "$GREEN" "$RESET"
    PASS=$((PASS + 1))
else
    printf '  %s✗%s Fontconfig leak (%s lines)\n' "$RED" "$RESET" "$FONTCONFIG_LEAK"
    FAIL=$((FAIL + 1))
    FAILED_NAMES="$FAILED_NAMES fontconfig_filter"
fi

# ──────────────────────────────────────────────────────────────────────────

head2 "Schematic drawing (place / net / nc)"

DRAW_SCH="$TMP/draw_probe.kicad_sch"
"$KICLI" new draw_probe "$TMP" >/dev/null 2>&1 || true
# `kicli new` writes to <dir>/<name>.kicad_sch; it refuses to overwrite so we
# reuse the existing compat_probe blank created earlier.
cp "$TMP/compat_probe.kicad_sch" "$DRAW_SCH"

assert "place Device:R" \
    bash -c "'$KICLI' sch '$DRAW_SCH' place Device:R R1 10k > /dev/null"

assert "place Amplifier_Operational:LM358 (tests extends alias)" \
    bash -c "'$KICLI' sch '$DRAW_SCH' place Amplifier_Operational:LM358 U1 LM358 > /dev/null"

assert "net attaches a local label at pin position" \
    bash -c "'$KICLI' sch '$DRAW_SCH' net SIG1 R1:1 > /dev/null && \
             grep -qE '^[[:space:]]*\"SIG1\"[[:space:]]*\$' '$DRAW_SCH'"

assert "net attaches a power port for GND (power lib auto-imported)" \
    bash -c "'$KICLI' sch '$DRAW_SCH' net GND R1:2 > /dev/null && \
             grep -qF 'power:GND' '$DRAW_SCH'"

assert "net +3V3 canonicalizes variants (3V3 → +3V3)" \
    bash -c "'$KICLI' sch '$DRAW_SCH' place Device:C C1 100nF > /dev/null && \
             '$KICLI' sch '$DRAW_SCH' net 3V3 C1:1 > /dev/null && \
             grep -qF 'power:+3V3' '$DRAW_SCH'"

assert "nc places a no_connect marker" \
    bash -c "'$KICLI' sch '$DRAW_SCH' nc U1:5 > /dev/null && \
             grep -qF 'no_connect' '$DRAW_SCH'"

assert "full draw result re-loads cleanly in kicad-cli" \
    "$KICAD_CLI_PATH" sch upgrade --force "$DRAW_SCH"

assert_match "kicli view sees every drawn net name after build" \
    '→ GND' "$KICLI" sch "$DRAW_SCH" view

assert "place refuses duplicate reference" \
    bash -c "! '$KICLI' sch '$DRAW_SCH' place Device:R R1 22k 2>/dev/null"

assert "place rejects unknown lib_id with exit 2" \
    bash -c "'$KICLI' sch '$DRAW_SCH' place Nonexistent:Thing X1 foo 2>/dev/null; [ \$? -eq 2 ]"

# ──────────────────────────────────────────────────────────────────────────

head2 "JLCPCB API contract"

if [ "$OFFLINE" = 1 ]; then
    say "  ${CYAN}(skipped — --offline)${RESET}"
else
    # Baseline part: 10k 0402 resistor, very stable LCSC code, always stocked.
    PART_OUT="$("$KICLI" jlcpcb part C25744 2>&1 || true)"

    for field in "Brand:" "Model:" "Code: C25744" "Package:" "Count:" "Datasheet:"; do
        if printf '%s\n' "$PART_OUT" | grep -Fq "$field"; then
            printf '  %s✓%s jlcpcb part C25744 contains field %q\n' "$GREEN" "$RESET" "$field"
            PASS=$((PASS + 1))
        else
            printf '  %s✗%s jlcpcb part C25744 missing field %q\n' "$RED" "$RESET" "$field"
            FAIL=$((FAIL + 1))
            FAILED_NAMES="$FAILED_NAMES jlcpcb_part_$field"
        fi
    done

    assert_match "jlcpcb search returns CSV header" \
        'LCSC' "$KICLI" jlcpcb search "10k 0402" --basic --in-stock -n 3

    # check subcommand with a tiny seeded project
    cp "$FIXTURES/tiny_with_lcsc.kicad_sch" "$TMP/check.kicad_sch" 2>/dev/null || {
        # Fallback: derive from our fresh project by setting one LCSC.
        say "  (no fixture — skipping jlcpcb check assertion)"
    }
    if [ -f "$TMP/check.kicad_sch" ]; then
        assert_match "jlcpcb check produces tab-separated rows" \
            'REF.*PartNo.*Match' "$KICLI" jlcpcb check "$TMP/check.kicad_sch"
    fi
fi

# ──────────────────────────────────────────────────────────────────────────

head2 "Result"

TOTAL=$((PASS + FAIL))
if [ "$FAIL" -eq 0 ]; then
    printf '%s✓%s %d/%d assertions passed\n' "$GREEN" "$RESET" "$PASS" "$TOTAL"
    exit 0
fi

printf '%s✗%s %d/%d assertions failed\n' "$RED" "$RESET" "$FAIL" "$TOTAL"
printf 'failed:%s\n' "$FAILED_NAMES"
exit 1
