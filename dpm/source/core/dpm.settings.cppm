export module dpm.settings;

import std;
import dpm.manifest;

export namespace dpm::settings
{
    using manifest::Backend;
    using manifest::Kind;
    using manifest::Libdcext;
    using manifest::Model;
    using manifest::Optimize;
    using manifest::Product;
    using manifest::Project;
    using manifest::Settings;
    using manifest::Target;
    using manifest::Tri;

    struct Overrides
    {
        std::optional<Target> target;
        std::optional<Optimize> optimize;
    };

    [[nodiscard]] inline Settings profile_defaults(std::string_view profile)
    {
        Settings s;
        if (profile == "debug")
        {
            s.optimize = Optimize::O0;
            s.whole_program = Tri::On;
            s.debug_info = Tri::On;
            s.bounds_check = Tri::On;
        }
        else if (profile == "release")
        {
            s.optimize = Optimize::O2;
            s.whole_program = Tri::On;
        }
        return s;
    }

    namespace detail
    {
        template <class T> void override_if(T& lower, T const& higher)
        {
            if (higher != T{})
                lower = higher;
        }
    } // namespace detail

    [[nodiscard]] inline Settings merge(Settings lower, Settings const& higher)
    {
        detail::override_if(lower.target, higher.target);
        detail::override_if(lower.backend, higher.backend);
        detail::override_if(lower.arch, higher.arch);
        detail::override_if(lower.model, higher.model);
        detail::override_if(lower.optimize, higher.optimize);
        detail::override_if(lower.whole_program, higher.whole_program);
        detail::override_if(lower.libdcext, higher.libdcext);
        detail::override_if(lower.debug_info, higher.debug_info);
        detail::override_if(lower.bounds_check, higher.bounds_check);
        detail::override_if(lower.restricted_check, higher.restricted_check);
        detail::override_if(lower.far_check, higher.far_check);
        detail::override_if(lower.pic, higher.pic);
        detail::override_if(lower.red_zone, higher.red_zone);
        detail::override_if(lower.simd, higher.simd);
        detail::override_if(lower.x87, higher.x87);
        detail::override_if(lower.stack_protector, higher.stack_protector);
        detail::override_if(lower.partial_eval, higher.partial_eval);
        detail::override_if(lower.base, higher.base);
        detail::override_if(lower.stack_reserve, higher.stack_reserve);
        lower.extra_flags.insert(lower.extra_flags.end(), higher.extra_flags.begin(), higher.extra_flags.end());
        return lower;
    }

    [[nodiscard]] inline Settings effective(Project const& project, std::string_view profile, Product const* product, Overrides const& overrides = {})
    {
        manifest::Profile const* custom = nullptr;
        for (auto const& p : project.profiles)
            if (p.name == profile)
                custom = &p;
        auto base = custom ? (custom->inherits == manifest::ProfileBase::Release ? "release" : "debug") : profile;
        auto result = merge(profile_defaults(base), project.settings);
        if (custom)
            result = merge(std::move(result), custom->settings);
        if (product)
            result = merge(std::move(result), product->settings);
        if (overrides.target)
            result.target = *overrides.target;
        if (overrides.optimize)
            result.optimize = *overrides.optimize;
        return result;
    }

    [[nodiscard]] inline std::string_view triple(Target target)
    {
        switch (target)
        {
            case Target::Host:
                return "";
            case Target::X86_64Elf:
                return "x86_64-elf";
            case Target::X86_64Coff:
                return "x86_64-coff";
            case Target::X86Elf:
                return "x86-elf";
            case Target::X86Coff:
                return "x86-coff";
            case Target::I8086Binary:
                return "i8086-binary";
            case Target::I8086Mz:
                return "i8086-mz";
        }
        return "";
    }

