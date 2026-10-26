#!/usr/bin/env bash
# Zinc test runner
# Usage: ./run_tests.sh [<number>] [-j <jobs>] [-q] [--zinc <path>] [--tests <dir>]
#
#   <number>        only run tests whose name starts with "<number>_"
#   -j, --jobs N    run N tests in parallel (default: number of CPUs)
#   -q, --quiet     hide the per-test grid, show only failures and the summary
#   --zinc PATH     compiler to test (default: $ZINC or ./zinc)
#   --tests DIR     test root (default: $TESTS_DIR or ./tests)
#
# Exit codes:
#   0 - all pass/fail tests succeeded
#   1 - one or more pass/fail tests failed

set -uo pipefail

EXE_EXT=""
case "${OSTYPE:-}" in msys*|cygwin*|win32*) EXE_EXT=".exe" ;; esac
# $OSTYPE is unreliable under the msys2 {0} / MINGW64 CI shell, so also probe
# uname. The compiler force-appends .exe on Windows regardless.
case "$(uname -s 2>/dev/null)" in MINGW*|MSYS*|CYGWIN*) EXE_EXT=".exe" ;; esac

# ─────────────────────────── per-test worker ───────────────────────────
#
# Each test runs in its own process (spawned by xargs -P) and reports through
# two files in $RES_DIR: <idx>.status ("STATUS<TAB>detail") and <idx>.log
# (extra lines shown under a failure). The status file is written last via
# mv, so the progress bar only ever counts finished tests.

# Resolve the binary the compiler actually produced. On Windows the compiler
# force-appends ".exe" to the -o name (see zgen.c), so the file on disk may not
# match the path we asked for. Echo the real path if it exists, else nothing.
resolve_binary() {
    local bin="$1"
    if [ -f "$bin" ] && [ -x "$bin" ]; then
        printf '%s' "$bin"
    elif [ -f "$bin.exe" ] && [ -x "$bin.exe" ]; then
        printf '%s' "$bin.exe"
    fi
}

ESC=$(printf '\033')

# Compiler output without colors and progress chatter, for the failure report.
clean_log() {
    sed -e "s/${ESC}\[[0-9;]*m//g" "$1" |
        grep -v -E '^[[:space:]]*(Building|Generated|Total:)' |
        head -n 25
}

describe_exit() {
    local code="$1"
    if [ "$code" -ge 128 ]; then
        echo "crashed (signal $((code - 128)))"
    else
        echo "exited with code $code"
    fi
}

# Run the produced binary; sets ACTUAL and RUN_EXIT.
run_binary() {
    RUN_EXIT=0
    ACTUAL=$("$1" 2>/dev/null) || RUN_EXIT=$?
}

# Writes a unified diff of expected vs actual output to the log.
log_diff() {
    local exp_file="$1" act_file="$OUT.actual"
    printf '%s\n' "$ACTUAL" > "$act_file"
    diff -u "$exp_file" "$act_file" | tail -n +3 | head -n 25 > "$LOG"
}

run_pass() {
    local expected="${SRC%.zn}.expected"
    if [ -z "$REALBIN" ]; then
        STATUS=FAIL
        if [ "$COMPILE_EXIT" -ge 128 ]; then
            DETAIL="compiler $(describe_exit "$COMPILE_EXIT")"
        else
            DETAIL="compile error"
        fi
        clean_log "$OUT.compile" > "$LOG"
        return
    fi

    run_binary "$REALBIN"
    if [ "$RUN_EXIT" -ne 0 ]; then
        STATUS=FAIL; DETAIL="binary $(describe_exit "$RUN_EXIT")"
        return
    fi

    if [ ! -f "$expected" ]; then
        STATUS=PASS; DETAIL="no .expected file"
    elif [ "$ACTUAL" = "$(cat "$expected")" ]; then
        STATUS=PASS
    else
        STATUS=FAIL; DETAIL="output mismatch"
        log_diff "$expected"
    fi
}

run_fail() {
    if [ -n "$REALBIN" ]; then
        STATUS=FAIL; DETAIL="compiled, but should have been rejected"
    elif [ "$COMPILE_EXIT" -ge 128 ]; then
        STATUS=FAIL; DETAIL="compiler $(describe_exit "$COMPILE_EXIT") - must report an error"
        clean_log "$OUT.compile" > "$LOG"
    else
        STATUS=PASS; DETAIL="rejected"
    fi
}

