#!/usr/bin/env bash
source ci/lib/container.sh
ci_section build
ci_make all
ci_section differential
ci_run_logged differential ci_make differential
grep -qx 'ALL-OK' "$LOG_DIR/differential.log"
