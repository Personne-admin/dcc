import std;

#include "harness.hh"

#include <sys/wait.h>

namespace
{
    [[nodiscard]] std::string shell_quote(std::filesystem::path const& path)
    {
        return std::format("'{}'", path.string());
    }

    [[nodiscard]] int build_and_run(std::string_view source, std::string_view backend = "llvm", std::string_view optimization = "-O0")
    {
        std::error_code ec;
        auto const dcc = std::filesystem::weakly_canonical("/proc/self/exe", ec).parent_path().parent_path() / "dcc";
        auto const dir = std::filesystem::temp_directory_path() / "dcc-stdlib-modules";
        std::filesystem::create_directories(dir, ec);
        auto const src = dir / "main.dc";
        auto const exe = dir / "program";
        {
            std::ofstream out{src};
            out << source;
        }

        auto const compile = std::format("{} -flibdcext -target x86_64-elf -fbackend {} {} -o {} {}", shell_quote(dcc), backend, optimization, shell_quote(exe),
                                         shell_quote(src));
        if (std::system(compile.c_str()) != 0)
            return -1;

        int const status = std::system(shell_quote(exe).c_str());
        std::filesystem::remove_all(dir, ec);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

} // namespace

SECTION("libdcext standard modules");

TEST_CASE("result, slice, utf8, math, hash, sort, mem, and prelude behave correctly")
{
    constexpr std::string_view source = R"(module main;
import std::result;
import std::slice;
import std::utf8;
import std::math;
import std::hash;
import std::sort;
import std::mem;
import std::prelude;
import std::fmt;

bool same([] const u8 a, [] const u8 b) { return std::mem::equal(a, b); }

public i32 main() {
    std::result::Result(i32, u8) ok = std::result::Result::Ok(7);
    std::result::Result(i32, u8) err = std::result::Result::Err(3);
    std::result::Status(u8) status_ok = std::result::Status::Ok;
    std::result::Status(u8) status_err = std::result::Status::Err(9);
    std::result::Optional(i32) some = std::result::Optional::Some(4);
    std::result::Optional(i32) none = std::result::Optional::None;
    if !ok.is_ok() || ok.is_err() || ok.unwrap() != 7 || ok.unwrap_or(1) != 7 { return 1; }
    if !err.is_err() || err.is_ok() || err.unwrap_err() != 3 || err.unwrap_or(8) != 8 { return 2; }
    if !status_ok.is_ok() || status_err.is_ok() || status_err.unwrap_err() != 9 { return 3; }
    if !some.is_some() || some.is_none() || some.unwrap_some() != 4 || some.some_or(1) != 4 { return 4; }
    if !none.is_none() || none.is_some() || none.some_or(6) != 6 { return 5; }

    u8[5] data = {1, 2, 3, 2, 1};
    [] u8 view = std::slice::from_raw(&data[0], 5);
    [] const u8 cview = std::slice::from_raw_const(&data[0], 5);
    if !std::slice::is_empty(cview[0..0]) { return 6; }
    if view.first().unwrap_some() != 1 || cview.last().unwrap_some() != 1 || view.at(2).unwrap_some() != &data[2] { return 7; }
    u8[3] middle = {2, 3, 2}; u8[2] prefix = {1, 2}; u8[2] suffix = {2, 1}; u8[2] pattern = {3, 2};
    if !std::slice::eq(cview[1..4], middle[0..3]) || !cview.starts_with(prefix[0..2]) || !cview.ends_with(suffix[0..2]) { return 8; }
    if cview.index_of(2).unwrap_some() != 1 || cview.last_index_of(2).unwrap_some() != 3 || cview.find(pattern[0..2]).unwrap_some() != 2 { return 9; }
    view.rotate_left(2);
    if view[0] != 3 || view[4] != 2 { return 10; }
    view.reverse();
    if view[0] != 2 || view[4] != 3 { return 11; }

    u8[1] ascii = {65}; u8[2] two = {0xC3, 0xA9}; u8[3] three = {0xE2, 0x82, 0xAC}; u8[4] four = {0xF0, 0x9F, 0x98, 0x80};
    u8[1] bad_start = {0x80}; u8[2] bad_cont = {0xC2, 65}; u8[3] overlong = {0xE0, 0x80, 0x80}; u8[3] surrogate = {0xED, 0xA0, 0x80}; u8[4] out_of_range = {0xF4, 0x90, 0x80, 0x80}; u8[2] truncated = {0xE2, 0x82};
    if std::utf8::decode(ascii[0..1]).unwrap().codepoint != 65 || std::utf8::decode(two[0..2]).unwrap().codepoint != 233 { return 12; }
    if std::utf8::decode(three[0..3]).unwrap().codepoint != 8364 || std::utf8::decode(four[0..4]).unwrap().codepoint != 128512 { return 13; }
    if std::utf8::decode(bad_start[0..1]).is_ok() || std::utf8::decode(bad_cont[0..2]).is_ok() || std::utf8::decode(overlong[0..3]).is_ok() { return 14; }
    if std::utf8::decode(surrogate[0..3]).is_ok() || std::utf8::decode(out_of_range[0..4]).is_ok() || std::utf8::decode(truncated[0..2]).is_ok() { return 15; }
    u8[4] encoded;
    if std::utf8::encode(0x1F600, encoded[0..4]).unwrap() != 4 || !std::utf8::is_valid(encoded[0..4]) || std::utf8::count_codepoints(encoded[0..4]) != 1 { return 16; }
    if std::utf8::validate(bad_start[0..1]).is_none() || std::utf8::boundary_before(two[0..2], 1) != 0 { return 17; }

    if std::math::min(3, 4) != 3 || std::math::max(3, 4) != 4 || std::math::clamp(9, 1, 5) != 5 { return 18; }
    if std::math::floor(-1.2) != -2.0 || std::math::ceil(1.2) != 2.0 || std::math::fabs(-3.0) != 3.0 { return 19; }
    if std::math::is_nan(1.0) || std::math::is_inf(1.0) || !std::math::is_finite(1.0) || std::math::gcd(12, 18) != 6 || std::math::ipow(3, 4) != 81 { return 20; }
    if std::math::fabs(std::math::sqrt(9.0) - 3.0) > 0.00001 || std::math::fabs(std::math::log(std::math::E) - 1.0) > 0.001 { return 21; }

    if std::hash::fnv1a_64("abc") != std::hash::fnv1a_64("abc") || std::hash::fnv1a_64("abc") == std::hash::fnv1a_64("abd") { return 22; }
    u64 a = 42; u64 b = 42; if a.hash_of() != b.hash_of() || !std::hash::key_eq(&a, &b) { return 23; }
    i32[6] values = {5, 1, 4, 1, 3, 2};
    std::sort::sort(values[0..6]);
    if values[0] != 1 || values[1] != 1 || values[2] != 2 || values[3] != 3 || values[4] != 4 || values[5] != 5 { return 24; }
    if std::sort::binary_search(values[0..6], 4).unwrap_some() != 4 || std::sort::binary_search(values[0..6], 9).is_some() { return 25; }
    std::mem::set(data[0..5], 7 as u8); std::mem::zero(data[1..4]);
    if data[0] != 7 || data[1] != 0 || data[4] != 7 { return 26; }
    u8[64] formatted;
    [] u8 rendered = std::fmt::format_buf(formatted[0..64], "{} {:x} {:04}", 7, 48879 as u16, 3).unwrap();
    if !same(rendered, "7 beef 0003") || std::fmt::format_len("{}", true).unwrap() != 4 { return 27; }
    std::prelude::Result(i32, u8) prelude_smoke = std::prelude::Result::Ok(1);
    return if prelude_smoke.unwrap() == 1 { 0 } else { 28 };
}
)";
    CHECK_EQ(build_and_run(source), 0);
}

