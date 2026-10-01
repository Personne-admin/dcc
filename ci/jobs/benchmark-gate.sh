#!/usr/bin/env bash
source ci/lib/container.sh
ci_section test-benchmark
ci_run_logged test-benchmark ci_make test-benchmark
