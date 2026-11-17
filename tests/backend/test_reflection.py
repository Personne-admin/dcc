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

STATIC_FOR = '''module main;
import core::meta;

public struct Triple(A, B, C) { A a; B b; C c; }

usize count(usize n) { return n + 1; }

Status(Error) each_argument(T)() {
    static for i in 0..core::meta::template_argument_count!T() {
        usize index = i;
        println("arg {} = {}", index, core::meta::type_name!(core::meta::type_argument(T, i))())?;
    }
    return Status::Ok;
}

Status(Error) sum_array(T)(T values) {
    i64 total = 0;
    static for i in 0..core::meta::array_length!T() {
        total += values[i] as i64;
    }
    println("sum {}", total)?;
    return Status::Ok;
}

public Status(Error) main() {
    static for i in 0..=2 {
        const usize square = i * i;
        println("square {}", square)?;
    }
    static for i in 1..count(2) {
        println("ctfe bound {}", i)?;
    }
    static for i in 0..3 {
        core::meta::type_argument(Triple(i32, i8, u16), i) value = {};
        println("bits {}", core::meta::bits!(typeof(value))())?;
    }
    static for i in 3..3 {
        println("never")?;
    }
    each_argument!(Triple(i32, bool, [] const char))()?;
    i16[4] values = {1, 2, 3, 4};
    sum_array(values)?;
    return Status::Ok;
}
'''

STATIC_FOR_EXPECTED = '''square 0
square 1
square 4
ctfe bound 1
ctfe bound 2
bits 32
bits 8
bits 16
arg 0 = i32
arg 1 = bool
arg 2 = []const char
sum 10
'''

for backend in ('llvm', 'custom'):
    for opt in ('-O0', '-O2'):
        run(STATIC_FOR, STATIC_FOR_EXPECTED, '-fbackend', backend, opt)

rejects('''module main;
public i32 main([] const char* args) {
    static for i in 0..args.len {
    }
    return 0;
}
''', [('static for range end must be a compile-time constant integer', 3)])
rejects('''module main;
public i32 main() {
    static for i in 0..70000 {
    }
    return 0;
}
''', [('static for range has 70000 iterations; at most 65536 are expanded', 3)])

RECORDS = '''module main;
import core::meta;

public struct Point { i32 x; i64 y = 7; u8 tag; }
public struct Box(T) { u8 flag; T value; }
public struct Flags { u8 a : 3; u8 b : 5; u16 c; }
public union Raw { u32 word; u8 byte; }
public struct Named { i32 x; i64 y; [] const char label; }

Status(Error) dump(T)() {
    println("{} has {} fields", core::meta::type_name!T(core::meta::NameStyle::Plain), core::meta::field_count!T())?;
    static for i in 0..core::meta::field_count!T() {
        static if core::meta::field_is_bitfield!T(i) {
            println("  {}: {} bits {}+{}", core::meta::field_name!T(i), core::meta::type_name!(core::meta::field_type(T, i))(),
                    core::meta::field_bit_offset!T(i), core::meta::field_bit_width!T(i))?;
        } else {
            println("  {}: {} @{} default={} line={}", core::meta::field_name!T(i), core::meta::type_name!(core::meta::field_type(T, i))(),
                    core::meta::field_offset!T(i), core::meta::field_has_default!T(i), core::meta::field_location!T(i).line)?;
        }
    }
    return Status::Ok;
}

Status(Error) show(T)(T* value) {
    print("{} {{", core::meta::type_name!T(core::meta::NameStyle::Plain))?;
    static for i in 0..core::meta::field_count!T() {
        print(" {} = {}", core::meta::field_name!T(i), *core::meta::field!T(value, i))?;
    }
    println(" }}")?;
    return Status::Ok;
}

void zero_integers(T)(T* value) {
    static for i in 0..core::meta::field_count!T() {
        static if core::meta::kind!(core::meta::field_type(T, i))() == core::meta::TypeKind::Int {
            *core::meta::field!T(value, i) = 0;
        }
    }
}

i64 sum_at_compile_time() {
    Named n = { x = 3, y = 4, label = "c" };
    return *core::meta::field!(Named)(&n, 0) as i64 + *core::meta::field!(Named)(&n, 1);
}

const i64 SUM = sum_at_compile_time();

public Status(Error) main() {
    println("records")?;
    dump!(Point)()?;
    dump!(Box(i64))()?;
    dump!(Flags)()?;
    dump!(Raw)()?;
    println("{} {} {}", core::meta::has_field!(Point)("y"), core::meta::has_field!(Point)("z"), core::meta::field_index!(Point)("tag"))?;
    Named n = { x = 1, y = 2, label = "here" };
    show(&n)?;
    zero_integers(&n);
    show(&n)?;
    println("{}", SUM)?;
    return Status::Ok;
}
'''

RECORDS_EXPECTED = '''records
Point has 3 fields
  x: i32 @0 default=false line=4
  y: i64 @8 default=true line=4
  tag: u8 @16 default=false line=4
Box(i64) has 2 fields
  flag: u8 @0 default=false line=5
  value: i64 @8 default=false line=5
Flags has 3 fields
  a: u8 bits 0+3
  b: u8 bits 3+5
  c: u16 @2 default=false line=6
Raw has 2 fields
  word: u32 @0 default=false line=7
  byte: u8 @0 default=false line=7
true false 2
Named { x = 1 y = 2 label = here }
Named { x = 0 y = 0 label = here }
7
'''

for backend in ('llvm', 'custom'):
    for opt in ('-O0', '-O2'):
        run(RECORDS, RECORDS_EXPECTED, '-fbackend', backend, opt)

rejects('''module main;
import core::meta;
public struct Flags { u8 a : 3; u8 b : 5; }
const usize O = core::meta::field_offset!(Flags)(0);
public i32 main() { return 0; }
''', [('field `a` is a bitfield; use field_bit_offset', 4)])
rejects('''module main;
import core::meta;
public struct Point { i32 x; }
const usize O = core::meta::field_offset!(Point)(3);
public i32 main() { return 0; }
''', [('field index 3 is out of range for `Point` with 1 fields', 4)])
rejects('''module main;
import core::meta;
const usize N = core::meta::field_count!(i32)();
public i32 main() { return 0; }
''', [('field_count requires a struct or union type, not `i32`', 3)])
rejects('''module main;
import core::meta;
public struct Point { i32 x; }
const usize N = core::meta::field_index!(Point)("y");
public i32 main() { return 0; }
''', [('`Point` has no field `y`', 4)])

print(f'  RESULT {checks}/{checks} reflection checks passed')