TEST_CASE("slice helpers, format_alloc, and debug assert behave correctly")
{
    constexpr std::string_view source = R"(module main;
import std::slice;
import std::fmt;
import std::mem;
import std::debug;
import std::result;

bool is_even(const u8* p) {
    u8 v = *p;
    return (v / 2) * 2 == v;
}

public i32 main() {
    u8[6] data = {1, 2, 3, 4, 5, 6};
    [] u8 view = data[0..6];
    if !std::slice::contains(view, 3 as u8) || std::slice::contains(view, 9 as u8) { return 1; }
    if std::slice::count(view, 2 as u8) != 1 { return 2; }
    if std::slice::all(view, is_even) || !std::slice::any(view, is_even) { return 3; }
    std::result::Optional(usize) w = std::slice::index_where(view, is_even);
    if w.is_none() || w.unwrap_some() != 1 { return 4; }
    std::result::Optional(usize) mn = std::slice::index_of_min(view);
    std::result::Optional(usize) mx = std::slice::index_of_max(view);
    if mn.is_none() || mn.unwrap_some() != 0 || mx.is_none() || mx.unwrap_some() != 5 { return 5; }

    std::slice::fill(view[0..3], 0 as u8);
    if view[0] != 0 || view[2] != 0 || view[3] != 4 { return 6; }

    u8[5] csv = {97, 44, 98, 44, 99};
    std::slice::Split(u8) sp = std::slice::split(csv[0..5], 44 as u8);
    usize parts = 0;
    usize total = 0;
    while true {
        std::result::Optional([] const u8) piece = sp.next();
        if piece.is_none() { break; }
        [] const u8 seg = piece.unwrap_some();
        total = total + seg.len;
        parts = parts + 1;
    }
    if parts != 3 || total != 3 { return 7; }

    u8[256] backing;
    std::mem::FixedBuffer fb = std::mem::new_fixed_buffer(backing[0..256]);
    std::mem::Allocator a = fb.allocator();
    std::result::Result([] u8, std::mem::AllocError) fr = std::fmt::format_alloc(&a, "{}-{:x}", 42, 255 as u8);
    if fr.is_err() { return 8; }
    [] u8 text = fr.unwrap();
    if text.len != 5 || text[0] != 52 || text[2] != 45 || text[4] != 102 { a.free(text); return 9; }
    a.free(text);

    std::debug::assert(true, "unreachable");
    std::debug::assert(view[3] == 4, "indexing works");
    return 0;
}
)";
    CHECK_EQ(build_and_run(source), 0);
}

