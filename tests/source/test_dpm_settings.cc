import std;
import dcc.cli;
import dpm.manifest;
import dpm.settings;

#include "harness.hh"

namespace
{
    namespace m = dpm::manifest;
    namespace s = dpm::settings;

    [[nodiscard]] std::vector<std::string> with_program(std::vector<std::string> flags)
    {
        flags.insert(flags.begin(), "dcc");
        return flags;
    }

    [[nodiscard]] bool dcc_accepts(std::vector<std::string> const& flags)
    {
        auto outcome = dcc::cli::parse_args_lenient(with_program(flags));
        for (auto const& e : outcome.errors)
            std::println(std::cerr, "      dcc rejected: {}", e);
        return outcome.errors.empty();
    }

    [[nodiscard]] bool contains(std::vector<std::string> const& flags, std::string_view flag)
    {
        return std::ranges::find(flags, flag) != flags.end();
    }

    [[nodiscard]] std::vector<std::string> strs(std::initializer_list<std::string> v)
    {
        return v;
    }

    template <class E> [[nodiscard]] std::vector<E> all(std::initializer_list<E> values)
    {
        return values;
    }

    [[nodiscard]] dcc::cli::OptionSpec const* find_spec(std::string_view name)
    {
        for (auto const& spec : dcc::cli::k_options)
            if (spec.name == name)
                return &spec;
        return nullptr;
    }
} // namespace

SECTION("dpm settings");

TEST_CASE("an empty settings value maps to no flags")
{
    CHECK(s::dcc_flags(m::Settings{}) == strs({"-flibdcext"}));
}

TEST_CASE("every field maps to its dcc flag")
{
    m::Settings st;
    st.target = m::Target::I8086Mz;
    st.backend = m::Backend::Custom;
    st.arch = "i386";
    st.model = m::Model::Unreal;
    st.optimize = m::Optimize::Size;
    st.whole_program = m::Tri::On;
    st.libdcext = m::Libdcext::Freestanding;
    st.debug_info = m::Tri::Off;
    st.bounds_check = m::Tri::On;
    st.restricted_check = m::Tri::Off;
    st.far_check = m::Tri::On;
    st.pic = m::Tri::Off;
    st.red_zone = m::Tri::Off;
    st.simd = m::Tri::Off;
    st.x87 = m::Tri::Off;
    st.stack_protector = m::Tri::Off;
    st.partial_eval = m::Tri::On;
    st.base = "?:0x100";
    st.stack_reserve = "512";
    st.extra_flags = {"-fno-simd"};
    auto expected = strs({"-target=i8086-mz", "-fbackend=custom", "-farch=i386", "-mcmodel=unreal", "-Os", "-fwhole-program", "-flibdcext=freestanding", "-g0",
                          "-fbounds-check", "-fno-restricted-check", "-ffar-check", "-fno-pic", "-fno-red-zone", "-fno-simd", "-fno-x87", "-fno-stack-protector",
                          "-fpartial-eval", "-fbase=?:0x100", "-fstack-reserve=512", "-fno-simd"});
    CHECK(s::dcc_flags(st) == expected);
    CHECK(dcc_accepts(s::dcc_flags(st)));
}

TEST_CASE("the kernel settings map to their flags in both directions and Default passes nothing")
{
    m::Settings on;
    on.target = m::Target::X86_64Elf;
    on.red_zone = m::Tri::On;
    on.simd = m::Tri::On;
    on.x87 = m::Tri::On;
    on.stack_protector = m::Tri::On;
    on.partial_eval = m::Tri::Off;
    CHECK(s::dcc_flags(on) == strs({"-target=x86_64-elf", "-fred-zone", "-fsimd", "-fx87", "-fstack-protector", "-fno-partial-eval"}));
    CHECK(dcc_accepts(s::dcc_flags(on)));
    m::Settings off;
    off.target = m::Target::X86_64Elf;
    off.red_zone = m::Tri::Off;
    off.simd = m::Tri::Off;
    off.x87 = m::Tri::Off;
    off.stack_protector = m::Tri::Off;
    off.partial_eval = m::Tri::On;
    CHECK(s::dcc_flags(off) == strs({"-target=x86_64-elf", "-fno-red-zone", "-fno-simd", "-fno-x87", "-fno-stack-protector", "-fpartial-eval"}));
    CHECK(dcc_accepts(s::dcc_flags(off)));
    m::Settings none;
    none.target = m::Target::X86_64Elf;
    CHECK(s::dcc_flags(none) == strs({"-target=x86_64-elf"}));
}