run_xfail() {
    local expected="${SRC%.zn}.expected"
    if [ -z "$REALBIN" ]; then
        STATUS=XFAIL; DETAIL="rejected by compiler"
        return
    fi

    run_binary "$REALBIN"
    if [ ! -f "$expected" ] ||
       { [ "$ACTUAL" = "$(cat "$expected")" ] && [ "$RUN_EXIT" -eq 0 ]; }; then
        STATUS=XPASS; DETAIL="passes now - move it to pass/"
    else
        STATUS=XFAIL; DETAIL="compiles, wrong output"
    fi
}

# worker <idx> <kind> <src> <name>
worker() {
    local idx="$1" kind="$2"
    SRC="$3"
    OUT="$RES_DIR/$idx"
    LOG="$OUT.log"
    STATUS="" DETAIL=""
    : > "$LOG"

    local bin="$TMP_DIR/bin/${kind}_${4}${EXE_EXT}"
    COMPILE_EXIT=0
    "$ZINC" build "$SRC" -o "$bin" > "$OUT.compile" 2>&1 || COMPILE_EXIT=$?
    REALBIN=$(resolve_binary "$bin")

    case "$kind" in
        pass)  run_pass  ;;
        fail)  run_fail  ;;
        xfail) run_xfail ;;
    esac

    printf '%s\t%s\n' "$STATUS" "$DETAIL" > "$OUT.tmp"
    mv "$OUT.tmp" "$OUT.status"
}

if [ "${1:-}" = "--worker" ]; then
    shift
    worker "$@"
    exit 0
fi

# ─────────────────────────────── driver ────────────────────────────────

ZINC="${ZINC:-./zinc}"
TESTS_DIR="${TESTS_DIR:-./tests}"
TEST_FILTER=""
QUIET=0
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)

while [ $# -gt 0 ]; do
    case "$1" in
        -j|--jobs)  JOBS="$2"; shift ;;
        -j*)        JOBS="${1#-j}" ;;
        -q|--quiet) QUIET=1 ;;
        --zinc)     ZINC="$2"; shift ;;
        --tests)    TESTS_DIR="$2"; shift ;;
        -h|--help)  sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)
            if [[ "$1" =~ ^[0-9]+$ ]]; then
                TEST_FILTER="$1"
            else
                echo "unknown argument: $1" >&2; exit 2
            fi ;;
    esac
    shift
done

# Colors and the live progress bar only on a terminal; CI logs stay plain.
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    TTY=1
    GREEN="${ESC}[0;32m" RED="${ESC}[0;31m" YELLOW="${ESC}[0;33m"
    CYAN="${ESC}[0;36m"  DIM="${ESC}[2m"    BOLD="${ESC}[1m" NC="${ESC}[0m"
    TERM_COLS=$(tput cols 2>/dev/null || echo 100)
else
    TTY=0
    GREEN="" RED="" YELLOW="" CYAN="" DIM="" BOLD="" NC=""
    TERM_COLS=${COLUMNS:-100}
fi

if [ ! -x "$ZINC" ]; then
    echo "${RED}error:${NC} zinc binary not found at '$ZINC'"
    echo "       Run 'make' first, or set ZINC=/path/to/zinc"
    exit 1
fi

# Create the scratch dir under the current working directory rather than the
# MSYS /tmp mount. zinc is a native Windows binary and does not do MSYS/Cygwin
# POSIX path translation, so a "/tmp/..." path from mktemp would be resolved by
# zinc against the current drive root and land somewhere bash isn't looking.
# A dir next to us is resolved identically by both.
TMP_DIR=$(mktemp -d "./.zinc-test.XXXXXX")
RES_DIR="$TMP_DIR/results"
mkdir -p "$RES_DIR" "$TMP_DIR/bin"
trap 'rm -rf "$TMP_DIR"' EXIT

install_stdlib() {
    [ -d std ] || return 0
    local home_dir="$HOME"
    case "${OSTYPE:-}" in
        msys*|cygwin*)
            [ -n "${USERPROFILE:-}" ] && home_dir=$(cygpath -u "$USERPROFILE")
            ;;
    esac
    local registry="$home_dir/.zinc/packages/std"
    rm -rf "$registry"
    mkdir -p "$registry"
    cp -R std/. "$registry/"
    echo "  ${DIM}installed stdlib -> $registry${NC}"
}