TEST_CASE("fmt format_buf correctly formats bool, int, float, and string")
{
    constexpr std::string_view source = R"(module main;
import std::fmt;

bool same([] const u8 a, [] const u8 b) {
    if a.len != b.len { return false; }
    for usize i = 0; i < a.len; i++ {
        if a[i] != b[i] { return false; }
    }
    return true;
}

public i32 main() {
    u8[128] buf;
    [] u8 r_bool = std::fmt::format_buf(buf[0..128], "{} {}", true, false).unwrap();
    if !same(r_bool, "true false") { return 1; }

    [] u8 r_mixed = std::fmt::format_buf(buf[0..128], "bool: {} int: {} float: {:.1} str: {}", true, -42 as i32, 1.5 as f64, "hello" as []const u8).unwrap();
    if !same(r_mixed, "bool: true int: -42 float: 1.5 str: hello") { return 2; }

    return 0;
}
)";
    CHECK_EQ(build_and_run(source), 0);
}

TEST_CASE("tuple constructs through Tuple::make and reads elements back")
{
    constexpr std::string_view source = R"(module main;
import std::tuple;

public i32 main() {
    std::tuple::Tuple(i32, f64) t = std::tuple::Tuple::make(1, 2.0);
    if t.items.0 != 1 { return 1; }
    if t.items.1 != 2.0 { return 2; }
    std::tuple::Tuple(i32) s = std::tuple::Tuple::make(5);
    if s.items.0 != 5 { return 3; }
    std::tuple::Tuple() e = std::tuple::Tuple::make();
    if sizeof(std::tuple::Tuple()) != 0 { return 4; }
    return 0;
}
)";
    CHECK_EQ(build_and_run(source), 0);
}

