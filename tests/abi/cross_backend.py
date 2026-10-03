#!/usr/bin/env python3
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MASK = (1 << 64) - 1

INT_TYPES = {
    "i8": ("int8_t", 8, True),
    "i16": ("int16_t", 16, True),
    "i32": ("int32_t", 32, True),
    "i64": ("int64_t", 64, True),
    "u8": ("uint8_t", 8, False),
    "u16": ("uint16_t", 16, False),
    "u32": ("uint32_t", 32, False),
    "u64": ("uint64_t", 64, False),
}
FLOAT_TYPES = {"f32": "float", "f64": "double"}
BASED_TYPES = {"BFS": "FS", "BGS": "GS"}
STRUCTS = {
    "F2": [("x", "f64"), ("y", "f64")],
    "FS": [("x", "f32"), ("y", "f32")],
    "M8": [("a", "i32"), ("b", "f32")],
    "D3": [("a", "f64"), ("b", "f64"), ("c", "f64")],
    "I3": [("a", "i32"), ("b", "i32"), ("c", "i32")],
    "MX": [("a", "f64"), ("b", "i64")],
}
BYTE_SIZES = list(range(1, 33))


def mix(h, c):
    return ((((h << 5) | (h >> 59)) & MASK) ^ (c & MASK)) & MASK


def is_bytes(t):
    return t.startswith("S") and t[1:].isdigit()


def is_slice(t):
    return t == "SL"


def is_agg(t):
    return is_bytes(t) or is_slice(t) or t in STRUCTS


def is_based(t):
    return t in BASED_TYPES


def dc_type(t):
    return "[] const u8" if is_slice(t) else "u8^" + BASED_TYPES[t] if is_based(t) else t


def wrap(value, bits, signed):
    value &= (1 << bits) - 1
    if signed and value >> (bits - 1):
        value -= 1 << bits
    return value


def scalar_value(t, base, j):
    if is_based(t):
        return base * 17 + j + 1
    if t in INT_TYPES:
        _, bits, signed = INT_TYPES[t]
        v = base * 0x100000001 if bits == 64 else base * 3
        if signed and j % 2 == 1:
            v = -v
        return wrap(v, bits, signed)
    return base + 0.25 * (j % 4)


def contrib_scalar(t, v):
    if t in INT_TYPES or is_based(t):
        return v & MASK
    return int(v * 4.0) & MASK


def agg_values(t, base):
    if is_slice(t):
        return [(base + i * 13) & 255 for i in range(base % 5 + 3)]
    if is_bytes(t):
        return [(base + i * 13) & 255 for i in range(int(t[1:]))]
    return [scalar_value(ft, base + i, i) for i, (_, ft) in enumerate(STRUCTS[t])]


def contrib(t, v):
    if t in INT_TYPES or t in FLOAT_TYPES or is_based(t):
        return contrib_scalar(t, v)
    h = mix(0, len(v)) if is_slice(t) else 0
    fields = (
        [("u8", b) for b in v]
        if is_bytes(t) or is_slice(t)
        else [(ft, x) for (_, ft), x in zip(STRUCTS[t], v)]
    )
    for ft, x in fields:
        h = mix(h, contrib_scalar(ft, x))
    return h


def dc_lit(t, v):
    if is_based(t):
        return "%s:(%d as usize)" % (BASED_TYPES[t], v)
    if t in FLOAT_TYPES:
        return "(%r as %s)" % (float(v), t)
    return "((%d) as %s)" % (v, t)


def c_type(t):
    if is_slice(t):
        return "struct SL"
    if is_based(t):
        return "uintptr_t"
    if t in INT_TYPES:
        return INT_TYPES[t][0]
    if t in FLOAT_TYPES:
        return FLOAT_TYPES[t]
    return "struct %s" % t


def c_lit(t, v):
    if is_based(t):
        return "(uintptr_t)%d" % v
    if t in FLOAT_TYPES:
        return "(%s)%r" % (FLOAT_TYPES[t], float(v))
    if t == "i64" and v == -(1 << 63):
        return "INT64_MIN"
    suffix = "ULL" if t == "u64" else ("LL" if t == "i64" else "")
    return "(%s)%d%s" % (INT_TYPES[t][0], v, suffix)


class Case:
    def __init__(self, kind, index, args, ret=None):
        self.kind = kind
        self.index = index
        self.args = args
        self.ret = ret
        self.values = []
        for j, t in enumerate(args):
            base = (index * 31 + j * 7) % 97 + 1
            self.values.append(
                agg_values(t, base) if is_agg(t) else scalar_value(t, base, j)
            )

    def describe(self):
        sig = "(" + ", ".join(self.args) + ")"
        return ("%s %s" % (self.ret, sig)) if self.kind == "ret" else ("u64 %s" % sig)


