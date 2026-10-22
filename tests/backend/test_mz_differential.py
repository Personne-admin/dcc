from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
compiler = Path(sys.argv[1]).resolve()
runner = ROOT / "i8086" / "run_dos.py"
startup = (ROOT / "i8086" / "dos_start.dc").read_text()
libdcext = ROOT.parent / "libdcext"
checks = 0
failures = []
excluded = []


def parse_fixture(path):
    text = path.read_text()
    sections = re.split(r"^=== (.+?) ===$", text, flags=re.M)
    files = {}
    entry = None
    exit_code = None
    for i in range(1, len(sections) - 1, 2):
        header = sections[i].strip()
        body = sections[i + 1]
        if header.startswith("FILE:"):
            files[header.split(":", 1)[1].strip()] = body
        elif header.startswith("ENTRY:"):
            entry = header.split(":", 1)[1].strip()
        elif header.startswith("EXPECT-EM64T-OBJECT"):
            for line in body.splitlines():
                if line.strip().startswith("RUN-EXIT:"):
                    exit_code = int(line.strip().split(":", 1)[1].strip())
    if not files or entry is None:
        entry = next(iter(files)) if files else None
    return files, entry, exit_code


def compile_image(directory, name, files, entry, opt):
    for filename, content in files.items():
        path = directory / filename
        path.parent.mkdir(parents=True, exist_ok=True)
        if filename == entry:
            content = content + "\n" + startup
        path.write_text(content)
    output = directory / f"{name}{opt}.exe"
    args = [compiler, "-target", "i8086-mz", "-fbackend", "custom", "-mcmodel=small", "-e", "_start",
            "-fstack-reserve=16384", opt, f"-I{libdcext}", directory / entry, "-o", output]
    result = subprocess.run(["timeout", "60", *map(str, args)], capture_output=True, text=True)
    if result.returncode != 0:
        return None, result.stderr.strip().splitlines()[-1:] or ["compile failed"]
    return output, None


with tempfile.TemporaryDirectory(prefix="dcc-mz-diff-") as temporary:
    temporary = Path(temporary)
    entries = []
    for line in (ROOT / "i8086" / "differential.list").read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            entries.append(line)
    for entry_name in entries:
        path = ROOT / "cases" / entry_name
        files, entry, expected = parse_fixture(path)
        if expected is None:
            failures.append((entry_name, "no RUN-EXIT"))
            continue
        for opt in ("-O0", "-O2"):
            work = temporary / Path(entry_name).parent / (Path(entry_name).stem + opt)
            work.mkdir(parents=True, exist_ok=True)
            image, error = compile_image(work, Path(entry_name).stem, files, entry, opt)
            if image is None:
                reason = error[-1] if error else "compile"
                if "requests alignment" in reason:
                    excluded.append((f"{entry_name} {opt}", reason))
                    continue
                failures.append((f"{entry_name} {opt}", reason))
                continue
            out_file = work / "stdout.bin"
            err_file = work / "stderr.bin"
            result = subprocess.run([sys.executable, str(runner), "--format", "mz", "--paragraph", "0x2000",
                                     "--stdout-out", str(out_file), "--stderr-out", str(err_file), str(image)],
                                    capture_output=True, text=True)
            if result.returncode != expected:
                failures.append((f"{entry_name} {opt}", f"exit {result.returncode}, expected {expected}"))
                continue
            checks += 1

print(f"  RESULT  i8086 MZ differential: {checks}/{checks + len(failures)} passed, {len(excluded)} excluded")
for name, reason in excluded:
    print(f"    EXCLUDED  {name}: {reason}")
for name, reason in failures:
    print(f"    FAIL  {name}: {reason}")
if failures:
    raise SystemExit(1)