TEST_CASE("variadic bridge survives void* round trip and heap allocation")
{
    constexpr std::string_view source = R"(module main;
import std::mem;
import std::os::heap;
import std::result;

struct Bridge(F, T...) {
    F entry;
    T... args;
}

using B2 = Bridge(void(*)(i32, f64), i32, f64);
using B0 = Bridge(void(*)());

void* pass_through(void* p) {
    return p;
}

i32 sunk0;
f64 sunk1;
i32 sunk2;

void target2(i32 x, f64 y) {
    sunk0 = x;
    sunk1 = y;
}

void target0() {
    sunk2 = 42;
}

public i32 main() {
    std::mem::Allocator alloc = std::os::heap::allocator();
    void(*)(i32, f64) fp2 = target2 as void(*)(i32, f64);
    void(*)() fp0 = target0 as void(*)();
    std::result::Result(B2*, std::mem::AllocError) r2 = std::mem::create!B2(&alloc);
    if r2.is_err() { return 10; }
    B2* b2 = r2.unwrap();
    b2.entry = fp2;
    b2.args.0 = 1;
    b2.args.1 = 2.0;
    void* raw = pass_through(b2 as void*);
    B2* back = raw as B2*;
    void(*)(i32, f64) e2 = back.entry;
    e2(back.args.0, back.args.1);
    if sunk0 != 1 { return 1; }
    if sunk1 != 2.0 { return 2; }
    std::result::Result(B0*, std::mem::AllocError) r0 = std::mem::create!B0(&alloc);
    if r0.is_err() { return 11; }
    B0* b0 = r0.unwrap();
    b0.entry = fp0;
    void(*)() e0 = b0.entry;
    e0();
    if sunk2 != 42 { return 3; }
    std::mem::destroy(&alloc, b2);
    std::mem::destroy(&alloc, b0);
    return 0;
}
)";
    CHECK_EQ(build_and_run(source), 0);
}

TEST_CASE("os::dir iteration executes on both backends at O0 and O2")
{
    static constexpr std::string_view source = (R"DCC(module main;
import std::os::dir;
import std::os::file;
import std::os::error;
import std::result;
import std::mem;

using std::result;
using std::os::dir;
using std::os::error;

struct DirTally {
    u64 count;
    u64 name_bytes;
    bool saw_subdir;
}

DirTally tally_step(DirTally acc, dir::Entry* e) {
    acc.count = acc.count + 1;
    acc.name_bytes = acc.name_bytes + (e.name.len as u64);
    if e.kind == dir::EntryKind::Directory {
        acc.saw_subdir = true;
    }
    return acc;
}

T drain(T)(dir::Iterator* it, T seed, T(*)(T, dir::Entry*) step) {
    T acc = seed;
    bool done = false;
    while !done {
        result::Optional(dir::Entry) oe = it.next();
        if oe.is_none() {
            done = true;
        } else {
            dir::Entry entry = oe.unwrap_some();
            acc = step(acc, &entry);
        }
    }
    return acc;
}

bool same([] const u8 a, [] const u8 b) {
    return std::mem::equal(a, b);
}

public i32 main() {
    if !std::os::file::make_directory("dmod-dir").is_ok() {
        return 1;
    }
    std::os::file::File f = std::os::file::create("dmod-dir/one").unwrap();
    f.write_all("hey").unwrap();
    f.close();
    if !std::os::file::make_directory("dmod-dir/two").is_ok() {
        return 2;
    }
    dir::Iterator it;
    if !dir::open("dmod-dir", &it).is_ok() {
        return 3;
    }
    DirTally t0;
    t0.count = 0;
    t0.name_bytes = 0;
    t0.saw_subdir = false;
    DirTally t = drain!DirTally(&it, t0, tally_step);
    if t.count != 4 {
        return 4;
    }
    if !t.saw_subdir {
        return 5;
    }
    if !it.rewind().is_ok() {
        return 6;
    }
    bool saw_one = false;
    bool saw_two = false;
    bool done = false;
    while !done {
        result::Optional(dir::Entry) oe = it.next();
        if oe.is_none() {
            done = true;
        } else {
            dir::Entry entry = oe.unwrap_some();
            if same(entry.name, "one") {
                saw_one = true;
                if entry.kind != dir::EntryKind::File {
                    return 7;
                }
                if entry.id == 0 {
                    return 8;
                }
                result::Result(dir::Metadata, error::Error) m = dir::stat_at(&it, &entry);
                if m.unwrap().size != 3 {
                    return 9;
                }
            }
            if same(entry.name, "two") {
                saw_two = true;
                if entry.kind != dir::EntryKind::Directory {
                    return 10;
                }
            }
        }
    }
    if !saw_one || !saw_two {
        return 11;
    }
    it.close();
    if !std::os::file::delete_file("dmod-dir/one").is_ok() {
        return 12;
    }
    if !std::os::file::remove_directory("dmod-dir/two").is_ok() {
        return 13;
    }
    if !std::os::file::remove_directory("dmod-dir").is_ok() {
        return 14;
    }
    dir::Iterator bad;
    if dir::open("dmod-dir", &bad).unwrap_err() != error::Error::NotFound {
        return 15;
    }
    return 0;
}
)DCC");
    for (auto optimization : {"-O0", "-O2"})
        CHECK_EQ(build_and_run(source, "llvm", optimization), 0);
}