def build_cases():
    cases = []
    params = [
        ["i64"] * 8,
        ["i32", "i64", "i8", "u16", "i16", "u8", "u32", "u64", "i8", "i64"],
        ["f64"] * 8,
        ["f32"] * 6,
        ["i32", "f64", "i64", "f32", "i32", "f64", "i64", "f32"],
        ["f64", "i64", "f32", "i32", "f64", "f64", "i64", "f32", "i32"],
    ]
    params += [["S%d" % n] for n in BYTE_SIZES]
    params += [
        ["i32", "S%d" % n, "f64", "S%d" % n, "i64", "f32"]
        for n in (3, 5, 8, 12, 16, 17, 24, 32)
    ]
    params += [[s] for s in STRUCTS] + [["f64", s, "i32", s, "f32"] for s in STRUCTS]
    params += [["S1", "S2", "S3", "S4", "S5", "S6", "S7", "S8", "S9"]]
    params += [
        ["SL"],
        ["i32", "SL", "f64", "SL", "i64", "SL"],
        ["SL", "S3", "SL", "F2", "SL"],
    ]
    for p in params:
        cases.append(Case("param", len(cases) + 1, p))
    rets = (
        ["S%d" % n for n in BYTE_SIZES]
        + list(STRUCTS)
        + ["SL", "f64", "f32", "i8", "u16", "i32", "u8"]
    )
    for r in rets:
        cases.append(
            Case("ret", len(cases) + 1, ["i64", "f64", "i32", "f64", "i64"], r)
        )
    cases += [
        Case("param", len(cases) + 1, ["BFS", "i64", "BGS", "u8"]),
        Case("param", len(cases) + 2, ["i64", "BGS", "BFS", "u64", "BFS"]),
        Case("ret", len(cases) + 3, ["i64", "f64", "i32", "f64", "i64"], "BFS"),
        Case("ret", len(cases) + 4, ["i64", "f64", "i32", "f64", "i64"], "BGS"),
    ]
    return cases


def expected_param(case):
    h = 0
    for t, v in zip(case.args, case.values):
        h = mix(h, contrib(t, v))
    return h


def ret_seed(case):
    return (case.values[0] & 0xFF) + 1


def ret_value(case):
    t = case.ret
    seed = ret_seed(case)
    if is_agg(t):
        return agg_values(t, seed)
    if t in FLOAT_TYPES:
        return seed + 0.5
    if is_based(t):
        return seed + 250
    _, bits, signed = INT_TYPES[t]
    return wrap(-seed if signed else seed + 250, bits, signed)


def gen_dc_types():
    out = []
    for n in BYTE_SIZES:
        out.append("public struct S%d {\n    u8[%d] b;\n}\n" % (n, n))
    for name, fields in STRUCTS.items():
        out.append(
            "public struct %s {\n%s}\n"
            % (name, "".join("    %s %s;\n" % (ft, fn) for fn, ft in fields))
        )
    return "\n".join(out)


def gen_c_types():
    out = ["#include <stdint.h>\n"]
    for n in BYTE_SIZES:
        out.append("struct S%d { uint8_t b[%d]; };\n" % (n, n))
    for name, fields in STRUCTS.items():
        out.append(
            "struct %s { %s };\n"
            % (name, " ".join("%s %s;" % (c_type(ft), fn) for fn, ft in fields))
        )
    out.append("struct SL { const uint8_t* p; uint64_t n; };\n")
    out.append(
        "static uint64_t mix(uint64_t h, uint64_t c) { return ((h << 5) | (h >> 59)) ^ c; }\n"
    )
    return "".join(out)


def dc_contrib_expr(t, expr):
    if is_based(t):
        return "(%s.offset() as u64)" % expr
    if t in INT_TYPES:
        return "(%s as u64)" % expr
    if t in FLOAT_TYPES:
        return "(((%s * 4.0) as i64) as u64)" % (
            expr if t == "f64" else "(%s as f64)" % expr
        )
    if is_slice(t):
        return "contrib_SL(%s)" % expr
    return "contrib_%s(&%s)" % (t, expr)


def c_contrib_expr(t, expr):
    if t in INT_TYPES or is_based(t):
        return "(uint64_t)(%s)" % expr
    if t in FLOAT_TYPES:
        return "(uint64_t)(int64_t)((double)(%s) * 4.0)" % expr
    if is_slice(t):
        return "contrib_SL(%s)" % expr
    return "contrib_%s(&%s)" % (t, expr)


