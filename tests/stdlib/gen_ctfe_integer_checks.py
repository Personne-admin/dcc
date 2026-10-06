from pathlib import Path


def generate():
    source = ["module main;", ""]
    checks = []
    number = 0
    for bits in (8, 16, 32, 64):
        unsigned = "u%d" % bits
        for signed in (False, True):
            typ = ("i" if signed else "u") + str(bits)
            mask = (1 << bits) - 1
            value = (1 << (bits - 1)) | 1
            def canonical(n):
                n &= mask
                return n - (1 << bits) if signed and n & (1 << (bits - 1)) else n
            operations = [
                ("shl", "v << 1", value, canonical(value << 1)),
                ("shr", "v >> 1", value, canonical(canonical(value) >> 1)),
                ("rotl", "(((v as %s) << 1) | ((v as %s) >> %d)) as %s" % (unsigned, unsigned, bits - 1, typ), value, canonical((value << 1) | (value >> (bits - 1)))),
                ("rotr", "(((v as %s) >> 1) | ((v as %s) << %d)) as %s" % (unsigned, unsigned, bits - 1, typ), value, canonical((value >> 1) | (value << (bits - 1)))),
                ("add", "v + 1", 17 if bits == 64 else mask >> (1 if signed else 0), canonical(18 if bits == 64 else (mask >> (1 if signed else 0)) + 1)),
                ("sub", "v - 1", 17 if signed or bits == 64 else 0, canonical(16 if signed or bits == 64 else -1)),
                ("mul", "v * 3", 17 if bits == 64 else (1 << (bits - 2)) + 1, canonical(51 if bits == 64 else ((1 << (bits - 2)) + 1) * 3)),
                ("not", "~v", 2, canonical(~2)),
                ("neg", "-v" if signed else "(-(v as i%d)) as %s" % (bits, typ), 2, canonical(-2)),
            ]
            for op, expr, arg, expected in operations:
                name = op + "_" + typ
                source += ["%s %s(%s v) { return %s; }" % (typ, name, typ, expr), "@runtime %s runtime_%s(%s v) { return %s; }" % (typ, name, typ, expr)]
                number += 1
                call = "%s(0x%x as %s)" % (name, arg, typ)
                checks += ["    static if %s != (%d as %s) { return %d; }" % (call, expected, typ, number), "    if runtime_%s != %s { return %d; }" % (call, call, number)]
            name = "cast_" + typ
            source += ["%s %s(u64 v) { return v as %s; }" % (typ, name, typ), "@runtime %s runtime_%s(u64 v) { return v as %s; }" % (typ, name, typ)]
            number += 1
            checks += ["    static if %s(0xffffffffffffffff) != (%d as %s) { return %d; }" % (name, canonical(mask), typ, number), "    if runtime_%s(0xffffffffffffffff) != %s(0xffffffffffffffff) { return %d; }" % (name, name, number)]
    checks += ["    i%d minimum%d = %d;" % (bits, bits, -(1 << (bits - 1))) for bits in (8, 16, 32, 64)]
    checks += ["    if minimum%d != (%d as i%d) { return 81; }" % (bits, -(1 << (bits - 1)), bits) for bits in (8, 16, 32, 64)]
    checks += ["    static if (0xa0968c82786e645a as u64) != (11571590784569205850 as u64) { return 82; }"]
    return "\n".join(source + ["", "public i32 main() {"] + checks + ["    return 0;", "}", ""])


if __name__ == "__main__":
    Path(__file__).with_name("ctfe-integer-canonical.dc").write_text(generate())
