from pathlib import Path
import re
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
checks = 0

ANSI = re.compile(r'\x1b\[[0-9;]*m')


def build(source, *flags):
    directory = Path(tempfile.mkdtemp(prefix='dcc-compile-time-heap-'))
    path, exe = directory / 'main.dc', directory / 'program'
    path.write_text(source)
    result = subprocess.run(['timeout', '60', str(compiler), '-flibdcext', *flags, str(path), '-o', str(exe)], capture_output=True, text=True, timeout=65)
    return result.returncode, ANSI.sub('', result.stderr), exe


def errors(stderr):
    return [line[len('error: '):] for line in stderr.splitlines() if line.startswith('error: ')]


CONTAINERS = '''module main;
import core;

Status(AllocError) append(Array(char)* text, [] const char bytes) {
    for usize i = 0; i < bytes.len; i++ {
        text.push(bytes[i])?;
    }
    return Status::Ok;
}

[] const char describe(i32 count) {
    Array(char) text = {};
    for i32 i = 0; i < count; i++ {
        if !append(&text, "item ").is_ok() { return "append failed"; }
        if !text.push(('0' as i32 + i % 10) as char).is_ok() { return "push failed"; }
        if !append(&text, "; ").is_ok() { return "append failed"; }
    }
    return text.items();
}

usize sum_squares(usize n) {
    Array(usize) values = {};
    defer values.deinit();
    for usize i = 1; i <= n; i++ {
        if !values.push(i * i).is_ok() { return 0; }
    }
    usize total = 0;
    for usize i = 0; i < values.items().len; i++ { total += values.items()[i]; }
    return total;
}

i32 defer_order() {
    i32 trace = 0;
    {
        defer trace = trace * 10 + 1;
        defer trace = trace * 10 + 2;
        trace = 3;
    }
    return trace;
}

i32 defer_after_return_value() {
    i32 value = 5;
    defer value = 9;
    return value;
}

bool mode() { return core::is_compile_time(); }

const usize SQUARES = sum_squares(10);
const [] const char TEXT = describe(40);
const i32 ORDER = defer_order();
const i32 RETURNED = defer_after_return_value();
const bool AT_COMPILE_TIME = mode();

void check(T)(T unused) {
    static if sum_squares(100) == 338350 {
        return;
    } else {
        core::compile_error("compile-time Array gave a wrong sum");
    }
}

public Status(Error) main() {
    check(1 as i32);
    println("{} {} {} {} {} {}", SQUARES, sum_squares(4), ORDER, defer_order(), RETURNED, defer_after_return_value())?;
    println("{} {} [{}] [{}]", AT_COMPILE_TIME, mode(), TEXT.len, TEXT[0..14])?;
    println("[{}]", describe(2))?;
    return Status::Ok;
}
'''

EXPECTED = '385 30 321 321 5 5\ntrue false [320] [item 0; item 1]\n[item 0; item 1; ]\n'

for backend in ('llvm', 'custom'):
    for opt in ('-O0', '-O2'):
        for whole in ([], ['-fwhole-program']):
            code, stderr, exe = build(CONTAINERS, '-fbackend', backend, opt, *whole)
            assert code == 0, (backend, opt, whole, stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            assert run.returncode == 0 and run.stdout == EXPECTED, (backend, opt, whole, run.returncode, run.stdout, run.stderr)
            checks += 1

MESSAGE = '''module main;
import core;

[] const char describe(i32 count) {
    Array(char) text = {};
    for i32 i = 0; i < count; i++ {
        if !text.push(('a' as i32 + i) as char).is_ok() { return "push failed"; }
    }
    return text.items();
}

void check(T)(T unused) {
    static if T == u8 {
    } else {
        core::compile_error(describe(5));
    }
}

public i32 main() { check(1 as i32); return 0; }
'''
code, stderr, _ = build(MESSAGE)
assert code == 1 and errors(stderr) == ['abcde'], stderr
checks += 1

ESCAPE = '''module main;
Array(i32) make() {
    Array(i32) a = {};
    if !a.push(7).is_ok() { return a; }
    return a;
}
const Array(i32) LEAKED = make();
public i32 main() { return 0; }
'''
code, stderr, _ = build(ESCAPE)
assert code == 1 and any('compile-time allocation escapes to run time' in e for e in errors(stderr)), stderr
checks += 1

DOUBLE_FREE = '''module main;
import core::meta;
usize twice() {
    [] i32 items = core::meta::allocate!i32(2);
    core::meta::free(items);
    core::meta::free(items);
    return items.len;
}
const usize N = twice();
public i32 main() { return 0; }
'''
code, stderr, _ = build(DOUBLE_FREE)
assert code == 1 and any('double free of a compile-time allocation' in e for e in errors(stderr)), stderr
checks += 1

FOREIGN_FREE = '''module main;
import core::meta;
usize foreign() {
    i32[2] local = {};
    core::meta::free(local[0..2]);
    return 0;
}
const usize N = foreign();
public i32 main() { return 0; }
'''
code, stderr, _ = build(FOREIGN_FREE)
assert code == 1 and any('free of storage not allocated at compile time' in e for e in errors(stderr)), stderr
checks += 1

MISUSE = '''module main;
import core::meta;
i32 misuse() {
    [] i32 raw = core::meta::allocate!i32(4);
    return raw.len as i32;
}
public i32 main() { return misuse(); }
'''
for backend in ('llvm', 'custom'):
    for opt in ('-O0', '-O2'):
        code, stderr, exe = build(MISUSE, '-fbackend', backend, opt)
        assert code == 0, stderr
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        assert run.returncode != 0 and 'core::meta::allocate called at run time' in run.stderr, (backend, opt, run.returncode, run.stderr)
        checks += 1

print(f'  RESULT {checks}/{checks} compile-time heap checks passed')