def gen_dc_contribs():
    out = [
        "u64 mix(u64 h, u64 c) {\n    return ((h << 5) | (h >> 59)) ^ c;\n}\n",
        "u64 contrib_SL([] const u8 s) {\n    u64 h = mix(0, s.len as u64);\n    for usize i = 0; i < s.len; i++ {\n        h = mix(h, s[i] as u64);\n    }\n    return h;\n}\n",
        "u8[64] ret_buf;\n",
    ]
    for n in BYTE_SIZES:
        out.append(
            "u64 contrib_S%d(const S%d* s) {\n    u64 h = 0;\n    for usize i = 0; i < %d; i++ {\n        h = mix(h, s.b[i] as u64);\n    }\n    return h;\n}\n"
            % (n, n, n)
        )
    for name, fields in STRUCTS.items():
        body = "".join(
            "    h = mix(h, %s);\n" % dc_contrib_expr(ft, "s.%s" % fn)
            for fn, ft in fields
        )
        out.append(
            "u64 contrib_%s(const %s* s) {\n    u64 h = 0;\n%s    return h;\n}\n"
            % (name, name, body)
        )
    return "\n".join(out)


def gen_c_contribs():
    out = [
        "static uint64_t contrib_SL(struct SL s) { uint64_t h = mix(0, s.n); for (uint64_t i = 0; i < s.n; ++i) h = mix(h, s.p[i]); return h; }\n",
        "static uint8_t ret_buf[64];\n",
    ]
    for n in BYTE_SIZES:
        out.append(
            "static uint64_t contrib_S%d(const struct S%d* s) { uint64_t h = 0; for (int i = 0; i < %d; ++i) h = mix(h, s->b[i]); return h; }\n"
            % (n, n, n)
        )
    for name, fields in STRUCTS.items():
        body = " ".join(
            "h = mix(h, %s);" % c_contrib_expr(ft, "s->%s" % fn) for fn, ft in fields
        )
        out.append(
            "static uint64_t contrib_%s(const struct %s* s) { uint64_t h = 0; %s return h; }\n"
            % (name, name, body)
        )
    return "".join(out)


def dc_fill(t, var, values):
    if is_slice(t):
        return "    u8[%d] %s_buf = {%s};\n" % (
            len(values),
            var,
            ", ".join(str(b) for b in values),
        )
    if is_bytes(t):
        return "".join(
            "    %s.b[%d] = %d as u8;\n" % (var, i, b) for i, b in enumerate(values)
        )
    return "".join(
        "    %s.%s = %s;\n" % (var, fn, dc_lit(ft, v))
        for (fn, ft), v in zip(STRUCTS[t], values)
    )


def c_fill(t, var, values):
    if is_slice(t):
        return (
            "    static const uint8_t %s_buf[%d] = {%s};\n    %s.p = %s_buf;\n    %s.n = %d;\n"
            % (
                var,
                len(values),
                ", ".join(str(b) for b in values),
                var,
                var,
                var,
                len(values),
            )
        )
    if is_bytes(t):
        return "".join(
            "    %s.b[%d] = %d;\n" % (var, i, b) for i, b in enumerate(values)
        )
    return "".join(
        "    %s.%s = %s;\n" % (var, fn, c_lit(ft, v))
        for (fn, ft), v in zip(STRUCTS[t], values)
    )


def dc_cmp_fields(t, var, values, fail):
    if is_slice(t):
        return "    if %s.len != %d { %s }\n" % (var, len(values), fail) + "".join(
            "    if %s[%d] != %d as u8 { %s }\n" % (var, i, b, fail)
            for i, b in enumerate(values)
        )
    if is_bytes(t):
        return "".join(
            "    if %s.b[%d] != %d as u8 { %s }\n" % (var, i, b, fail)
            for i, b in enumerate(values)
        )
    return "".join(
        "    if %s.%s != %s { %s }\n" % (var, fn, dc_lit(ft, v), fail)
        for (fn, ft), v in zip(STRUCTS[t], values)
    )


def c_cmp_fields(t, var, values, fail):
    if is_slice(t):
        return "    if (%s.n != %d) %s\n" % (var, len(values), fail) + "".join(
            "    if (%s.p[%d] != %d) %s\n" % (var, i, b, fail)
            for i, b in enumerate(values)
        )
    if is_bytes(t):
        return "".join(
            "    if (%s.b[%d] != %d) %s\n" % (var, i, b, fail)
            for i, b in enumerate(values)
        )
    return "".join(
        "    if (%s.%s != %s) %s\n" % (var, fn, c_lit(ft, v), fail)
        for (fn, ft), v in zip(STRUCTS[t], values)
    )


def ret_guard_dc(case):
    conds = [
        "a%d == %s" % (j, dc_lit(t, v))
        for j, (t, v) in enumerate(zip(case.args, case.values))
        if j > 0
    ]
    return " && ".join(conds)


def ret_guard_c(case):
    return " && ".join(
        "a%d == %s" % (j, c_lit(t, v))
        for j, (t, v) in enumerate(zip(case.args, case.values))
        if j > 0
    )


PRESSURE_INTS = 12
PRESSURE_FLOATS = 12


