#!/usr/bin/env bash
source ci/lib/container.sh
ci_section build
ci_make all
ci_section test
ci_run_logged test ci_make test
grep -qE '^\s*PASS\s+([0-9]+)/\1 test suites' "$LOG_DIR/test.log"
