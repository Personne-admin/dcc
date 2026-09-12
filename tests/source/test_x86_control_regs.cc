import std;
import dcc.session;
import dcc.target;

#include "harness.hh"

namespace
{
    struct TempDir
    {
        std::filesystem::path path;

        TempDir()
        {
            auto base = std::filesystem::temp_directory_path();
            auto tag = std::format("dcc-x86-regs-test-{}", std::chrono::steady_clock::now().time_since_epoch().count());
            path = base / tag;
            std::filesystem::create_directories(path);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        void write_file(std::string_view name, std::string_view content) const
        {
            std::ofstream out{path / name};
            out << content;
        }
    };

}

SECTION("backend: x86 control registers");

TEST_CASE("x86 control registers validate as 32-bit asm operands")
{
    auto const* cr3 = dcc::target::lookup_register(dcc::target::Arch::X86, "cr3");
    REQUIRE(cr3 != nullptr);
    CHECK_EQ(cr3->width, 32u);
    CHECK(!cr3->reserved);
    CHECK(cr3->cls == dcc::target::PhysRegClass::GPR);

    for (auto name : {std::string_view{"cr0"}, std::string_view{"cr2"}, std::string_view{"cr4"}, std::string_view{"dr0"},
                      std::string_view{"dr7"}})
    {
        auto const* reg = dcc::target::lookup_register(dcc::target::Arch::X86, name);
        REQUIRE(reg != nullptr);
        CHECK_EQ(reg->width, 32u);
        CHECK(!reg->reserved);
    }
}

TEST_CASE("cr3 reads and writes asm-compile on x86-elf")
{
    TempDir td;
    td.write_file("mmu.dc",
                    "module test;\n"
                    "usize read_cr3() {\n"
                    "    usize v = asm @[output(usize in eax)] { \"mov %%cr3, %0\" };\n"
                    "    return v;\n"
                    "}\n"
                    "void write_cr3(usize v) {\n"
                    "    asm @[inputs(v in eax = v)] { \"mov %0, %%cr3\" };\n"
                    "}\n");

    auto target = dcc::target::TargetConfig::parse_triple("x86-elf");
    REQUIRE(target.has_value());
    CHECK(target->arch == dcc::target::Arch::X86);
    CHECK_EQ(target->pointer_bits, 32u);

    dcc::session::CompilerSession session{{.silent_diagnostics = true}};
    dcc::session::CompileOptions copts;
    copts.target = *target;
    copts.import_roots.push_back(td.path);
    auto result = session.analyze_entry(td.path / "mmu.dc", copts);
    REQUIRE(result.module != nullptr);
    CHECK(!result.has_errors);
    CHECK(!session.diagnostics().has_errors());
}