def pressure_expected(seed):
    ints = [(seed * (i + 3) + i * i) & MASK for i in range(PRESSURE_INTS)]
    floats = [seed * 0.5 + i * 1.25 for i in range(PRESSURE_FLOATS)]
    total = 0
    for v in ints:
        total = mix(total, v)
    for v in floats:
        total = mix(total, int(v * 4.0))
    return total


def clobber_expected(seed):
    data = [(seed + i * 7) & 255 for i in range(512)]
    h = 0
    for b in data:
        h = mix(h, b)
    acc = 0.0
    for i in range(16):
        acc = acc + (seed + i) * 0.5
    return mix(h, int(acc * 4.0))


def gen_dc_callee(cases):
    out = ["module abi_callee;\nimport core::seg;\n", gen_dc_types(), gen_dc_contribs()]
    for c in cases:
        params = ", ".join("%s a%d" % (dc_type(t), j) for j, t in enumerate(c.args))
        if c.kind == "param":
            body = "    u64 h = 0;\n" + "".join(
                "    h = mix(h, %s);\n" % dc_contrib_expr(t, "a%d" % j)
                for j, t in enumerate(c.args)
            )
            out.append(
                "@nomangle\npublic u64 abi_p%d(%s) {\n%s    return h;\n}\n"
                % (c.index, params, body)
            )
            continue
        t = c.ret
        seed = "(if %s { ((a0 & 255) + 1) as u64 } else { 0 as u64 })" % ret_guard_dc(c)
        if is_slice(t):
            body = (
                "    u64 seed = %s;\n    usize n = ((seed %% 5) + 3) as usize;\n    for usize i = 0; i < n; i++ {\n        ret_buf[i] = ((seed + (i as u64) * 13) & 255) as u8;\n    }\n    return ret_buf[0..n];\n"
                % seed
            )
        elif is_bytes(t):
            n = int(t[1:])
            body = (
                "    %s r;\n    u64 seed = %s;\n    for usize i = 0; i < %d; i++ {\n        r.b[i] = ((seed + (i as u64) * 13) & 255) as u8;\n    }\n    return r;\n"
                % (t, seed, n)
            )
        elif t in STRUCTS:
            fills = "".join(
                "    r.%s = %s;\n"
                % (
                    fn,
                    (
                        ("((seed + %d) * 3) as %s" % (i, ft))
                        if ft in INT_TYPES
                        else (
                            ("((seed + %d) as f64 + %r) as %s" % (i, 0.25 * i, ft))
                            if ft == "f32"
                            else "(seed + %d) as f64 + %r" % (i, 0.25 * i)
                        )
                    ),
                )
                for i, (fn, ft) in enumerate(STRUCTS[t])
            )
            body = "    %s r;\n    u64 seed = %s;\n%s    return r;\n" % (t, seed, fills)
        elif t in FLOAT_TYPES:
            body = (
                "    u64 seed = %s;\n    return (seed as f64 + 0.5) as %s;\n"
                % (seed, t)
                if t == "f32"
                else "    u64 seed = %s;\n    return seed as f64 + 0.5;\n" % seed
            )
        elif is_based(t):
            body = "    u64 seed = %s;\n    return %s:((seed + 250) as usize);\n" % (seed, BASED_TYPES[t])
        else:
            _, bits, signed = INT_TYPES[t]
            expr = "(0 as i64 - (seed as i64))" if signed else "(seed + 250)"
            body = "    u64 seed = %s;\n    return %s as %s;\n" % (seed, expr, t)
        out.append(
            "@nomangle\npublic %s abi_r%d(%s) {\n%s}\n"
            % (dc_type(t), c.index, params, body)
        )
    out.append("""public struct Big {
    u8[512] b;
}

Big make_big(u64 seed) {
    Big r;
    for usize i = 0; i < 512; i++ {
        r.b[i] = ((seed + (i as u64) * 7) & 255) as u8;
    }
    return r;
}

@nomangle
public u64 abi_clobber(u64 seed) {
    Big first = make_big(seed);
    Big copy = first;
    u64 h = 0;
    for usize i = 0; i < 512; i++ {
        h = mix(h, copy.b[i] as u64);
    }
    f64 acc = 0.0;
    f64 x0 = (seed as f64) * 0.5;
    f64 x1 = x0 + 0.5;
    f64 x2 = x1 + 0.5;
    f64 x3 = x2 + 0.5;
    f64 x4 = x3 + 0.5;
    f64 x5 = x4 + 0.5;
    f64 x6 = x5 + 0.5;
    f64 x7 = x6 + 0.5;
    f64 x8 = x7 + 0.5;
    f64 x9 = x8 + 0.5;
    f64 x10 = x9 + 0.5;
    f64 x11 = x10 + 0.5;
    f64 x12 = x11 + 0.5;
    f64 x13 = x12 + 0.5;
    f64 x14 = x13 + 0.5;
    f64 x15 = x14 + 0.5;
    acc = x0 + x1 + x2 + x3 + x4 + x5 + x6 + x7 + x8 + x9 + x10 + x11 + x12 + x13 + x14 + x15;
    return mix(h, (acc * 4.0) as u64);
}
""")
    return "\n".join(out)


