from pathlib import Path
import re
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
checks = 0

ANSI = re.compile(r'\x1b\[[0-9;]*m')

COMMON = '''module common;
import core;

public [] const char repeat(char c, i32 count) {
    Array(char) text = {};
    for i32 i = 0; i < count; i++ {
        if !text.push(c).is_ok() { return "push failed"; }
    }
    return text.items();
}

public void fail(i32 n) {
    core::compile_error(repeat('x', n));
}

public void warn(i32 n) {
    core::compile_warning(repeat('w', n));
}

public void relay(i32 n) {
    fail(n + 1);
}

public i32 limited(i32 n) {
    if n > 4 { fail(n); }
    return n;
}
'''


def build(source, *flags):
    directory = Path(tempfile.mkdtemp(prefix='dcc-compile-time-actions-'))
    (directory / 'common.dc').write_text(COMMON)
    (directory / 'main.dc').write_text(source)
    exe = directory / 'program'
    result = subprocess.run(['timeout', '60', str(compiler), '-flibdcext', '-I', str(directory), *flags, str(directory / 'main.dc'), '-o', str(exe)],
                            capture_output=True, text=True, timeout=65)
    stderr = ANSI.sub('', result.stderr)
    diagnostics = []
    lines = stderr.splitlines()
    for i, line in enumerate(lines):
        match = re.match(r'(error|warning|note): (.*)', line)
        if not match:
            continue
        location = re.search(r'main\.dc:(\d+):', lines[i + 1]) if i + 1 < len(lines) else None
        diagnostics.append((match.group(1), match.group(2), int(location.group(1)) if location else None))
    return result.returncode, diagnostics, stderr, exe


def expect(source, code, diagnostics, *flags):
    global checks
    actual_code, actual, stderr, exe = build(source, *flags)
    assert actual_code == code and actual == diagnostics, (actual_code, actual, stderr)
    checks += 1
    return exe


expect('module main;\nimport common;\npublic i32 main() { common::fail(3); return 0; }\n', 1, [('error', 'xxx', 3)])
expect('module main;\nimport common;\npublic i32 main() { common::relay(4); return 0; }\n', 1, [('error', 'xxxxx', 3)])
expect('''module main;
import common;
void need_small(T)(T value) {
    static if sizeof(T) <= 2 {
        return;
    } else {
        common::fail(sizeof(T) as i32);
    }
}
public i32 main() { need_small(1 as u8); need_small(1 as u64); return 0; }
''', 1, [('error', 'xxxxxxxx', 7)])
expect('''module main;
import common;
bool checked(i32 n) {
    if n > 4 { common::fail(n); }
    return true;
}
void use(T)(T value) {
    static if checked(sizeof(T) as i32) {
        return;
    }
}
public i32 main() { use(1 as u64); return 0; }
''', 1, [('error', 'xxxxxxxx', 8)])
expect('''module main;
import common;
const i32 OK = common::limited(2);
const i32 BAD = common::limited(6);
public i32 main() { return OK; }
''', 1, [('error', 'xxxxxx', 4)])
expect('module main;\nimport common;\npublic i32 main([] const char* args) { common::fail(args.len as i32); return 0; }\n', 1,
       [('error', '`main` must be generated as run-time code, but its call to `fail` can only be evaluated at compile time and its arguments are not compile-time constants', 3)])
expect('''module main;
import core;
import common;
public i32 main([] const char* args) { core::compile_warning(common::repeat('a', args.len as i32)); return 0; }
''', 1, [('error', '`main` must be generated as run-time code, but this compile-time action has a computed message', 4)])
expect('''module main;
import core;
import common;
void probe(T)(T value) {
    static if compiles (T t) { common::fail(2); } {
        core::compile_error("probe should fail");
    } else {
        core::compile_note("probe failed as expected");
    }
}
public i32 main() { probe(1 as i32); return 0; }
''', 0, [('note', 'probe failed as expected', 8)])

code, diagnostics, stderr, _ = build('module main;\nimport common;\npublic i32 main() { void(*)(i32) f = common::fail; return 0; }\n')
assert code == 1 and '`fail` runs only at compile time and has no run-time address' in stderr, stderr
checks += 1

for backend in ('llvm', 'custom'):
    for opt in ('-O0', '-O2'):
        for whole in ([], ['-fwhole-program']):
            exe = expect('''module main;
import common;
public i32 main() { common::warn(2); return common::limited(3) + 4; }
''', 0, [('warning', 'ww', 3)], '-fbackend', backend, opt, *whole)
            run = subprocess.run([str(exe)], capture_output=True, timeout=30)
            assert run.returncode == 7, (backend, opt, whole, run.returncode)
            checks += 1
            symbols = subprocess.run(['nm', str(exe)], capture_output=True, text=True).stdout
            emitted = [line for line in symbols.splitlines() if 'common' in line and re.search(r'(fail|warn|relay)', line)]
            assert not emitted, emitted
            checks += 1

print(f'  RESULT {checks}/{checks} compile-time action checks passed')
