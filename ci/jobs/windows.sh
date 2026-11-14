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
python3 tests/lsp_stdio.py build-windows/bin/dccd.exe --wine "${WINE:-wine}"

ci_section hosted-pe-link
python3 tests/backend/test_coff_executable.py build/bin/dcc --import-libs "${MINGW_SYSROOT:-/opt/llvm-mingw}/x86_64-w64-mingw32/lib"
