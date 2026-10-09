#!/usr/bin/env bash
source ci/lib/container.sh
ci_section actionlint
actionlint -color .github/workflows/*.yml
ci_section shell
for f in ci/run.sh ci/toolchain/build.sh ci/lib/*.sh ci/jobs/*.sh ci/release/*.sh; do
    bash -n "$f"
done
ci_section python
python3 -m py_compile ci/release/*.py
ci_section release-tools
python3 ci/release/version.py selftest
python3 ci/release/test_notes.py