def gen_c_callee(cases):
    out = [gen_c_types(), gen_c_contribs()]
    for c in cases:
        params = ", ".join("%s a%d" % (c_type(t), j) for j, t in enumerate(c.args))
        if c.kind == "param":
            body = "    uint64_t h = 0;\n" + "".join(
                "    h = mix(h, %s);\n" % c_contrib_expr(t, "a%d" % j)
                for j, t in enumerate(c.args)
            )
            out.append(
                "uint64_t abi_p%d(%s) {\n%s    return h;\n}\n" % (c.index, params, body)
            )
            continue
        t = c.ret
        seed = "uint64_t seed = (%s) ? (uint64_t)((a0 & 255) + 1) : 0;\n" % ret_guard_c(
            c
        )
        if is_slice(t):
            body = (
                "    %s    struct SL r;\n    r.n = seed %% 5 + 3;\n    for (uint64_t i = 0; i < r.n; ++i) ret_buf[i] = (uint8_t)((seed + i * 13) & 255);\n    r.p = ret_buf;\n    return r;\n"
                % seed
            )
        elif is_bytes(t):
            body = (
                "    %s r;\n    %s    for (int i = 0; i < %d; ++i) r.b[i] = (uint8_t)((seed + (uint64_t)i * 13) & 255);\n    return r;\n"
                % (c_type(t), seed, int(t[1:]))
            )
        elif t in STRUCTS:
            fills = "".join(
                "    r.%s = %s;\n"
                % (
                    fn,
                    (
                        ("(%s)((seed + %d) * 3)" % (c_type(ft), i))
                        if ft in INT_TYPES
                        else "(%s)((double)(seed + %d) + %r)"
                        % (c_type(ft), i, 0.25 * i)
                    ),
                )
                for i, (fn, ft) in enumerate(STRUCTS[t])
            )
            body = "    %s r;\n    %s%s    return r;\n" % (c_type(t), seed, fills)
        elif t in FLOAT_TYPES:
            body = "    %s    return (%s)((double)seed + 0.5);\n" % (seed, c_type(t))
        elif is_based(t):
            body = "    %s    return (uintptr_t)(seed + 250);\n" % seed
        else:
            _, bits, signed = INT_TYPES[t]
            expr = "(int64_t)0 - (int64_t)seed" if signed else "seed + 250"
            body = "    %s    return (%s)(%s);\n" % (seed, c_type(t), expr)
        out.append("%s abi_r%d(%s) {\n%s}\n" % (c_type(t), c.index, params, body))
    out.append("""struct Big { uint8_t b[512]; };
static struct Big make_big(uint64_t seed) { struct Big r; for (int i = 0; i < 512; ++i) r.b[i] = (uint8_t)((seed + (uint64_t)i * 7) & 255); return r; }
uint64_t abi_clobber(uint64_t seed) {
    volatile struct Big first = make_big(seed);
    struct Big copy = first;
    uint64_t h = 0;
    for (int i = 0; i < 512; ++i) h = mix(h, copy.b[i]);
    double acc = 0.0;
    for (int i = 0; i < 16; ++i) acc += (double)seed * 0.5 + 0.5 * i;
    return mix(h, (uint64_t)(acc * 4.0));
}
""")
    return "".join(out)


def ret_struct_fields(t, seed):
    if is_slice(t):
        return [(seed + i * 13) & 255 for i in range(seed % 5 + 3)]
    if is_bytes(t):
        return [(seed + i * 13) & 255 for i in range(int(t[1:]))]
    vals = []
    for i, (fn, ft) in enumerate(STRUCTS[t]):
        if ft in INT_TYPES:
            _, bits, signed = INT_TYPES[ft]
            vals.append(wrap((seed + i) * 3, bits, signed))
        else:
            vals.append(seed + i + 0.25 * i)
    return vals


def ret_scalar(t, seed):
    if t in FLOAT_TYPES:
        return seed + 0.5
    if is_based(t):
        return seed + 250
    _, bits, signed = INT_TYPES[t]
    return wrap(-seed if signed else seed + 250, bits, signed)


