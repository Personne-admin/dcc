def generate(out):
    out.mkdir(parents=True, exist_ok=True)
    for n in [64, 256, 1024]:
        source = 'module main;\n'
        for i in range(n):
            source += f'''i64 work{i}(i64 x) {{
    i64 sum = 0;
    for i64 j = 0; j < 16; j++ {{
        if (j % 2) == 0 {{ sum += x + j; }} else {{ sum += x - j; }}
    }}
    return sum;
}}
'''

        source += 'public i32 main() {\n    i64 sum = 0;\n'
        for i in range(n):
            source += f'    sum += work{i}({i});\n'

        source += f'    return if sum == {16*n*(n-1)//2-8*n} {{ 0 }} else {{ 1 }};\n}}\n'
        (out/f'compile_large_{n}.dc').write_text(source)

    for n in [8, 32, 128]:
        directory = out/f'imports_{n}'
        directory.mkdir(exist_ok=True)
        for i in range(n):
            source = f'module level{i};\n'
            if i+1 < n:
                source += f'import level{i+1};\npublic i64 value{i}(T)(T x) {{ return level{i+1}::value{i+1}(x) + 1; }}\n'
            else:
                source += f'public i64 value{i}(T)(T x) {{ return x + 1; }}\n'

            (directory/f'level{i}.dc').write_text(source)

        (directory/'main.dc').write_text(f'module main;\nimport level0;\npublic i32 main() {{ return if level0::value0(1 as i64) == {n+1} {{ 0 }} else {{ 1 }}; }}\n')

    for n in [16, 64, 256]:
        source = '''module main;
import std::sort;
i64 algorithm(T)(T x) {
    i64 sum = 0;
    for i64 i = 0; i < 16; i++ { sum += (x.value as i64) + i; }
    return sum;
}
'''
        for i in range(n):
            source += f'struct Item{i} {{ i64 value; }}\n'

        source += 'public i32 main() {\n    i64 sum = 0;\n'
        for i in range(n):
            source += f'    Item{i} v{i} = {{ value = {i} }};\n    sum += algorithm(v{i});\n'

        source += f'    return if sum == {16*n*(n-1)//2+120*n} {{ 0 }} else {{ 1 }};\n}}\n'
        (out/f'compile_generic_{n}.dc').write_text(source)

    for n in [16, 64, 256]:
        source = 'module main;\ni64 workload(i64 x) {\n    i64 sum = 0;\n'
        for i in range(n):
            source += f'    if (x + {i}) % 3 == 0 {{ sum += x + {i}; }} else {{ sum -= x + {i}; }}\n'

        source += '    return sum;\n}\n'
        expected = sum((7+i) if (7+i)%3 == 0 else -(7+i) for i in range(n))
        source += f'public i32 main() {{ return if workload(7) == ({expected} as i64) {{ 0 }} else {{ 1 }}; }}\n'
        (out/f'compile_cfg_{n}.dc').write_text(source)

    return sorted(out.glob('*.dc')) + sorted(out.glob('imports_*/main.dc'))
