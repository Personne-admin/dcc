import std;

#include "harness.hh"

#include <sys/wait.h>

namespace
{
    [[nodiscard]] std::string shell_quote(std::filesystem::path const& path)
    {
        return std::format("'{}'", path.string());
    }

    [[nodiscard]] int build_and_run(std::string_view source, std::string_view name)
    {
        std::error_code ec;
        auto const dcc = std::filesystem::weakly_canonical("/proc/self/exe", ec).parent_path().parent_path() / "dcc";
        auto const dir = std::filesystem::temp_directory_path() / "dcc-stdlib-containers" / name;
        std::filesystem::create_directories(dir, ec);
        auto const src = dir / "main.dc";
        auto const exe = dir / "program";
        {
            std::ofstream out{src};
            out << source;
        }

        auto const compile = std::format("{} -flibdcext -target x86_64-elf -o {} {}", shell_quote(dcc), shell_quote(exe), shell_quote(src));
        if (std::system(compile.c_str()) != 0)
            return -1;

        int const status = std::system(shell_quote(exe).c_str());
        std::filesystem::remove_all(dir, ec);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

} // namespace

SECTION("libdcext containers and allocators");

TEST_CASE("array push/pop/insert/remove and growth behave correctly")
{
    constexpr std::string_view source = R"(module main;
import std::array;
import std::mem;
import std::result;

public i32 main() {
    u8[512] backing;
    std::mem::FixedBuffer fb = std::mem::new_fixed_buffer(backing[0..512]);
    std::mem::Allocator a = fb.allocator();

    std::array::Array(i32) arr = std::array::new_array(a);
    if !arr.is_empty() || arr.len() != 0 { return 1; }
    if arr.at(0).is_some() || arr.last().is_some() || arr.pop().is_some() { return 2; }

    usize i = 0;
    while i < 20 {
        if !arr.push(i as i32).is_ok() { return 3; }
        i = i + 1;
    }
    if arr.len() != 20 { return 4; }
    [] i32 e0 = arr.items();
    if e0[0] != 0 || e0[19] != 19 { return 4; }
    std::result::Optional(i32*) a0 = arr.at(0);
    if a0.is_none() { return 4; }
    i32* a0p = a0.unwrap_some();
    if *a0p != 0 { return 4; }
    if arr.last().unwrap_some() != 19 { return 5; }

    if !arr.insert(0, -1).is_ok() { return 6; }
    if !arr.insert(10, -2).is_ok() { return 7; }
    if !arr.insert(arr.len(), -3).is_ok() { return 8; }
    if arr.len() != 23 { return 9; }
    [] i32 e1 = arr.items();
    if e1[0] != -1 || e1[10] != -2 || e1[22] != -3 { return 9; }
    std::result::Optional(i32*) a10 = arr.at(10);
    if a10.is_none() { return 10; }
    i32* a10p = a10.unwrap_some();
    if *a10p != -2 { return 10; }

    if arr.remove(0).unwrap_some() != -1 { return 11; }
    if arr.swap_remove(9).unwrap_some() != -2 { return 12; }
    if arr.len() != 21 { return 13; }

    if arr.pop().unwrap_some() != 19 { return 14; }
    if arr.len() != 20 { return 15; }

    i32[3] extra = {100, 101, 102};
    if !arr.append_slice(extra[0..3]).is_ok() { return 16; }
    if arr.len() != 23 || arr.last().unwrap_some() != 102 { return 17; }

    if !arr.resize(25).is_ok() { return 18; }
    if arr.len() != 25 { return 19; }
    if !arr.shrink_to_fit().is_ok() { return 20; }
    if arr.capacity() != arr.len() { return 21; }

    arr.clear();
    if !arr.is_empty() { return 22; }

    std::result::Result(std::array::Array(i32), std::mem::AllocError) cap_r = std::array::with_capacity(a, 16);
    if cap_r.is_err() { return 23; }
    std::array::Array(i32) cap = cap_r.unwrap();
    if cap.capacity() < 16 { cap.deinit(); return 24; }
    cap.deinit();

    i32[4] seed = {4, 3, 2, 1};
    std::result::Result(std::array::Array(i32), std::mem::AllocError) cp_r = std::array::from_slice(a, seed[0..4]);
    if cp_r.is_err() { return 25; }
    std::array::Array(i32) cp = cp_r.unwrap();
    [] i32 cpe = cp.items();
    if cp.len() != 4 || cpe[0] != 4 { cp.deinit(); return 26; }
    std::result::Result([] i32, std::mem::AllocError) own_r = cp.to_owned_slice();
    if own_r.is_err() { cp.deinit(); return 27; }
    [] i32 owned = own_r.unwrap();
    if owned.len != 4 || owned[3] != 1 { cp.deinit(); return 28; }
    cp.deinit();
    arr.deinit();
    return 0;
}
)";
    CHECK_EQ(build_and_run(source, "array"), 0);
}