def gen_dc_caller(cases):
    out = ["module abi_caller;\n\nimport abi_callee;\nimport core::seg;\n\nusing abi_callee::*;\n"]
    for c in cases:
        decls = ""
        args = []
        for j, (t, v) in enumerate(zip(c.args, c.values)):
            if is_slice(t):
                decls += dc_fill(t, "v%d" % j, v)
                args.append("v%d_buf[0..%d]" % (j, len(v)))
            elif is_bytes(t) or t in STRUCTS:
                decls += "    %s v%d;\n" % (t, j) + dc_fill(t, "v%d" % j, v)
                args.append("v%d" % j)
            else:
                args.append(dc_lit(t, v))
        call = "abi_callee::abi_%s%d(%s)" % (
            "p" if c.kind == "param" else "r",
            c.index,
            ", ".join(args),
        )
        fail = "return %d;" % c.index
        if c.kind == "param":
            body = decls + "    if %s != (%d as u64) { %s }\n" % (
                call,
                expected_param(c),
                fail,
            )
        else:
            seed = (c.values[0] & 255) + 1
            t = c.ret
            if is_agg(t):
                body = (
                    decls
                    + "    %s got = %s;\n" % (dc_type(t), call)
                    + dc_cmp_fields(t, "got", ret_struct_fields(t, seed), fail)
                )
            else:
                if is_based(t):
                    body = decls + "    %s got = %s;\n    if got.offset() != (%d as usize) { %s }\n" % (
                        dc_type(t), call, ret_scalar(t, seed), fail
                    )
                else:
                    body = decls + "    %s got = %s;\n    if got != %s { %s }\n" % (
                        t,
                        call,
                        dc_lit(t, ret_scalar(t, seed)),
                        fail,
                    )
        out.append(
            "@nomangle\npublic i32 abi_case%d() {\n%s    return 0;\n}\n"
            % (c.index, body)
        )
    pressure_id = len(cases) + 1
    ints = "".join(
        "    u64 k%d = seed * %d + %d;\n" % (i, i + 3, i * i)
        for i in range(PRESSURE_INTS)
    )
    floats = "".join(
        "    f64 d%d = (seed as f64) * 0.5 + %r;\n" % (i, i * 1.25)
        for i in range(PRESSURE_FLOATS)
    )
    fold = "".join(
        "    t = mix(t, k%d);\n" % i for i in range(PRESSURE_INTS)
    ) + "".join(
        "    t = mix(t, (d%d * 4.0) as u64);\n" % i for i in range(PRESSURE_FLOATS)
    )
    out.append(
        """u64 mix(u64 h, u64 c) {
    return ((h << 5) | (h >> 59)) ^ c;
}

@nomangle
public u64 abi_seed_value = 9;

@nomangle
public i32 abi_pressure() {
    u64 seed = abi_seed_value;
%s%s    u64 c = abi_callee::abi_clobber(seed);
    u64 t = 0;
%s    if t != (%d as u64) { return %d; }
    if c != (%d as u64) { return %d; }
    return 0;
}
"""
        % (
            ints,
            floats,
            fold,
            pressure_expected(9),
            pressure_id,
            clobber_expected(9),
            pressure_id + 1,
        )
    )
    return "\n".join(out)


def gen_c_caller(cases):
    out = [gen_c_types()]
    for c in cases:
        params = ", ".join(c_type(t) for t in c.args)
        rt = "uint64_t" if c.kind == "param" else c_type(c.ret)
        out.append(
            "%s abi_%s%d(%s);\n"
            % (rt, "p" if c.kind == "param" else "r", c.index, params)
        )
    out.append("uint64_t abi_clobber(uint64_t seed);\n")
    for c in cases:
        decls = ""
        args = []
        for j, (t, v) in enumerate(zip(c.args, c.values)):
            if is_agg(t):
                decls += "    %s v%d;\n" % (c_type(t), j) + c_fill(t, "v%d" % j, v)
                args.append("v%d" % j)
            else:
                args.append(c_lit(t, v))
        call = "abi_%s%d(%s)" % (
            "p" if c.kind == "param" else "r",
            c.index,
            ", ".join(args),
        )
        fail = "return %d;" % c.index
        if c.kind == "param":
            body = decls + "    if (%s != %dULL) %s\n" % (call, expected_param(c), fail)
        else:
            seed = (c.values[0] & 255) + 1
            t = c.ret
            if is_agg(t):
                body = (
                    decls
                    + "    %s got = %s;\n" % (c_type(t), call)
                    + c_cmp_fields(t, "got", ret_struct_fields(t, seed), fail)
                )
            else:
                body = decls + "    %s got = %s;\n    if (got != %s) %s\n" % (
                    c_type(t),
                    call,
                    c_lit(t, ret_scalar(t, seed)),
                    fail,
                )
        out.append(
            "__attribute__((noinline)) int abi_case%d(void) {\n%s    return 0;\n}\n"
            % (c.index, body)
        )
    pressure_id = len(cases) + 1
    ints = "".join(
        "    uint64_t k%d = seed * %d + %d;\n" % (i, i + 3, i * i)
        for i in range(PRESSURE_INTS)
    )
    floats = "".join(
        "    double d%d = (double)seed * 0.5 + %r;\n" % (i, i * 1.25)
        for i in range(PRESSURE_FLOATS)
    )
    fold = "".join(
        "    t = mix(t, k%d);\n" % i for i in range(PRESSURE_INTS)
    ) + "".join(
        "    t = mix(t, (uint64_t)(int64_t)(d%d * 4.0));\n" % i
        for i in range(PRESSURE_FLOATS)
    )
    out.append(
        """uint64_t abi_seed_value = 9;
int abi_pressure(void) {
    uint64_t seed = *(volatile uint64_t*)&abi_seed_value;
%s%s    uint64_t c = abi_clobber(seed);
    uint64_t t = 0;
%s    if (t != %dULL) return %d;
    if (c != %dULL) return %d;
    return 0;
}
"""
        % (
            ints,
            floats,
            fold,
            pressure_expected(9),
            pressure_id,
            clobber_expected(9),
            pressure_id + 1,
        )
    )
    return "".join(out)