TEST_CASE("os::thread spawn, join, and sync execute on llvm at O0 and O2")
{
    static constexpr std::string_view source = (R"DCC(module main;
import std::os::thread;
import std::os::error;

using std::os::thread;
using std::os::error;

thread::Mutex mu;
volatile i32 counter;

void bump() {
    for i32 i = 0; i < 1000; i++ {
        mu.lock();
        counter = counter + 1;
        mu.unlock();
    }
}

void bump_by(i32 n) {
    for i32 i = 0; i < n; i++ {
        mu.lock();
        counter = counter + 10;
        mu.unlock();
    }
}

struct Pair {
    i32 a;
    i32 b;
    bool seen;
}

void use_pair(Pair* p, i32 x, i32 y) {
    mu.lock();
    p.a = x;
    p.b = y;
    p.seen = true;
    counter = counter + 100;
    mu.unlock();
}

thread::Mutex cv_mu;
thread::Condvar cv;
volatile i32 ready;

void worker() {
    cv_mu.lock();
    ready = ready + 1;
    cv_mu.unlock();
    cv.signal();
}

thread::Once flag;
volatile i32 onces;

void init_fn() {
    onces = onces + 1;
}

void do_once() {
    flag.call_once(init_fn);
}

thread::RwLock rw;
volatile i32 readers_ok;

void read_it(i32 v) {
    rw.read_lock();
    i32 x = readers_ok;
    rw.read_unlock();
    mu.lock();
    readers_ok = x + v;
    mu.unlock();
}

thread::Semaphore sem;
volatile i32 sem_done;

void sem_worker() {
    sem.wait();
    sem_done = sem_done + 1;
}

thread::TlsKey* tls_key;
volatile i32 tls_ok;

void tls_user(usize v) {
    if !(tls_key.set(v as void*).is_ok()) {
        return;
    }
    thread::sleep_ms(10);
    void* back = tls_key.get();
    if (back as usize) == v {
        tls_ok = tls_ok + 1;
    }
}

public i32 main() {
    counter = 0;
    if thread::hardware_concurrency() == 0 {
        return 1;
    }
    thread::ThreadHandle me = thread::current();
    if (me as usize) == 0 {
        return 2;
    }
    if !thread::set_name("tmod").is_ok() {
        return 3;
    }
    thread::Thread[8] ts;
    ts[0] = thread::spawn(bump).unwrap();
    ts[1] = thread::spawn(bump_by, 100).unwrap();
    Pair p;
    p.a = 0;
    p.b = 0;
    p.seen = false;
    ts[2] = thread::spawn(use_pair, &p, 3, 4).unwrap();
    i32 st = 0;
    for i32 i = 0; i < 3; i++ {
        if !ts[i].join().is_ok() {
            st = 4;
        }
    }
    if st != 0 {
        return st;
    }
    if counter != 1000 + 1000 + 100 {
        return 6;
    }
    if !p.seen || p.a != 3 || p.b != 4 {
        return 7;
    }
    if ts[0].join().unwrap_err() != error::Error::BadHandle {
        return 8;
    }
    thread::Thread td = thread::spawn(bump).unwrap();
    if !td.detach().is_ok() {
        return 9;
    }
    thread::sleep_ms(100);
    ready = 0;
    thread::Thread[8] ws;
    ws[0] = thread::spawn(worker).unwrap();
    ws[1] = thread::spawn(worker).unwrap();
    cv_mu.lock();
    while ready < 2 {
        cv.wait(&cv_mu);
    }
    cv_mu.unlock();
    st = 0;
    for i32 i = 0; i < 2; i++ {
        if !ws[i].join().is_ok() {
            st = 10;
        }
    }
    if st != 0 {
        return st;
    }
    if ready != 2 {
        return 11;
    }
    onces = 0;
    thread::Thread[8] os;
    os[0] = thread::spawn(do_once).unwrap();
    os[1] = thread::spawn(do_once).unwrap();
    os[2] = thread::spawn(do_once).unwrap();
    st = 0;
    for i32 i = 0; i < 3; i++ {
        if !os[i].join().is_ok() {
            st = 12;
        }
    }
    if st != 0 {
        return st;
    }
    if onces != 1 {
        return 13;
    }
    readers_ok = 0;
    thread::Thread[8] rs;
    rs[0] = thread::spawn(read_it, 1).unwrap();
    rs[1] = thread::spawn(read_it, 1).unwrap();
    st = 0;
    for i32 i = 0; i < 2; i++ {
        if !rs[i].join().is_ok() {
            st = 14;
        }
    }
    if st != 0 {
        return st;
    }
    if readers_ok != 2 {
        return 15;
    }
    if !mu.try_lock() {
        return 16;
    }
    mu.unlock();
    sem_done = 0;
    sem.init(0);
    thread::Thread[8] ss;
    ss[0] = thread::spawn(sem_worker).unwrap();
    ss[1] = thread::spawn(sem_worker).unwrap();
    thread::sleep_ms(30);
    sem.post();
    sem.post();
    st = 0;
    for i32 i = 0; i < 2; i++ {
        if !ss[i].join().is_ok() {
            st = 17;
        }
    }
    if st != 0 {
        return st;
    }
    if sem_done != 2 {
        return 18;
    }
    if sem.try_wait() {
        return 19;
    }
    thread::TlsKey k = thread::create().unwrap();
    tls_key = &k;
    tls_ok = 0;
    if (k.get() as usize) != 0 {
        return 20;
    }
    thread::Thread[8] tls_ts;
    tls_ts[0] = thread::spawn(tls_user, 700 as usize).unwrap();
    tls_ts[1] = thread::spawn(tls_user, 800 as usize).unwrap();
    st = 0;
    for i32 i = 0; i < 2; i++ {
        if !tls_ts[i].join().is_ok() {
            st = 21;
        }
    }
    if st != 0 {
        return st;
    }
    if tls_ok != 2 {
        return 22;
    }
    if (k.get() as usize) != 0 {
        return 23;
    }
    k.destroy();
    return 0;
}
)DCC");
    for (auto optimization : {"-O0", "-O2"})
        CHECK_EQ(build_and_run(source, "llvm", optimization), 0);
}