TEST_CASE("list link/unlink/iterate/splice/reverse behave correctly")
{
    constexpr std::string_view source = R"(module main;
import std::list;
import std::mem;
import std::result;

public i32 main() {
    u8[1024] backing;
    std::mem::FixedBuffer fb = std::mem::new_fixed_buffer(backing[0..1024]);
    std::mem::Allocator a = fb.allocator();

    std::list::List(i32) l = std::list::new_list();
    if !l.is_empty() || l.len() != 0 { return 1; }
    if l.front().is_some() || l.back().is_some() { return 2; }
    if l.pop_front().is_some() || l.pop_back().is_some() { return 3; }

    if !l.push_back_alloc(&a, 1).is_ok() { return 4; }
    if !l.push_back_alloc(&a, 2).is_ok() { return 5; }
    if !l.push_front_alloc(&a, 0).is_ok() { return 6; }
    if l.len() != 3 { return 7; }
    std::result::Optional(i32*) f = l.front();
    std::result::Optional(i32*) b = l.back();
    if f.is_none() || b.is_none() { return 8; }
    i32* fp = f.unwrap_some();
    i32* bp = b.unwrap_some();
    if *fp != 0 || *bp != 2 { return 8; }

    std::list::Iterator(i32) it = l.iterate();
    i32 expect = 0;
    while true {
        std::result::Optional(std::list::Node(i32)*) n = it.next();
        if n.is_none() { break; }
        std::list::Node(i32)* np = n.unwrap_some();
        if (*np).value != expect { return 9; }
        expect = expect + 1;
    }
    if expect != 3 { return 10; }

    std::list::Iterator(i32) rit = l.iterate_reverse();
    expect = 2;
    while true {
        std::result::Optional(std::list::Node(i32)*) n = rit.previous();
        if n.is_none() { break; }
        std::list::Node(i32)* np = n.unwrap_some();
        if (*np).value != expect { return 11; }
        expect = expect - 1;
    }
    if expect != -1 { return 12; }

    std::result::Optional(std::list::Node(i32)*) pf = l.pop_front();
    if pf.is_none() { return 13; }
    std::list::Node(i32)* pfn = pf.unwrap_some();
    if (*pfn).value != 0 { return 13; }
    a.destroy_node(pfn);
    std::result::Optional(std::list::Node(i32)*) pb = l.pop_back();
    if pb.is_none() { return 14; }
    std::list::Node(i32)* pbn = pb.unwrap_some();
    if (*pbn).value != 2 { return 14; }
    a.destroy_node(pbn);
    std::result::Optional(i32*) only = l.front();
    if l.len() != 1 || only.is_none() { return 15; }
    i32* onlyp = only.unwrap_some();
    if *onlyp != 1 { return 15; }

    std::list::List(i32) other = std::list::new_list();
    if !other.push_back_alloc(&a, 10).is_ok() { return 16; }
    if !other.push_back_alloc(&a, 11).is_ok() { return 17; }
    l.splice(&other);
    if l.len() != 3 || !other.is_empty() { return 18; }

    l.reverse();
    std::result::Optional(i32*) rf = l.front();
    std::result::Optional(i32*) rb = l.back();
    if rf.is_none() || rb.is_none() { return 19; }
    i32* rfp = rf.unwrap_some();
    i32* rbp = rb.unwrap_some();
    if *rfp != 11 || *rbp != 1 { return 19; }

    l.deinit(&a);
    if !l.is_empty() { return 20; }
    return 0;
}
)";
    CHECK_EQ(build_and_run(source, "list"), 0);
}