# ── collect: one line per test, "<idx> <kind> <src> <name>" ──
LIST="$TMP_DIR/list"
: > "$LIST"
count=0

add_test() {
    local kind="$1" src="$2" name="$3"
    [ -n "$TEST_FILTER" ] && [[ "$name" != ${TEST_FILTER}_* ]] && return
    count=$((count + 1))
    printf '%04d %s %s %s\n' "$count" "$kind" "$src" "$name" >> "$LIST"
}

for kind in pass fail xfail; do
    dir="$TESTS_DIR/$kind"
    [ -d "$dir" ] || continue
    for f in "$dir"/*.zn; do
        [ -f "$f" ] && add_test "$kind" "$f" "$(basename "$f" .zn)"
    done
    # Directory-based tests: <name>/test.zn (entry) + <name>/test.expected,
    # with sibling .zn files acting as importable modules.
    for f in "$dir"/*/test.zn; do
        [ -f "$f" ] && add_test "$kind" "$f" "$(basename "$(dirname "$f")")"
    done
done

install_stdlib
echo "${BOLD}Zinc test suite${NC}  ${DIM}zinc: $ZINC · $count tests · $JOBS jobs${NC}"
[ -n "$TEST_FILTER" ] && echo "  filter: tests starting with ${BOLD}${TEST_FILTER}_${NC}"

if [ "$count" -eq 0 ]; then
    echo "  no tests matched"
    exit 0
fi

# ── run ──
export ZINC TMP_DIR RES_DIR EXE_EXT
xargs -P "$JOBS" -n 4 bash "$0" --worker < "$LIST" &
XARGS_PID=$!