TEST_CASE("os::pipe transfer, EOF, and errors execute on llvm at O0 and O2")
{
    static constexpr std::string_view source = (R"DCC(module main;
import std::os::pipe;
import std::os::thread;
import std::os::error;
import std::slice;
import std::mem;

using std::os::pipe;
using std::os::thread;
using std::os::error;
using std::slice;
using std::mem;

struct Msg {
    u32 id;
    u64 stamp;
    bool flag;
}

void send_msg(pipe::PipeWriter* w, Msg* m) {
    u8* raw = m as u8*;
    [] u8 bytes = slice::from_raw(raw, sizeof(Msg));
    if !w.write_all(bytes).is_ok() {
        return;
    }
}

pipe::Pipe* big_pipe;

void big_writer() {
    u8[4096] chunk;
    for usize i = 0; i < 4096; i++ {
        chunk[i] = (i & 255) as u8;
    }
    [] u8 view = slice::from_raw(&chunk[0], 4096);
    bool ok = true;
    for i32 k = 0; k < 32; k++ {
        if ok {
            if !big_pipe.writer.write_all(view).is_ok() {
                ok = false;
            }
        }
    }
    big_pipe.writer.close();
}

public i32 main() {
    if !pipe::ignore_sigpipe().is_ok() {
        return 1;
    }
    pipe::Pipe p = pipe::create().unwrap();
    u8[8] hello;
    hello[0] = 'h' as u8;
    hello[1] = 'e' as u8;
    hello[2] = 'l' as u8;
    hello[3] = 'l' as u8;
    hello[4] = 'o' as u8;
    hello[5] = '!' as u8;
    [] u8 out = slice::from_raw(&hello[0], 6);
    if !p.writer.write_all(out).is_ok() {
        return 2;
    }
    u8[8] back;
    [] u8 six = slice::from_raw(&back[0], 6);
    if p.reader.read_all(six).unwrap() != 6 as usize {
        return 3;
    }
    if !mem::equal(out, six) {
        return 4;
    }
    p.writer.close();
    p.writer.close();
    [] u8 rest = slice::from_raw(&back[0], 8);
    if p.reader.read(rest).unwrap() != 0 as usize {
        return 5;
    }
    if p.reader.read_all(rest).unwrap() != 0 as usize {
        return 6;
    }
    p.reader.close();
    p.reader.close();
    if p.writer.write(out).unwrap_err() != error::Error::BadHandle {
        return 7;
    }
    Msg m;
    m.id = 42;
    m.stamp = 123456789;
    m.flag = true;
    Msg got;
    got.id = 0;
    got.stamp = 0;
    got.flag = false;
    pipe::Pipe q = pipe::create().unwrap();
    thread::Thread t = thread::spawn(send_msg, &q.writer, &m).unwrap();
    u8* graw = &got as u8*;
    [] u8 gbytes = slice::from_raw(graw, sizeof(Msg));
    if q.reader.read_all(gbytes).unwrap() != sizeof(Msg) {
        return 8;
    }
    if !t.join().is_ok() {
        return 9;
    }
    if got.id != 42 || got.stamp != 123456789 || !got.flag {
        return 10;
    }
    q.reader.close();
    q.writer.close();
    pipe::Pipe bp = pipe::create().unwrap();
    big_pipe = &bp;
    thread::Thread bt = thread::spawn(big_writer).unwrap();
    u8[4096] rchunk;
    [] u8 rview = slice::from_raw(&rchunk[0], 4096);
    u64 total = 0;
    u64 checksum = 0;
    while true {
        usize n = bp.reader.read(rview).unwrap();
        if n == 0 as usize {
            break;
        }
        total = total + (n as u64);
        for usize i = 0; i < n; i++ {
            checksum = checksum + (rchunk[i] as u64);
        }
    }
    if !bt.join().is_ok() {
        return 11;
    }
    if total != 131072 as u64 {
        return 12;
    }
    if checksum != 16711680 as u64 {
        return 13;
    }
    bp.reader.close();
    pipe::Pipe cp = pipe::create().unwrap();
    cp.reader.close();
    if cp.writer.write(out).unwrap_err() != error::Error::BrokenPipe {
        return 14;
    }
    if cp.writer.write_all(out).unwrap_err() != error::Error::BrokenPipe {
        return 15;
    }
    cp.writer.close();
    u8[1] one;
    [] u8 oneview = slice::from_raw(&one[0], 1);
    pipe::Pipe ep = pipe::create().unwrap();
    if ep.reader.read(oneview[0..0]).unwrap() != 0 as usize {
        return 16;
    }
    if ep.writer.write(oneview[0..0]).unwrap() != 0 as usize {
        return 17;
    }
    ep.reader.close();
    ep.writer.close();
    return 0;
}
)DCC");
    for (auto optimization : {"-O0", "-O2"})
        CHECK_EQ(build_and_run(source, "llvm", optimization), 0);
}