TEST_CASE("map put/get/remove/iterate behave correctly")
{
    constexpr std::string_view source = R"(module main;
import std::map;
import std::mem;
import std::result;

public i32 main() {
    u8[8192] backing;
    std::mem::FixedBuffer fb = std::mem::new_fixed_buffer(backing[0..8192]);
    std::mem::Allocator a = fb.allocator();

    std::map::Map(u64, u64) m = std::map::new_map(a);
    if !m.is_empty() || m.len() != 0 { return 1; }
    if m.get(1).is_some() || m.contains(1) { return 2; }
    if m.remove(1).is_some() { return 3; }

    usize k = 1;
    while k <= 50 {
        if m.put(k as u64, (k as u64) * (10 as u64)).is_err() { return 4; }
        k = k + 1;
    }
    if m.len() != 50 { return 5; }

    std::result::Optional(u64) g = m.get(7);
    if g.is_none() || g.unwrap_some() != 70 { return 6; }
    if !m.contains(50) || m.contains(51) { return 7; }

    std::result::Result(std::result::Optional(u64), std::mem::AllocError) rep = m.put(7, 777);
    if rep.is_err() { return 8; }
    std::result::Optional(u64) prev = rep.unwrap();
    if prev.is_none() || prev.unwrap_some() != 70 { return 9; }
    std::result::Optional(u64) g2 = m.get(7);
    if g2.is_none() || g2.unwrap_some() != 777 { return 10; }
    if m.len() != 50 { return 11; }

    std::result::Result(u64*, std::mem::AllocError) gp = m.get_or_put(51, 510);
    if gp.is_err() { return 12; }
    u64* pp = gp.unwrap();
    if *pp != 510 { return 13; }
    *pp = 511;
    std::result::Optional(u64) g3 = m.get(51);
    if g3.is_none() || g3.unwrap_some() != 511 { return 14; }

    std::map::Iterator(u64, u64) it = m.iterate();
    u64 sum = 0;
    usize seen = 0;
    while true {
        std::result::Optional(std::map::Entry(u64, u64)) e = it.next();
        if e.is_none() { break; }
        std::map::Entry(u64, u64) ent = e.unwrap_some();
        sum = sum + *ent.value;
        seen = seen + 1;
    }
    if seen != 51 { return 15; }
    u64 want = (10 as u64) * ((50 as u64) * (51 as u64) / (2 as u64)) + (777 as u64) - (70 as u64) + (511 as u64);
    if sum != want { return 16; }

    std::result::Optional(u64) r = m.remove(7);
    if r.is_none() || r.unwrap_some() != 777 { return 17; }
    if m.len() != 50 || m.contains(7) { return 18; }

    m.clear();
    if !m.is_empty() || m.len() != 0 { return 19; }
    if m.put(1, 1).is_err() { m.deinit(); return 20; }
    if m.len() != 1 { m.deinit(); return 21; }
    m.deinit();
    return 0;
}
)";
    CHECK_EQ(build_and_run(source, "map"), 0);
}