draw_progress() {
    local done="$1" width=30
    local filled=$((done * width / count))
    local bar empty nfail
    bar=$(printf '%*s' "$filled" '' | tr ' ' '#')
    empty=$(printf '%*s' "$((width - filled))" '' | tr ' ' '.')
    nfail=$(cat "$RES_DIR"/*.status 2>/dev/null | grep -c '^FAIL' || true)
    printf '\r  [%s%s] %d/%d' "$bar" "$empty" "$done" "$count"
    [ "$nfail" -gt 0 ] && printf '  %s%d failing%s' "$RED" "$nfail" "$NC"
}

if [ "$TTY" -eq 1 ]; then
    while kill -0 "$XARGS_PID" 2>/dev/null; do
        draw_progress "$(ls "$RES_DIR" | grep -c '\.status$' || true)"
        sleep 0.2
    done
    printf '\r%s[K' "$ESC"
fi
wait "$XARGS_PID"

# ── gather results (in collection order) ──
passed=0 failed=0 xfailed=0 xpassed=0
# Per-suite counters: total pass fail xfail xpass
for kind in pass fail xfail; do
    eval "s_${kind}_total=0 s_${kind}_PASS=0 s_${kind}_FAIL=0 s_${kind}_XFAIL=0 s_${kind}_XPASS=0"
done

while read -r idx kind src name; do
    status_file="$RES_DIR/$idx.status"
    if [ -f "$status_file" ]; then
        IFS=$'\t' read -r st _ < "$status_file"
    else
        st=FAIL
        printf 'FAIL\ttest runner died before reporting\n' > "$status_file"
    fi
    eval "s_${kind}_total=\$((s_${kind}_total + 1)); s_${kind}_${st}=\$((s_${kind}_${st} + 1))"
    case "$st" in
        PASS)  passed=$((passed + 1)) ;;
        FAIL)  failed=$((failed + 1)) ;;
        XFAIL) xfailed=$((xfailed + 1)) ;;
        XPASS) xpassed=$((xpassed + 1)) ;;
    esac
done < "$LIST"

mark() {
    case "$1" in
        PASS)  printf '%s✓%s' "$GREEN"  "$NC" ;;
        FAIL)  printf '%s✗%s' "$RED"    "$NC" ;;
        XFAIL) printf '%s~%s' "$YELLOW" "$NC" ;;
        XPASS) printf '%s↑%s' "$CYAN"   "$NC" ;;
    esac
}

# Grid of every test in a suite, filled column by column like `ls`.
render_grid() {
    local kind="$1" title="$2"
    local names=() statuses=() n=0 maxlen=0 idx k src name st
    while read -r idx k src name; do
        [ "$k" = "$kind" ] || continue
        IFS=$'\t' read -r st _ < "$RES_DIR/$idx.status"
        names[n]="$name"; statuses[n]="$st"; n=$((n + 1))
        [ "${#name}" -gt "$maxlen" ] && maxlen=${#name}
    done < "$LIST"
    [ "$n" -eq 0 ] && return

    local colw=$((maxlen + 4))
    local cols=$(( (TERM_COLS - 2) / colw ))
    [ "$cols" -lt 1 ] && cols=1
    local rows=$(( (n + cols - 1) / cols ))

    echo ""
    echo "${BOLD}${title}${NC}"
    local r c i
    for ((r = 0; r < rows; r++)); do
        printf '  '
        for ((c = 0; c < cols; c++)); do
            i=$((c * rows + r))
            [ "$i" -ge "$n" ] && continue
            mark "${statuses[i]}"
            printf ' %-*s' "$((colw - 2))" "${names[i]}"
        done
        echo ""
    done
}

if [ "$QUIET" -eq 0 ]; then
    render_grid pass  "Pass tests"
    render_grid fail  "Fail tests ${DIM}(compiler must reject these)${NC}"
    render_grid xfail "Expected failures ${DIM}(unimplemented features)${NC}"
fi

# ── failures and promotions, with their logs ──
if [ "$failed" -gt 0 ] || [ "$xpassed" -gt 0 ]; then
    echo ""
    echo "${BOLD}Needs attention${NC}"
    while read -r idx kind src name; do
        IFS=$'\t' read -r st detail < "$RES_DIR/$idx.status"
        [ "$st" = FAIL ] || [ "$st" = XPASS ] || continue
        echo ""
        echo "  $(mark "$st") ${BOLD}$kind/$name${NC}  ${DIM}—${NC} $detail"
        if [ -s "$RES_DIR/$idx.log" ]; then
            sed "s/^/    ${DIM}│${NC} /" "$RES_DIR/$idx.log"
        fi
    done < "$LIST"
fi

# ── summary table ──
cell() {  # cell <value> <color-if-nonzero>
    # "·" is multi-byte, so pad it by hand: printf widths count bytes.
    if [ "$1" -eq 0 ]; then printf '    %s·%s' "$DIM" "$NC"
    else printf '%s%5d%s' "$2" "$1" "$NC"
    fi
}

row() {  # row <label> <total> <pass> <fail> <xfail> <xpass>
    printf '  │ %-6s │ %5d │ %s │ %s │ %s │ %s │\n' "$1" "$2" \
        "$(cell "$3" "$GREEN")" "$(cell "$4" "$RED")" \
        "$(cell "$5" "$YELLOW")" "$(cell "$6" "$CYAN")"
}

echo ""
echo "  ┌────────┬───────┬───────┬───────┬───────┬───────┐"
echo "  │ ${BOLD}suite ${NC} │ ${BOLD}total${NC} │ ${BOLD} pass${NC} │ ${BOLD} fail${NC} │ ${BOLD}xfail${NC} │ ${BOLD}xpass${NC} │"
echo "  ├────────┼───────┼───────┼───────┼───────┼───────┤"
for kind in pass fail xfail; do
    eval "t=\$s_${kind}_total p=\$s_${kind}_PASS f=\$s_${kind}_FAIL xf=\$s_${kind}_XFAIL xp=\$s_${kind}_XPASS"
    [ "$t" -eq 0 ] && continue
    row "$kind" "$t" "$p" "$f" "$xf" "$xp"
done
echo "  ├────────┼───────┼───────┼───────┼───────┼───────┤"
row "total" "$count" "$passed" "$failed" "$xfailed" "$xpassed"
echo "  └────────┴───────┴───────┴───────┴───────┴───────┘"

if [ "$failed" -eq 0 ]; then
    verdict="${GREEN}${BOLD}all tests passed${NC}"
else
    verdict="${RED}${BOLD}$failed failed${NC}"
fi
echo "  $verdict  ${DIM}in ${SECONDS}s${NC}"
[ "$xpassed" -gt 0 ] && echo "  ${CYAN}$xpassed xfail test(s) now pass - promote them to pass/${NC}"

[ "$failed" -eq 0 ]