TEST_CASE("os::random fills buffers from OS entropy on llvm at O0 and O2")
{
    static constexpr std::string_view source = (R"DCC(module main;
import core;
import core::target;
import std::slice;
import std::mem;
import std::os::random;
import std::os::error;
import std::result;
import std::sys::linux::abi;
import std::sys::win::abi;

using std::os::{ random, error };
using std::{ mem, slice, result };
using error::Error;

struct Blob {
    u64 a;
    u32 b;
    u8 tag;
}

Status(Error) fill_any(T)(T* value) {
    [] u8 bytes = mem::as_bytes_mut(value);
    return random::fill(bytes);
}

public i32 main() {
    u8[1] one;
    if !random::fill(one[0..0]).is_ok() {
        return 1;
    }
    u8[1] tiny;
    u8[7] odd;
    u8[1000] big;
    if !random::fill(tiny[0..1]).is_ok() {
        return 2;
    }
    if !random::fill(odd[0..7]).is_ok() {
        return 3;
    }
    if !random::fill(big[0..1000]).is_ok() {
        return 4;
    }
    u8[64] a;
    u8[64] b;
    if !random::fill(a[0..64]).is_ok() {
        return 5;
    }
    if !random::fill(b[0..64]).is_ok() {
        return 6;
    }
    if mem::equal(a[0..64], b[0..64]) {
        return 7;
    }
    Blob x;
    Blob y;
    if !fill_any(&x).is_ok() {
        return 8;
    }
    if !fill_any(&y).is_ok() {
        return 9;
    }
    if mem::equal(mem::as_bytes(&x), mem::as_bytes(&y)) {
        return 10;
    }
    static if core::target::OS == core::target::Os::Linux {
        if error::from_errno(std::sys::linux::abi::EINTR) != Error::Interrupted {
            return 11;
        }
        [] u8 bad = slice::from_raw(null as u8*, 16);
        if random::fill(bad).unwrap_err() != Error::InvalidArgument {
            return 12;
        }
    } else static if core::target::OS == core::target::Os::Windows {
        if error::from_win32(std::sys::win::abi::WSAEINTR) != Error::Interrupted {
            return 11;
        }
    }
    return 0;
}
)DCC");
    for (auto optimization : {"-O0", "-O2"})
        CHECK_EQ(build_and_run(source, "llvm", optimization), 0);
}