TEST_CASE("stable sort with allocator and custom comparators behave correctly")
{
    constexpr std::string_view source = R"(module main;
import std::sort;
import std::mem;
import std::result;

i32 descending(const i32* a, const i32* b) {
    i32 x = *a;
    i32 y = *b;
    if x < y { return 1; }
    if x > y { return -1; }
    return 0;
}

public i32 main() {
    u8[1024] backing;
    std::mem::FixedBuffer fb = std::mem::new_fixed_buffer(backing[0..1024]);
    std::mem::Allocator a = fb.allocator();

    i32[8] values = {5, 1, 4, 1, 3, 2, 8, 0};
    if !std::sort::sort_stable_alloc(&a, values[0..8], descending).is_ok() { return 1; }
    if values[0] != 8 || values[7] != 0 { return 2; }
    if !std::sort::is_sorted_by(values[0..8], descending) { return 3; }

    std::sort::sort(values[0..8]);
    if !std::sort::is_sorted_by(values[0..8], std::sort::compare_default) { return 4; }
    if values[0] != 0 || values[7] != 8 { return 5; }

    i32 key4 = 4;
    if std::sort::lower_bound_by(values[0..8], &key4, std::sort::compare_default) != 5 { return 6; }
    if std::sort::upper_bound_by(values[0..8], &key4, std::sort::compare_default) != 6 { return 7; }
    std::result::Optional(usize) bs = std::sort::binary_search(values[0..8], 3);
    if bs.is_none() || bs.unwrap_some() != 4 { return 8; }
    if std::sort::binary_search(values[0..8], 9).is_some() { return 9; }
    return 0;
}
)";
    CHECK_EQ(build_and_run(source, "sort"), 0);
}

TEST_CASE("fixed-buffer and arena allocators behave correctly")
{
    constexpr std::string_view source = R"(module main;
import std::mem;
import std::result;

public i32 main() {
    u8[16384] backing;
    std::mem::FixedBuffer fb = std::mem::new_fixed_buffer(backing[0..16384]);
    std::mem::Allocator a = fb.allocator();

    std::result::Result([] u32, std::mem::AllocError) zr = a.alloc_zeroed(4);
    if zr.is_err() { return 1; }
    [] u32 z = zr.unwrap();
    if z.len != 4 || z[0] != 0 || z[3] != 0 { return 2; }
    z[1] = 11;
    a.free(z);

    std::result::Result(u64*, std::mem::AllocError) cr = a.create();
    if cr.is_err() { return 3; }
    u64* p = cr.unwrap();
    *p = 0xDEADBEEF as u64;
    if *p != 0xDEADBEEF as u64 { a.destroy(p); return 4; }
    a.destroy(p);

    u32[3] src = {7, 8, 9};
    std::result::Result([] u32, std::mem::AllocError) dr = a.dupe(src[0..3]);
    if dr.is_err() { return 5; }
    [] u32 d = dr.unwrap();
    if d.len != 3 || d[0] != 7 || d[2] != 9 { a.free(d); return 6; }
    a.free(d);

    std::result::Result([] u8, std::mem::AllocError) rr = a.raw_alloc(64, 8);
    if rr.is_err() { return 7; }
    [] u8 raw = rr.unwrap();
    if !a.raw_resize(raw, 32, 8) { a.raw_free(raw, 8); return 8; }
    a.raw_free(raw[0..32], 8);

    std::result::Result([] u32, std::mem::AllocError) oor = a.alloc(1000000);
    if oor.is_ok() { return 9; }
    if oor.unwrap_err() != std::mem::AllocError::OutOfMemory { return 10; }

    fb.reset();

    std::mem::Arena ar = std::mem::new_arena(a);
    std::mem::Allocator al = ar.allocator();
    std::result::Result([] u64, std::mem::AllocError) ar1 = al.alloc(100);
    if ar1.is_err() { ar.deinit(); return 11; }
    [] u64 block = ar1.unwrap();
    block[0] = 1;
    block[99] = 2;
    std::result::Result([] u64, std::mem::AllocError) ar2 = al.alloc(100);
    if ar2.is_err() { ar.deinit(); return 12; }
    [] u64 block2 = ar2.unwrap();
    if block[0] != 1 || block[99] != 2 { ar.deinit(); return 13; }
    block2[0] = 3;
    ar.deinit();
    return 0;
}
)";
    CHECK_EQ(build_and_run(source, "mem"), 0);
}
