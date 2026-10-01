set -euo pipefail

export CXX=clang++ CC=clang AR=ar
JOBS="${CI_JOBS:-$(nproc)}"
DCC_TIMEOUT="${DCC_TIMEOUT:-60}"
TOPLEVEL="$(pwd)"
LOG_DIR="${DCC_CI_OUT:-/tmp}/logs"
mkdir -p "$LOG_DIR"

printf '#!/bin/sh\nexec timeout %s %s/build/bin/dcc "$@"\n' "$DCC_TIMEOUT" "$TOPLEVEL" > /tmp/dcc-bounded
chmod +x /tmp/dcc-bounded

MAKE_ARGS=(CXX=clang++ CC=clang AR=ar BUILD_TYPE=release STATIC_LINK=1 DCC=/tmp/dcc-bounded)

ci_make() {
    make -j"$JOBS" "${MAKE_ARGS[@]}" "$@"
}

ci_section() {
    echo "--- $* ($(date -u +%H:%M:%S))"
}

ci_check_log() {
    local log="$1"
    if grep -nE '^\s*(FAIL|SKIP)\s' "$log"; then
        echo "error: failing or skipped tests in $log" >&2
        return 1
    fi
    if grep -nE 'WARN\s+(llvm-as|nasm) not available' "$log"; then
        echo "error: verification tool missing in $log" >&2
        return 1
    fi
}

ci_run_logged() {
    local name="$1"; shift
    local log="$LOG_DIR/$name.log"
    set +e
    "$@" 2>&1 | tee "$log"
    local rc=${PIPESTATUS[0]}
    set -e
    [[ $rc -eq 0 ]] || { echo "error: $name exited with $rc" >&2; return "$rc"; }
    ci_check_log "$log"
}