    [[nodiscard]] inline std::string_view model_name(Model model)
    {
        switch (model)
        {
            case Model::Default:
                return "";
            case Model::Small:
                return "small";
            case Model::Kernel:
                return "kernel";
            case Model::Medium:
                return "medium";
            case Model::Large:
                return "large";
            case Model::Unreal:
                return "unreal";
            case Model::Unreal32:
                return "unreal32";
        }
        return "";
    }

    [[nodiscard]] inline std::string_view optimize_level(Optimize optimize)
    {
        switch (optimize)
        {
            case Optimize::Default:
                return "";
            case Optimize::O0:
                return "0";
            case Optimize::O1:
                return "1";
            case Optimize::O2:
                return "2";
            case Optimize::Size:
                return "s";
        }
        return "";
    }

    [[nodiscard]] inline std::string_view libdcext_os(Libdcext lib)
    {
        switch (lib)
        {
            case Libdcext::Off:
                return "";
            case Libdcext::Linux:
                return "linux";
            case Libdcext::Windows:
                return "windows";
            case Libdcext::Freestanding:
                return "freestanding";
            case Libdcext::Default:
                return "";
        }
        return "";
    }

    namespace detail
    {
        inline void tri_flag(std::vector<std::string>& out, Tri value, std::string_view on, std::string_view off)
        {
            if (value == Tri::On)
                out.emplace_back(on);
            else if (value == Tri::Off)
                out.emplace_back(off);
        }
    } // namespace detail

    [[nodiscard]] inline std::vector<std::string> dcc_flags(Settings const& s)
    {
        std::vector<std::string> out;
        if (s.target != Target::Host)
            out.push_back("-target=" + std::string{triple(s.target)});
        if (s.backend == Backend::Llvm)
            out.emplace_back("-fbackend=llvm");
        else if (s.backend == Backend::Custom)
            out.emplace_back("-fbackend=custom");
        if (!s.arch.empty())
            out.push_back("-farch=" + s.arch);
        if (s.model != Model::Default)
            out.push_back("-mcmodel=" + std::string{model_name(s.model)});
        if (s.optimize != Optimize::Default)
            out.push_back("-O" + std::string{optimize_level(s.optimize)});
        detail::tri_flag(out, s.whole_program, "-fwhole-program", "-fno-whole-program");
        if (s.libdcext == Libdcext::Default && s.target == Target::Host)
            out.emplace_back("-flibdcext");
        else if (auto os = libdcext_os(s.libdcext); !os.empty())
            out.push_back("-flibdcext=" + std::string{os});
        detail::tri_flag(out, s.debug_info, "-g", "-g0");
        detail::tri_flag(out, s.bounds_check, "-fbounds-check", "-fno-bounds-check");
        detail::tri_flag(out, s.restricted_check, "-frestricted-check", "-fno-restricted-check");
        detail::tri_flag(out, s.far_check, "-ffar-check", "-fno-far-check");
        detail::tri_flag(out, s.pic, "-fpic", "-fno-pic");
        detail::tri_flag(out, s.red_zone, "-fred-zone", "-fno-red-zone");
        detail::tri_flag(out, s.simd, "-fsimd", "-fno-simd");
        detail::tri_flag(out, s.x87, "-fx87", "-fno-x87");
        detail::tri_flag(out, s.stack_protector, "-fstack-protector", "-fno-stack-protector");
        detail::tri_flag(out, s.partial_eval, "-fpartial-eval", "-fno-partial-eval");
        if (!s.base.empty())
            out.push_back("-fbase=" + s.base);
        if (!s.stack_reserve.empty())
            out.push_back("-fstack-reserve=" + s.stack_reserve);
        out.insert(out.end(), s.extra_flags.begin(), s.extra_flags.end());
        return out;
    }

    [[nodiscard]] inline std::vector<std::string> product_flags(Product const& product)
    {
        std::vector<std::string> out;
        if (product.kind == Kind::SharedLibrary)
            out.emplace_back("-shared");
        for (auto const& lib : product.link)
            out.push_back("-l" + lib);
        return out;
    }
} // namespace dpm::settings
