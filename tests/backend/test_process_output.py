from pathlib import Path
import os
import signal
import subprocess
import sys
import tempfile
import time
from process_output import run_output

with tempfile.TemporaryDirectory(prefix='dcc-inherited-output-') as temporary:
    root = Path(temporary)
    launcher = root / 'launcher.py'
    launcher.write_text('import subprocess, sys\np = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"], close_fds=False)\nprint(p.pid, flush=True)\n')
    try:
        subprocess.run([sys.executable, launcher], capture_output=True, timeout=0.5, check=True)
        raise AssertionError('pipe capture unexpectedly finished before inherited handles closed')
    except subprocess.TimeoutExpired as failure:
        inherited_child = int(failure.output.strip())
        os.kill(inherited_child, signal.SIGTERM)
    start = time.monotonic()
    child = int(run_output([sys.executable, launcher], limit=5).strip())
    try:
        assert time.monotonic() - start < 10
        os.kill(child, 0)
    finally:
        os.kill(child, signal.SIGTERM)
    assert run_output([sys.executable, '-c', 'print("expected")']) == 'expected\n'
    try:
        run_output([sys.executable, '-c', 'import sys; print("out"); print("err", file=sys.stderr); sys.exit(7)'])
        raise AssertionError('missing execution failure')
    except RuntimeError as failure:
        assert 'exit 7' in str(failure) and 'out\nerr\n' in str(failure)
    print('  RESULT inherited output handles do not delay process completion; output and exit errors preserved')
