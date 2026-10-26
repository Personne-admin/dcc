import subprocess
import tempfile


def run_output(command, cwd=None, limit=65):
    with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
        result = subprocess.run(list(map(str, command)), cwd=cwd, stdout=stdout, stderr=stderr, timeout=limit)
        stdout.seek(0)
        stderr.seek(0)
        out, err = stdout.read().decode(), stderr.read().decode()
    if result.returncode:
        raise RuntimeError('%s\nexit %s\n%s%s' % (command, result.returncode, out, err))
    return out