TEST_CASE("the default libdcext links the hosted variant only for the host target")
{
    m::Settings st;
    CHECK(contains(s::dcc_flags(st), "-flibdcext"));
    for (auto target :
         all<m::Target>({m::Target::X86_64Elf, m::Target::X86_64Coff, m::Target::X86Elf, m::Target::X86Coff, m::Target::I8086Binary, m::Target::I8086Mz}))
    {
        m::Settings other;
        other.target = target;
        auto flags = s::dcc_flags(other);
        CHECK(flags == strs({"-target=" + std::string{s::triple(target)}}));
    }
    st.target = m::Target::X86_64Elf;
    st.libdcext = m::Libdcext::Linux;
    CHECK(contains(s::dcc_flags(st), "-flibdcext=linux"));
    st.libdcext = m::Libdcext::Off;
    CHECK(s::dcc_flags(st) == strs({"-target=x86_64-elf"}));
    st.target = m::Target::Host;
    CHECK(s::dcc_flags(st) == strs({}));
}

TEST_CASE("product flags cover shared libraries and link libraries")
{
    m::Product p;
    p.kind = m::Kind::SharedLibrary;
    p.link = {"m", "z"};
    CHECK(s::product_flags(p) == strs({"-shared", "-lm", "-lz"}));
    CHECK(dcc_accepts(s::product_flags(p)));
}

TEST_CASE("layers override from the lowest to the highest and extra flags concatenate")
{
    m::Project project;
    project.settings.optimize = m::Optimize::O1;
    project.settings.extra_flags = {"-a"};
    m::Profile fast;
    fast.name = "fast";
    fast.settings.optimize = m::Optimize::O2;
    fast.settings.extra_flags = {"-b"};
    project.profiles.push_back(fast);
    m::Product product;
    product.settings.whole_program = m::Tri::Off;
    product.settings.extra_flags = {"-c"};

    project.settings.red_zone = m::Tri::Off;
    project.settings.partial_eval = m::Tri::On;
    product.settings.red_zone = m::Tri::On;
    product.settings.simd = m::Tri::Off;
    fast.settings.stack_protector = m::Tri::Off;
    project.profiles.back().settings.stack_protector = m::Tri::Off;

    auto base = s::effective(project, "debug", nullptr);
    CHECK(base.red_zone == m::Tri::Off);
    CHECK(base.partial_eval == m::Tri::On);
    CHECK(base.simd == m::Tri::Default);
    CHECK(base.optimize == m::Optimize::O1);
    CHECK(base.debug_info == m::Tri::On);
    CHECK(base.bounds_check == m::Tri::On);
    CHECK(base.extra_flags == strs({"-a"}));

    auto layered = s::effective(project, "fast", &product);
    CHECK(layered.optimize == m::Optimize::O2);
    CHECK(layered.whole_program == m::Tri::Off);
    CHECK(layered.red_zone == m::Tri::On);
    CHECK(layered.simd == m::Tri::Off);
    CHECK(layered.stack_protector == m::Tri::Off);
    CHECK(layered.partial_eval == m::Tri::On);
    CHECK(layered.extra_flags == strs({"-a", "-b", "-c"}));

    auto release = s::effective(m::Project{}, "release", nullptr);
    CHECK(release.optimize == m::Optimize::O2);
    CHECK(release.whole_program == m::Tri::On);

    auto cli = s::effective(project, "fast", &product, {.target = m::Target::X86_64Elf, .optimize = m::Optimize::O0});
    CHECK(cli.optimize == m::Optimize::O0);
    CHECK(cli.target == m::Target::X86_64Elf);
}

TEST_CASE("a custom profile starts from the defaults of the profile it inherits")
{
    m::Project project;
    m::Profile debugish;
    debugish.name = "debugish";
    debugish.settings.optimize = m::Optimize::O1;
    m::Profile releaseish;
    releaseish.name = "releaseish";
    releaseish.inherits = m::ProfileBase::Release;
    releaseish.settings.debug_info = m::Tri::On;
    project.profiles = {debugish, releaseish};
    auto d = s::effective(project, "debugish", nullptr);
    CHECK(d.optimize == m::Optimize::O1);
    CHECK(d.debug_info == m::Tri::On);
    CHECK(d.bounds_check == m::Tri::On);
    CHECK(d.whole_program == m::Tri::On);
    auto r = s::effective(project, "releaseish", nullptr);
    CHECK(r.optimize == m::Optimize::O2);
    CHECK(r.whole_program == m::Tri::On);
    CHECK(r.debug_info == m::Tri::On);
    CHECK(r.bounds_check == m::Tri::Default);
}

