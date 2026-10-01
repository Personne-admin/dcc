#!/usr/bin/env bash
source ci/lib/container.sh
ci_section build
ci_make driver dccd dcdoc
ci_section check
ci/lib/check-static.sh build/bin/dcc build/bin/dccd build/bin/dcdoc
