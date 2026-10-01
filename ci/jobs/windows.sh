#!/usr/bin/env bash
source ci/lib/container.sh
export WINEDEBUG=-all WINEPREFIX="$HOME/wine"
ci_section build
ci_make all
ci_section test-win
ci_run_logged test-win ci_make test-win
ci_section tools-windows
make -j"$JOBS" CROSS=windows ENABLE_LLVM=1 BUILD_TYPE=release tools-windows
ci/lib/check-windows-dcc.sh build-windows/bin/dcc.exe
