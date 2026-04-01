#!/usr/bin/env bash
# Zinc test runner
# Usage: ./run_tests.sh [--zinc <path>] [--tests <dir>]
#
# Exit codes:
#   0 - all pass/fail tests succeeded
#   1 - one or more pass/fail tests failed

set -uo pipefail

ZINC="${ZINC:-./zinc}"
TESTS_DIR="${TESTS_DIR:-./tests}"
PASS_DIR="$TESTS_DIR/pass"
FAIL_DIR="$TESTS_DIR/fail"
XFAIL_DIR="$TESTS_DIR/xfail"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

passed=0
failed=0
xfailed=0
xpassed=0

TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

# ── helpers ──────────────────────────────────────────────────────────────────

binary_was_produced() {
    local bin="$1"
    [ -f "$bin" ] && [ -x "$bin" ]
}

compile() {
    local src="$1" bin="$2" log="$3"
    # Redirect all compiler noise (including crash messages) to the log file.
    # zinc always exits 0 on normal errors, but can segfault on bad input.
    { "$ZINC" "$src" -o "$bin" > "$log" 2>&1; } 2>/dev/null || true
}

# ── pass tests ───────────────────────────────────────────────────────────────
# A pass test must:
#   1. Produce an executable
#   2. Run without crashing (exit 0)
#   3. Produce output matching <test>.expected (if the file exists)

run_pass_test() {
    local src="$1"
    local name
    name=$(basename "$src" .zn)
    local expected="${src%.zn}.expected"
    local bin="$TMP_DIR/$name"
    local compile_log="$TMP_DIR/${name}.compile.log"

    compile "$src" "$bin" "$compile_log"

    if ! binary_was_produced "$bin"; then
        echo -e "  ${RED}FAIL${NC}  $name"
        echo -e "       compilation did not produce a binary"
        if grep -q "error:" "$compile_log" 2>/dev/null; then
            grep "error:" "$compile_log" | head -3 | sed 's/^/       /'
        fi
        failed=$((failed + 1))
        return
    fi

    # Run the binary
    local actual exit_code
    actual=$("$bin" 2>/dev/null) || exit_code=$?
    exit_code=${exit_code:-0}

    if [ "$exit_code" -ne 0 ]; then
        echo -e "  ${RED}FAIL${NC}  $name"
        echo -e "       binary exited with code $exit_code"
        failed=$((failed + 1))
        return
    fi

    # Compare output only when an .expected file is present
    if [ -f "$expected" ]; then
        local exp
        exp=$(cat "$expected")
        if [ "$actual" = "$exp" ]; then
            echo -e "  ${GREEN}PASS${NC}  $name"
            passed=$((passed + 1))
        else
            echo -e "  ${RED}FAIL${NC}  $name  (output mismatch)"
            echo -e "       expected: $(echo "$exp"    | head -1 | cat -v)"
            echo -e "       got:      $(echo "$actual" | head -1 | cat -v)"
            failed=$((failed + 1))
        fi
    else
        echo -e "  ${GREEN}PASS${NC}  $name  (no output check)"
        passed=$((passed + 1))
    fi
}

# ── fail tests ───────────────────────────────────────────────────────────────
# A fail test must NOT produce an executable.

run_fail_test() {
    local src="$1"
    local name
    name=$(basename "$src" .zn)
    local bin="$TMP_DIR/$name"
    local compile_log="$TMP_DIR/${name}.compile.log"

    compile "$src" "$bin" "$compile_log"

    if binary_was_produced "$bin"; then
        echo -e "  ${RED}FAIL${NC}  $name  (should have been rejected, but compiled)"
        failed=$((failed + 1))
    else
        echo -e "  ${GREEN}PASS${NC}  $name  (correctly rejected)"
        passed=$((passed + 1))
    fi
}

# ── xfail tests ──────────────────────────────────────────────────────────────
# Expected to fail for now (unimplemented features).
# Counts as xfailed if it fails, xpassed (bonus) if it succeeds.

run_xfail_test() {
    local src="$1"
    local name
    name=$(basename "$src" .zn)
    local expected="${src%.zn}.expected"
    local bin="$TMP_DIR/$name"
    local compile_log="$TMP_DIR/${name}.compile.log"

    compile "$src" "$bin" "$compile_log"

    if ! binary_was_produced "$bin"; then
        echo -e "  ${YELLOW}XFAIL${NC} $name  (expected — compiler rejected)"
        xfailed=$((xfailed + 1))
        return
    fi

    # Binary was produced — check output if an .expected file exists
    local actual exit_code
    actual=$("$bin" 2>/dev/null) || exit_code=$?
    exit_code=${exit_code:-0}

    if [ -f "$expected" ]; then
        local exp
        exp=$(cat "$expected")
        if [ "$actual" = "$exp" ] && [ "$exit_code" -eq 0 ]; then
            echo -e "  ${CYAN}XPASS${NC} $name  (unexpectedly passed — promote to pass/)"
            xpassed=$((xpassed + 1))
        else
            echo -e "  ${YELLOW}XFAIL${NC} $name  (expected — compiles but output wrong)"
            xfailed=$((xfailed + 1))
        fi
    else
        # No expected file and binary exists: treat as xpass
        echo -e "  ${CYAN}XPASS${NC} $name  (unexpectedly passed — promote to pass/)"
        xpassed=$((xpassed + 1))
    fi
}

# ── main ─────────────────────────────────────────────────────────────────────

# Check zinc binary
if [ ! -x "$ZINC" ]; then
    echo -e "${RED}error:${NC} zinc binary not found at '$ZINC'"
    echo "       Run 'make' first, or set ZINC=/path/to/zinc"
    exit 1
fi

echo -e "${BOLD}Zinc test suite${NC}  (zinc: $ZINC)"
echo ""

if [ -d "$PASS_DIR" ] && compgen -G "$PASS_DIR/*.zn" > /dev/null 2>&1; then
    echo -e "${BOLD}Pass tests${NC}"
    for f in "$PASS_DIR"/*.zn; do
        run_pass_test "$f"
    done
    echo ""
fi

if [ -d "$FAIL_DIR" ] && compgen -G "$FAIL_DIR/*.zn" > /dev/null 2>&1; then
    echo -e "${BOLD}Fail tests${NC}  (compiler must reject these)"
    for f in "$FAIL_DIR"/*.zn; do
        run_fail_test "$f"
    done
    echo ""
fi

if [ -d "$XFAIL_DIR" ] && compgen -G "$XFAIL_DIR/*.zn" > /dev/null 2>&1; then
    echo -e "${BOLD}Expected failures${NC}  (unimplemented features)"
    for f in "$XFAIL_DIR"/*.zn; do
        run_xfail_test "$f"
    done
    echo ""
fi

total=$((passed + failed))
echo "────────────────────────────────"
echo -e "${BOLD}Results${NC}"
printf "  %-10s %d / %d\n" "Passed:"  "$passed"  "$total"
printf "  %-10s %d\n"      "Failed:"  "$failed"
printf "  %-10s %d\n"      "XFailed:" "$xfailed"
[ "$xpassed" -gt 0 ] && printf "  %-10s %d  ← promote to pass tests!\n" "XPassed:" "$xpassed"
echo "────────────────────────────────"

[ "$failed" -eq 0 ]
