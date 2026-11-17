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

LOCATED = '''module main;
import core;

[] const char repeat(char c, i32 count) {
    Array(char) text = {};
    for i32 i = 0; i < count; i++ {
        if !text.push(c).is_ok() { return "push failed"; }
    }
    return text.items();
}

void fail_here(i32 width, core::source_location::SourceLocation at = core::source_location::source_location()) {
    core::compile_error_at(repeat('x', width), at);
}

void check(T)(T value) {
    static if sizeof(T) > 4 {
        fail_here(sizeof(T) as i32);
    }
}

public i32 main() {
    core::compile_warning_at("located warning", core::source_location::source_location());
    core::Note[2] notes = {
        { message = "first note", location = core::source_location::source_location() },
        { message = "second note", location = { file = "elsewhere.dc", line = 3, column = 4, function = "" } },
    };
    core::compile_error_with("error with notes", core::source_location::source_location(), notes[0..2]);
    core::compile_error_at("unmapped", { file = "nowhere.dc", line = 9, column = 2, function = "" });
    check(1 as u64);
    return 0;
}
'''
directory = Path(tempfile.mkdtemp(prefix='dcc-compile-time-actions-'))
(directory / 'main.dc').write_text(LOCATED)
result = subprocess.run(['timeout', '60', str(compiler), '-flibdcext', str(directory / 'main.dc'), '-o', str(directory / 'program')],
                        capture_output=True, text=True, timeout=65)
output = [line.replace(str(directory) + '/', '') for line in ANSI.sub('', result.stderr).splitlines()
          if re.match(r'(error|warning|note): |\s*-->|\s*= note', line)]
assert result.returncode == 1 and output == [
    'warning: located warning', '   --> main.dc:23:49',
    'error: error with notes', '   --> main.dc:28:50', '    = note: first note', '  --> main.dc:25:46', '    = note: second note', '  --> elsewhere.dc:3:4',
    'error: nowhere.dc:9:2: unmapped', '   --> main.dc:29:5',
    'error: xxxxxxxx', '   --> main.dc:18:9',
], output
checks += 1

def located_output(source):
    directory = Path(tempfile.mkdtemp(prefix='dcc-compile-time-actions-'))
    (directory / 'main.dc').write_text(source)
    result = subprocess.run(['timeout', '60', str(compiler), '-flibdcext', str(directory / 'main.dc'), '-o', str(directory / 'program')],
                            capture_output=True, text=True, timeout=65)
    return result.returncode, [line.replace(str(directory) + '/', '') for line in ANSI.sub('', result.stderr).splitlines()
                               if re.match(r'(error|warning|note): |\s*-->|\s*= note', line)]


FORMATTED = '''module main;
import core;
public import std::fmt;

public struct Money { i64 cents; }
public Status(std::fmt::FormatError(std::fmt::WriterError(W))) format(W)(const Money* self, W* out, const std::fmt::Options* opts) if std::fmt::Writer(W) {
    return std::fmt::format(out, "${}.{:02}", self.cents / 100, self.cents % 100);
}

void limit(T)(T value) {
    static if sizeof(T) > 4 {
        std::fmt::compile_error("`{}` is {} bytes, the limit is {:>3}; costs {}", "T", sizeof(T), 4, Money { cents = 1250 });
    }
}

public i32 main() {
    std::fmt::compile_warning("plain warning {}", 1);
    limit(1 as u64);
    const core::Note[1] notes = { std::fmt::note_at(core::source_location::source_location(), "hint {}", "here") };
    std::fmt::compile_error_with(core::source_location::source_location(), notes[0..1], "with {} note", 1);
}
'''
code, output = located_output(FORMATTED)
assert code == 1 and output == [
    'warning: plain warning 1', '   --> main.dc:17:5',
    'error: `T` is 8 bytes, the limit is   4; costs $12.50', '   --> main.dc:12:9',
    'error: with 1 note', '   --> main.dc:20:34', '    = note: hint here', '  --> main.dc:19:53',
], output
checks += 1

GUIDE = '''module main;
import core;
import core::meta;
public import std::fmt;
public struct Plain { i32 x; }
void require_hook(T)() {
    const core::meta::Check hook = compiles (T t, std::fmt::CountingWriter* w, const std::fmt::Options* o) {
        t.format(w, o);
    };
    static if !hook.ok {
        core::Note[1] notes = { std::fmt::note_at(hook.first.location, "{}", hook.first.message) };
        std::fmt::compile_error_with(core::source_location::source_location(), notes[0..1],
            "`{}` has no usable format hook", core::meta::type_name!T());
    }
}

public i32 main() { require_hook!(Plain)(); return 0; }
'''
code, output = located_output(GUIDE)
assert code == 1 and output == [
    'error: `main::Plain` has no usable format hook', '   --> main.dc:12:38',
    '    = note: no matching UFCS function for `format` on receiver type `Plain`', '  --> main.dc:8:9',
], output
checks += 1

print(f'  RESULT {checks}/{checks} compile-time action checks passed')