TEST_CASE("drift: dcc accepts every flag for every enumerator")
{
    for (auto target : all<m::Target>(
             {m::Target::Host, m::Target::X86_64Elf, m::Target::X86_64Coff, m::Target::X86Elf, m::Target::X86Coff, m::Target::I8086Binary, m::Target::I8086Mz}))
    {
        m::Settings st;
        st.target = target;
        auto flags = s::dcc_flags(st);
        CHECK(dcc_accepts(flags));
        auto outcome = dcc::cli::parse_args_lenient(with_program(flags));
        CHECK(outcome.options.target_triple == s::triple(target));
    }
    for (auto backend : all<m::Backend>({m::Backend::Default, m::Backend::Llvm, m::Backend::Custom}))
    {
        m::Settings st;
        st.backend = backend;
        CHECK(dcc_accepts(s::dcc_flags(st)));
    }
    for (auto model :
         all<m::Model>({m::Model::Default, m::Model::Small, m::Model::Kernel, m::Model::Medium, m::Model::Large, m::Model::Unreal, m::Model::Unreal32}))
    {
        m::Settings st;
        st.model = model;
        CHECK(dcc_accepts(s::dcc_flags(st)));
    }
    for (auto optimize : all<m::Optimize>({m::Optimize::Default, m::Optimize::O0, m::Optimize::O1, m::Optimize::O2, m::Optimize::Size}))
    {
        m::Settings st;
        st.optimize = optimize;
        CHECK(dcc_accepts(s::dcc_flags(st)));
    }
    for (auto lib : all<m::Libdcext>({m::Libdcext::Default, m::Libdcext::Off, m::Libdcext::Linux, m::Libdcext::Windows, m::Libdcext::Freestanding}))
        for (auto target : all<m::Target>({m::Target::Host, m::Target::X86_64Elf, m::Target::X86_64Coff}))
        {
            m::Settings st;
            st.libdcext = lib;
            st.target = target;
            CHECK(dcc_accepts(s::dcc_flags(st)));
        }
    for (auto tri : all<m::Tri>({m::Tri::Default, m::Tri::On, m::Tri::Off}))
    {
        m::Settings st;
        st.whole_program = st.debug_info = st.bounds_check = st.restricted_check = st.far_check = st.pic = tri;
        CHECK(dcc_accepts(s::dcc_flags(st)));
    }
}

TEST_CASE("drift: every dcc choice for the mapped options has a dpm enumerator")
{
    auto* model = find_spec("-mcmodel");
    REQUIRE(model != nullptr);
    for (auto choice : model->choices)
    {
        bool found = false;
        for (auto value :
             all<m::Model>({m::Model::Default, m::Model::Small, m::Model::Kernel, m::Model::Medium, m::Model::Large, m::Model::Unreal, m::Model::Unreal32}))
            if (choice == "default" ? value == m::Model::Default : s::model_name(value) == choice)
                found = true;
        CHECK(found);
    }
    auto* backend = find_spec("-fbackend");
    REQUIRE(backend != nullptr);
    CHECK(backend->choices.size() == 2);
    auto* opt = find_spec("-O");
    REQUIRE(opt != nullptr);
    for (auto choice : opt->choices)
    {
        bool found = false;
        for (auto value : all<m::Optimize>({m::Optimize::O0, m::Optimize::O1, m::Optimize::O2, m::Optimize::Size}))
            if (s::optimize_level(value) == choice)
                found = true;
        CHECK(found);
    }
    auto* lib = find_spec("-flibdcext");
    REQUIRE(lib != nullptr);
    for (auto choice : lib->choices)
    {
        bool found = false;
        for (auto value : all<m::Libdcext>({m::Libdcext::Linux, m::Libdcext::Windows, m::Libdcext::Freestanding}))
        {
            m::Settings st;
            st.libdcext = value;
            if (contains(s::dcc_flags(st), "-flibdcext=" + std::string{choice}))
                found = true;
        }
        CHECK(found);
    }
}

TEST_CASE("drift: every emitted flag name exists in the dcc option table")
{
    m::Settings st;
    st.target = m::Target::I8086Mz;
    st.backend = m::Backend::Llvm;
    st.arch = "i386";
    st.model = m::Model::Small;
    st.optimize = m::Optimize::O2;
    st.whole_program = m::Tri::On;
    st.libdcext = m::Libdcext::Linux;
    st.debug_info = m::Tri::On;
    st.bounds_check = m::Tri::Off;
    st.restricted_check = m::Tri::On;
    st.far_check = m::Tri::Off;
    st.pic = m::Tri::On;
    st.red_zone = m::Tri::Off;
    st.simd = m::Tri::On;
    st.x87 = m::Tri::Off;
    st.stack_protector = m::Tri::On;
    st.partial_eval = m::Tri::Off;
    st.base = "1:0";
    st.stack_reserve = "1";
    m::Product product;
    product.kind = m::Kind::SharedLibrary;
    product.link = {"x"};
    auto flags = s::dcc_flags(st);
    for (auto const& f : s::product_flags(product))
        flags.push_back(f);
    for (auto const& flag : flags)
    {
        bool known = false;
        for (auto const& spec : dcc::cli::k_options)
            if (dcc::cli::match_option(spec, flag).matched)
                known = true;
        CHECK(known);
    }
}
