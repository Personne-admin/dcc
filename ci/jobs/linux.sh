#!/usr/bin/env bash
source ci/lib/container.sh
ci_section build
ci_make all
ci_section test-linux
ci_run_logged test-linux ci_make test-linux
