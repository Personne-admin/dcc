from pathlib import Path
import re
import subprocess
import sys
import tempfile

compiler = Path(sys.argv[1]).resolve()
checks = 0

ANSI = re.compile(r'\x1b\[[0-9;]*m')


def build(source, *flags):
    directory = Path(tempfile.mkdtemp(prefix='dcc-reflection-'))
    (directory / 'main.dc').write_text(source)
    exe = directory / 'program'
    result = subprocess.run(['timeout', '60', str(compiler), '-flibdcext', *flags, str(directory / 'main.dc'), '-o', str(exe)],
                            capture_output=True, text=True, timeout=65)
    stderr = ANSI.sub('', result.stderr)
    lines = stderr.splitlines()
    errors = []
    for i, line in enumerate(lines):
        if line.startswith('error: '):
            location = re.search(r'main\.dc:(\d+):', lines[i + 1]) if i + 1 < len(lines) else None
            errors.append((line[len('error: '):], int(location.group(1)) if location else None))
    return result.returncode, errors, stderr, exe


def run(source, expected, *flags):
    global checks
    code, _, stderr, exe = build(source, *flags)
    assert code == 0, (flags, stderr)
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0 and result.stdout == expected, (flags, result.returncode, result.stdout, result.stderr)
    checks += 1


def rejects(source, expected):
    global checks
    code, errors, stderr, _ = build(source)
    assert code == 1 and errors == expected, (errors, stderr)
    checks += 1


TYPES = '''module main;
import core::meta;

public struct Pair(A, B) { A first; B second; }
public struct Plain { i32 x; }
public enum Color : u8 { Red, Green }
public enum Shape { Dot, Circle(i32) }
@nominal using Meters = i32;

Status(Error) describe(T)(T value) {
    static if core::meta::kind!T() == core::meta::TypeKind::Int {
        println("{} int signed={} bits={}", core::meta::type_name!T(), core::meta::is_signed!T(), core::meta::bits!T())?;
    } else static if core::meta::kind!T() == core::meta::TypeKind::Array {
        core::meta::element(T) first = value[0];
        println("{} array of {} x{} first={}", core::meta::type_name!T(), core::meta::type_name!(core::meta::element(T))(), core::meta::array_length!T(), first)?;
    } else {
        println("{} kind={} args={}", core::meta::type_name!T(), core::meta::kind!T() as u8, core::meta::template_argument_count!T())?;
    }
    return Status::Ok;
}

public Status(Error) main() {
    describe(1 as i64)?;
    describe(1 as u8)?;
    i32[3] numbers = {4, 5, 6};
    describe(numbers)?;
    Pair(i32, bool) pair = { first = 1, second = true };
    describe(pair)?;
    describe(Plain { x = 1 })?;
    describe(Color::Green)?;
    describe(Shape::Dot)?;
    i32* p = null;
    core::meta::pointee(typeof(p)) pointed = 9;
    core::meta::underlying(Color) raw = 7;
    core::meta::element([] const u16) wide = 300;
    println("{} {} {} {} {}", pointed, raw, wide, core::meta::same_template!(Pair(i32, bool), Pair(u8, u8))(),
            core::meta::same_template!(Pair(i32, bool), Plain)())?;
    println("{} | {}", core::meta::type_name!(Pair(Plain, [] const char))(core::meta::NameStyle::Plain), core::meta::type_name!(f64)())?;
    println("{} {} {}", core::meta::bits!(Meters)(), core::meta::kind!(Meters)() as u8, core::meta::type_name!(core::meta::underlying(Meters))())?;
    return Status::Ok;
}
'''

EXPECTED = '''i64 int signed=true bits=64
u8 int signed=false bits=8
i32[3] array of i32 x3 first=4
main::Pair(i32, bool) kind=8 args=2
main::Plain kind=8 args=0
main::Color kind=10 args=0
main::Shape kind=11 args=0
9 7 300 true false
Pair(Plain, []const char) | f64
32 14 i32
'''

for backend in ('llvm', 'custom'):
    for opt in ('-O0', '-O2'):
        run(TYPES, EXPECTED, '-fbackend', backend, opt)

rejects('''module main;
import core::meta;
public struct Plain { i32 x; }
const bool S = core::meta::is_signed!(Plain)();
public i32 main() { return 0; }
''', [('is_signed requires an integer or floating-point type, not `Plain`', 4)])
rejects('''module main;
import core::meta;
const usize N = core::meta::array_length!([] const u8)();
public i32 main() { return 0; }
''', [('array_length requires an array type, not `[]const u8`', 3)])
rejects('''module main;
import core::meta;
core::meta::element(i32) x = 0;
public i32 main() { return 0; }
''', [('`i32` has no element type', 3)])
rejects('''module main;
import core::meta;
core::meta::pointee(i32) x = 0;
public i32 main() { return 0; }
''', [('`i32` has no pointee type', 3)])

print(f'  RESULT {checks}/{checks} reflection checks passed')
