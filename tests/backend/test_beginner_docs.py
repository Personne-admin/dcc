from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
compiler = Path(sys.argv[1]).resolve()
examples = re.findall(r'```dc\n(.*?)```', (root / 'docs/getting-started.md').read_text(), re.S)
assert len(examples) == 2
outputs = ['Hello, world!\n', 'Hello, DC!\nNumber: 10\nNumber: 20\n']
checks = 0
with tempfile.TemporaryDirectory(prefix='dcc-beginner-docs-') as directory:
    source, executable = Path(directory) / 'main.dc', Path(directory) / 'program'
    for example, output in zip(examples, outputs):
        source.write_text(example)
        for backend in ('llvm', 'custom'):
            for opt in ('-O0', '-O2'):
                for mode in ([], ['-fwhole-program']):
                    result = subprocess.run(['timeout', '60', str(compiler), '-flibdcext', 'linux', '-fbackend', backend, opt,
                                             *mode, str(source), '-o', str(executable)], capture_output=True, text=True, timeout=65)
                    assert result.returncode == 0, result.stderr
                    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
                    assert (result.returncode, result.stdout, result.stderr) == (0, output, ''), result
                    checks += 1
print(f'  RESULT {checks}/{checks} published beginner example executions passed')