def gen_main(cases):
    externs = "".join(
        "@nomangle\npublic extern i32 abi_case%d();\n\n" % c.index for c in cases
    )
    calls = "".join(
        '    say(&out, "run ", %d);\n    failures = failures + check(&out, %d, abi_case%d());\n'
        % (c.index, c.index, c.index)
        for c in cases
    )
    return """module main;

import std::os::file;
import std::result;
import std::slice;

using std::os::{ file };
using std::{ slice };

%s@nomangle
public extern i32 abi_pressure();

i32 check(file::File* out, i32 index, i32 code) {
    if index < 0 {
        return 0;
    }
    if code == 0 {
        return 0;
    }
    say(out, "fail ", code);
    return 1;
}

void say(file::File* out, [] const u8 word, i32 code) {
    if !out.write_all(word).is_ok() {
        return;
    }
    u8[16] buf;
    usize n = 16;
    buf[15] = 10 as u8;
    n = 15;
    u32 v = code as u32;
    while true {
        n = n - 1;
        buf[n] = ((v %% 10) + 48) as u8;
        v = v / 10;
        if v == 0 {
            break;
        }
    }
    if !out.write_all(slice::from_raw(&buf[n], 16 - n)).is_ok() {
        return;
    }
}

public i32 main() {
    file::File out = file::stdout();
    i32 failures = 0;
%s    say(&out, "run ", %d);
    failures = failures + check(&out, %d, abi_pressure());
    return failures;
}
""" % (externs, calls, len(cases) + 1, len(cases) + 1)


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        raise RuntimeError(
            "command failed (%d): %s\n%s%s"
            % (
                r.returncode,
                " ".join(map(str, cmd)),
                r.stdout[-2000:],
                r.stderr[-4000:],
            )
        )
    return r


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("target", choices=["linux", "win"])
    parser.add_argument("--build", default=str(ROOT / "build"))
    parser.add_argument("--work", default=None)
    parser.add_argument(
        "--only", default=None, help="caller:callee pair to run, e.g. custom-O2:llvm-O0"
    )
    args = parser.parse_args()

    build = Path(args.build)
    dcc = Path(os.environ.get("DCC", build / "bin/dcc"))
    windows = args.target == "win"
    mingw = Path(os.environ.get("MINGW_SYSROOT", "/opt/llvm-mingw"))
    wine = os.environ.get("WINE") or "wine"
    cc = [str(mingw / "bin/x86_64-w64-mingw32-clang")] if windows else ["clang"]
    triple = "x86_64-coff" if windows else "x86_64-elf"
    os_name = "windows" if windows else "linux"
    timeout = (
        []
        if not os.environ.get("DCC_TIMEOUT")
        else ["timeout", os.environ["DCC_TIMEOUT"]]
    )

    cases = build_cases()
    legacy_cases = [
        c for c in cases if not is_based(c.ret) and not any(is_based(t) for t in c.args)
    ]
    based_case_count = len(cases) - len(legacy_cases)
    work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix="dcc-abi-"))
    work.mkdir(parents=True, exist_ok=True)
    legacy_work = work / "legacy"
    legacy_work.mkdir(parents=True, exist_ok=True)
    (work / "abi_callee.dc").write_text(gen_dc_callee(cases))
    (work / "abi_caller.dc").write_text(gen_dc_caller(cases))
    (work / "callee.c").write_text(gen_c_callee(cases))
    (work / "caller.c").write_text(gen_c_caller(cases))
    (work / "main-full.dc").write_text(gen_main(cases))
    (work / "main-legacy.dc").write_text(gen_main(legacy_cases))
    (legacy_work / "abi_callee.dc").write_text(gen_dc_callee(legacy_cases))
    (legacy_work / "abi_caller.dc").write_text(gen_dc_caller(legacy_cases))
    (legacy_work / "caller.c").write_text(gen_c_caller(legacy_cases))

    variants = ["llvm-O0", "llvm-O2", "custom-O0", "custom-O2", "c"]
    c_flags = ["-O2", "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-c"]
    objs = {}
    for side in ("caller", "callee"):
        for v in variants:
            subsets = ("legacy",) if v.startswith("custom") else ("full", "legacy") if side == "caller" else ("full",)
            for subset in subsets:
                source_dir = legacy_work if subset == "legacy" else work
                obj = work / ("%s-%s-%s.o" % (side, v, subset))
                if v == "c":
                    run(cc + c_flags + ["-o", str(obj), str(source_dir / ("%s.c" % side))])
                else:
                    backend, opt = v.split("-")
                    run(
                        timeout
                        + [
                            str(dcc),
                            "-target",
                            triple,
                            "-fbackend",
                            backend,
                            "-" + opt,
                            "-I",
                            str(source_dir),
                            "-c",
                            "-o",
                            str(obj),
                            str(source_dir / ("abi_%s.dc" % side)),
                        ]
                    )
                objs[(side, v, subset)] = obj
    main_objs = {}
    for subset in ("full", "legacy"):
        obj = work / ("main-%s.o" % subset)
        run(
            timeout
            + [
                str(dcc),
                "-flibdcext",
                os_name,
                "-target",
                triple,
                "-c",
                "-o",
                str(obj),
                str(work / ("main-%s.dc" % subset)),
            ]
        )
        main_objs[subset] = obj

    names = {c.index: c.describe() for c in cases}
    names[len(cases) + 1] = (
        "callee-saved pressure across a 512-byte aggregate copy and float-heavy callee"
    )
    names[len(cases) + 2] = "abi_clobber result"
    failures = 0
    pairs = [(a, b) for a in variants for b in variants]
    if args.only:
        pairs = [tuple(args.only.split(":"))]
    print(
        "cross-backend ABI matrix (%s, %d cases): rows are callers, columns callees"
        % ("Win64" if windows else "SysV", len(cases) + 2)
    )
    results = {}
    for caller, callee in pairs:
        subset = "legacy" if caller.startswith("custom") or callee.startswith("custom") else "full"
        exe = work / ("abi-%s-%s.exe" % (caller, callee))
        objects = [
            str(main_objs[subset]),
            str(objs[("caller", caller, subset)]),
            str(objs[("callee", callee, "legacy" if callee.startswith("custom") else "full")]),
        ]
        if windows:
            link = (
                cc
                + [
                    "-nostdlib",
                    "-Wl,--entry,_start",
                    "-Wl,--subsystem,console",
                    "-o",
                    str(exe),
                ]
                + objects
                + [
                    str(build / "lib/libdcext-windows-llvm.a"),
                    "-lkernel32",
                    "-lws2_32",
                    "-ladvapi32",
                    "-lshell32",
                ]
            )
            cmd = [wine, str(exe)]
        else:
            link = [
                str(dcc),
                "-flibdcext",
                "linux",
                "-target",
                "x86_64-elf",
                "-o",
                str(exe),
            ] + objects
            cmd = [str(exe)]
        try:
            run(link)
            r = subprocess.run(
                ["timeout", "60"] + cmd,
                capture_output=True,
                text=True,
                env=dict(os.environ, WINEDEBUG="-all"),
            )
            lines = [l.split() for l in r.stdout.splitlines()]
            failed = [
                int(l[1])
                for l in lines
                if len(l) == 2 and l[0] == "fail" and l[1].isdigit()
            ]
            ran = [
                int(l[1])
                for l in lines
                if len(l) == 2 and l[0] == "run" and l[1].isdigit()
            ]
            code = r.returncode
            if code != len(failed):
                failed.append(
                    "crash in case %s (exit %d)" % (ran[-1] if ran else "?", code)
                )
        except RuntimeError as error:
            print(error, file=sys.stderr)
            failed = ["build failure"]
        results[(caller, callee)] = failed
        if subset == "legacy":
            print(
                "EXCLUDED %s -> %s: %d based-pointer ABI cases (custom far/based lowering is deferred)"
                % (caller, callee, based_case_count)
            )
        if failed:
            failures += 1
    width = max(len(v) for v in variants) + 2
    print("".ljust(width) + "".join(v.ljust(width) for v in variants))
    for caller in variants:
        print(
            caller.ljust(width)
            + "".join(
                (
                    "ok"
                    if not results.get((caller, callee))
                    else "FAIL(%d)" % len(results[(caller, callee)])
                ).ljust(width)
                for callee in variants
            )
        )
    for (caller, callee), failed in sorted(results.items()):
        for item in failed:
            print(
                "FAIL %s -> %s: case %s: %s"
                % (caller, callee, item, names.get(item, ""))
            )
    if failures or args.work:
        print("work directory: %s" % work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    if failures:
        print("ABI MATRIX FAILED: %d of %d pairs" % (failures, len(pairs)))
        return 1
    print("ABI MATRIX OK: %d pairs" % len(pairs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