TEST_CASE("implicit function pointer pack deduction executes on both backends at O0 and O2")
{
    auto fixture = std::filesystem::path{"cases/em64t/fnptr-pack-deduction-exec.dcc-test"};
    if (!std::filesystem::exists(fixture))
        fixture = std::filesystem::path{"tests"} / fixture;
    std::ifstream input{fixture};
    REQUIRE(input.good());
    std::string contents{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto start = contents.find('\n') + 1;
    auto end = contents.find("=== EXPECT-");
    auto source = contents.substr(start, end - start);
    auto entry = source.find("@nomangle\npublic i32 dcc_main()");
    REQUIRE(entry != std::string::npos);
    source.replace(entry, std::string_view{"@nomangle\npublic i32 dcc_main()"}.size(), "public i32 main()");
    auto mod = source.find("module test;");
    if (mod != std::string::npos)
        source.replace(mod, std::string_view{"module test;"}.size(), "module main;");
    for (auto backend : {"llvm", "em64t"})
        for (auto optimization : {"-O0", "-O2"})
            CHECK_EQ(build_and_run(source, backend, optimization), 0);
}

TEST_CASE("constants materialized per block execute on both backends at O0, O1 and O2")
{
    auto fixture = std::filesystem::path{"cases/em64t/const-cross-block-exec.dcc-test"};
    if (!std::filesystem::exists(fixture))
        fixture = std::filesystem::path{"tests"} / fixture;
    std::ifstream input{fixture};
    REQUIRE(input.good());
    std::string contents{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto start = contents.find('\n') + 1;
    auto end = contents.find("=== EXPECT-");
    auto source = contents.substr(start, end - start);
    auto entry = source.find("@nomangle\npublic i32 dcc_main()");
    REQUIRE(entry != std::string::npos);
    source.replace(entry, std::string_view{"@nomangle\npublic i32 dcc_main()"}.size(), "public i32 main()");
    auto mod = source.find("module test;");
    if (mod != std::string::npos)
        source.replace(mod, std::string_view{"module test;"}.size(), "module main;");
    for (auto backend : {"llvm", "em64t"})
        for (auto optimization : {"-O0", "-O1", "-O2"})
            CHECK_EQ(build_and_run(source, backend, optimization), 0);
}
