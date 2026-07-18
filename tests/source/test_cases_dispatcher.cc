#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>

import std;

import dcc.sm;
import dcc.si;
import dcc.lex;
import dcc.ast;
import dcc.ast.serializer;
import dcc.diag;
import dcc.parser;
import dcc.sema;
import dcc.sema.scope_dumper;
import dcc.sema.body_analyzer;
import dcc.sema.type_dumper;
import dcc.sema.instantiator;
import dcc.comptime;
import dcc.sema.infer;
import dcc.types;
import dcc.ir;
import dcc.ir.lower;
import dcc.ir.pass;
import dcc.target;
import dcc.backend;
#if DCC_ENABLE_LLVM
import dcc.backend.llvm;
#endif
import dcc.backend.em64t;

namespace
{
    namespace fs = std::filesystem;

    struct VirtualFile
    {
        std::string path;
        std::string contents;
    };

    struct ExpectAst
    {
        std::string target_file;
        std::string body;
        std::size_t base_line{};
    };

    struct ExpectScope
    {
        std::string module_path;
        std::string body;
        std::size_t base_line{};
    };

    struct ExpectTypes
    {
        std::string module_path;
        std::string body;
        std::size_t base_line{};
    };

    struct ExpectBody
    {
        std::string module_path;
        std::string body;
        std::size_t base_line{};
    };

    struct ExpectInstBody
    {
        std::string module_path;
        std::string body;
        std::size_t base_line{};
    };

    struct ExpectIr
    {
        std::string body;
        std::size_t base_line{};
        bool bounds_check{false};
    };

    struct ExpectLlvm
    {
        std::string body;
        std::size_t base_line{};
        std::string target_triple;
        bool is_error{false};
        bool verify{false};
        bool emit_debug_info{false};
        dcc::backend::DebugFormat debug_format{dcc::backend::DebugFormat::Auto};
        bool no_red_zone{false};
        bool no_simd{false};
        bool no_x87{false};
        bool position_independent_code{false};
        std::optional<dcc::target::CodeModel> code_model;
        bool omit_frame_pointer{true};
        bool bounds_check{false};
    };

    struct ExpectRegistry
    {
        std::string body;
        std::size_t base_line{};
    };

    struct ExpectError
    {
        std::string file;
        int line{};
        std::string substring;
    };

    struct ExpectExecutable
    {
        std::string target_triple;
        bool is_error{false};
        std::size_t base_line{};
        bool no_red_zone{false};
        bool no_simd{false};
        bool no_x87{false};
        bool position_independent_code{false};
        std::optional<dcc::target::CodeModel> code_model;
        bool omit_frame_pointer{true};
    };

    struct RequiredRela
    {
        std::string section;
        std::uint32_t rel_type;
    };

    struct ExpectEm64tObject
    {
        std::size_t base_line{};
        bool verify_sections{true};
        std::optional<int> run_exit_code;
        int opt_level{0};
        bool pic{false};
        std::vector<RequiredRela> required_relas;
    };

    struct ExpectEm64tAsm
    {
        std::size_t base_line{};
        std::vector<std::string> contains;
        int opt_level{0};
    };

    struct Fixture
    {
        std::vector<VirtualFile> files;
        std::string entry;
        std::vector<ExpectAst> ast_blocks;
        std::vector<ExpectScope> scope_blocks;
        std::vector<ExpectTypes> type_blocks;
        std::vector<ExpectBody> body_blocks;
        std::vector<ExpectInstBody> inst_body_blocks;
        std::vector<ExpectIr> ir_blocks;
        std::vector<ExpectLlvm> llvm_blocks;
        std::vector<ExpectRegistry> registry_blocks;
        std::vector<ExpectError> errors;
        std::vector<ExpectExecutable> executable_blocks;
        std::vector<ExpectEm64tObject> em64t_object_blocks;
        std::vector<ExpectEm64tAsm> em64t_asm_blocks;
        std::vector<std::string> injected_decls;
        bool errors_block_present{};
        bool interactive_mode{};
    };

    struct Section
    {
        std::string header;
        std::string body;
        std::size_t body_start_line{};
    };

    bool starts_with(std::string_view s, std::string_view p)
    {
        return s.size() >= p.size() && s.substr(0, p.size()) == p;
    }

    std::string trim(std::string_view s)
    {
        std::size_t a = 0;
        while (a < s.size() && (s[a] == ' ' || s[a] == '\t'))
            ++a;

        std::size_t b = s.size();
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
            --b;

        return std::string{s.substr(a, b - a)};
    }

    std::vector<Section> split_sections(std::string_view content)
    {
        std::vector<Section> out;
        std::size_t i = 0;
        std::size_t line_no = 1;

        Section* current = nullptr;
        std::string buffer;

        while (i < content.size())
        {
            auto nl = content.find('\n', i);
            auto end = (nl == std::string_view::npos) ? content.size() : nl;
            std::string_view raw = content.substr(i, end - i);
            std::string_view stripped = raw;
            while (!stripped.empty() && (stripped.back() == '\r'))
                stripped.remove_suffix(1);

            bool is_header = false;
            std::string header;
            if (starts_with(stripped, "=== ") && stripped.size() >= 8 && stripped.ends_with(" ==="))
            {
                header = std::string{stripped.substr(4, stripped.size() - 8)};
                is_header = true;
            }

            if (is_header)
            {
                if (current)
                    current->body = std::move(buffer);

                out.push_back(Section{std::move(header), {}, line_no + 1});
                current = &out.back();
                buffer.clear();
            }
            else if (current)
            {
                if (!buffer.empty())
                    buffer += '\n';

                buffer.append(stripped);
            }

            i = (nl == std::string_view::npos) ? content.size() : nl + 1;
            ++line_no;
        }

        if (current)
            current->body = std::move(buffer);

        return out;
    }

    std::optional<Fixture> parse_fixture(std::string_view content)
    {
        Fixture fx;
        auto sections = split_sections(content);
        if (sections.empty())
            return std::nullopt;

        for (auto& sec : sections)
        {
            auto h = trim(sec.header);
            if (starts_with(h, "FILE:"))
            {
                VirtualFile vf;
                vf.path = trim(std::string_view{h}.substr(5));
                vf.contents = sec.body;
                if (!vf.contents.empty() && vf.contents.back() != '\n')
                    vf.contents += '\n';

                fx.files.push_back(std::move(vf));
            }
            else if (starts_with(h, "ENTRY:"))
                fx.entry = trim(std::string_view{h}.substr(6));
            else if (starts_with(h, "MODE:") && trim(std::string_view{h}.substr(5)) == "interactive")
                fx.interactive_mode = true;
            else if (starts_with(h, "INJECT:"))
                fx.injected_decls.push_back(trim(std::string_view{h}.substr(7)));
            else if (starts_with(h, "EXPECT-AST FOR:"))
            {
                ExpectAst e;
                e.target_file = trim(std::string_view{h}.substr(15));
                e.body = sec.body;
                e.base_line = sec.body_start_line;
                fx.ast_blocks.push_back(std::move(e));
            }
            else if (starts_with(h, "EXPECT-AST"))
            {
                ExpectAst e;
                e.body = sec.body;
                e.base_line = sec.body_start_line;
                fx.ast_blocks.push_back(std::move(e));
            }
            else if (starts_with(h, "EXPECT-SCOPE FOR:"))
            {
                ExpectScope e;
                e.module_path = trim(std::string_view{h}.substr(17));
                e.body = sec.body;
                e.base_line = sec.body_start_line;
                fx.scope_blocks.push_back(std::move(e));
            }
            else if (starts_with(h, "EXPECT-TYPES FOR:"))
            {
                ExpectTypes e;
                e.module_path = trim(std::string_view{h}.substr(17));
                e.body = sec.body;
                e.base_line = sec.body_start_line;
                fx.type_blocks.push_back(std::move(e));
            }
            else if (starts_with(h, "EXPECT-BODY FOR:"))
            {
                ExpectBody e;
                e.module_path = trim(std::string_view{h}.substr(16));
                e.body = sec.body;
                e.base_line = sec.body_start_line;
                fx.body_blocks.push_back(std::move(e));
            }
            else if (starts_with(h, "EXPECT-BODY"))
            {
                ExpectBody e;
                e.body = sec.body;
                e.base_line = sec.body_start_line;
                fx.body_blocks.push_back(std::move(e));
            }
            else if (starts_with(h, "EXPECT-INSTANTIATED-BODY FOR:"))
            {
                ExpectInstBody e;
                e.module_path = trim(std::string_view{h}.substr(29));
                e.body = sec.body;
                e.base_line = sec.body_start_line;
                fx.inst_body_blocks.push_back(std::move(e));
            }
            else if (starts_with(h, "EXPECT-IR"))
            {
                ExpectIr e;
                e.body = sec.body;
                e.base_line = sec.body_start_line;
                auto flags_start = h.find("FLAGS:");
                if (flags_start != std::string::npos)
                {
                    auto flags_str = trim(std::string_view{h}.substr(flags_start + 6));
                    if (flags_str.find("-fbounds-check") != std::string::npos)
                        e.bounds_check = true;
                }
                fx.ir_blocks.push_back(std::move(e));
            }
            else if (starts_with(h, "EXPECT-LLVM"))
            {
                ExpectLlvm e;
                e.body = sec.body;
                e.base_line = sec.body_start_line;
                e.is_error = starts_with(h, "EXPECT-LLVM-ERRORS");
                auto flags_start = h.find("FLAGS:");
                if (flags_start != std::string::npos)
                {
                    auto flags_str = trim(std::string_view{h}.substr(flags_start + 6));

                    auto target_pos = flags_str.find("-target");
                    if (target_pos != std::string::npos)
                    {
                        auto target_val = trim(std::string_view{flags_str}.substr(target_pos + 7));
                        if (target_val.starts_with("="))
                        {
                            auto remaining = std::string_view{target_val}.substr(1);
                            auto sp = remaining.find(' ');
                            if (sp != std::string::npos)
                                e.target_triple = std::string{trim(remaining.substr(0, sp))};
                            else
                                e.target_triple = std::string{trim(remaining)};
                        }
                        else if (!target_val.empty())
                        {
                            auto sp = target_val.find(' ');
                            if (sp != std::string::npos)
                                e.target_triple = std::string{trim(std::string_view{target_val}.substr(0, sp))};
                            else
                                e.target_triple = std::string{target_val};
                        }
                    }

                    if (flags_str.find("-verify") != std::string::npos)
                        e.verify = true;
                    if (flags_str.find("-gdwarf") != std::string::npos)
                    {
                        e.emit_debug_info = true;
                        e.debug_format = dcc::backend::DebugFormat::Dwarf;
                    }
                    else if (flags_str.find("-gpdb") != std::string::npos)
                    {
                        e.emit_debug_info = true;
                        e.debug_format = dcc::backend::DebugFormat::Pdb;
                    }
                    else if (flags_str.find("-g") != std::string::npos && flags_str.find("-gnone") == std::string::npos &&
                             flags_str.find("-g0") == std::string::npos)
                    {
                        e.emit_debug_info = true;
                    }
                    if (flags_str.find("-fbounds-check") != std::string::npos)
                        e.bounds_check = true;
                    if (flags_str.find("-fno-red-zone") != std::string::npos)
                        e.no_red_zone = true;
                    if (flags_str.find("-fno-simd") != std::string::npos)
                        e.no_simd = true;
                    if (flags_str.find("-fno-x87") != std::string::npos)
                        e.no_x87 = true;
                    if (flags_str.find("-fPIC") != std::string::npos || flags_str.find("-fPIE") != std::string::npos)
                        e.position_independent_code = true;
                    if (flags_str.find("-fno-omit-frame-pointer") != std::string::npos)
                        e.omit_frame_pointer = false;
                    else if (flags_str.find("-fomit-frame-pointer") != std::string::npos)
                        e.omit_frame_pointer = true;

                    auto mcmodel_pos = flags_str.find("-mcmodel=");
                    if (mcmodel_pos != std::string::npos)
                    {
                        auto after = std::string_view{flags_str}.substr(mcmodel_pos + 9);
                        auto sp = after.find(' ');
                        auto mcmodel_val = (sp != std::string::npos) ? after.substr(0, sp) : after;
                        e.code_model = dcc::target::TargetConfig::parse_code_model(trim(mcmodel_val));
                    }
                    else
                    {
                        mcmodel_pos = flags_str.find("-mcmodel ");
                        if (mcmodel_pos != std::string::npos)
                        {
                            auto after = std::string_view{flags_str}.substr(mcmodel_pos + 9);
                            auto sp = after.find(' ');
                            auto mcmodel_val = (sp != std::string::npos) ? after.substr(0, sp) : after;
                            e.code_model = dcc::target::TargetConfig::parse_code_model(trim(mcmodel_val));
                        }
                    }
                }
                fx.llvm_blocks.push_back(std::move(e));
            }
            else if (starts_with(h, "EXPECT-EM64T-ASM"))
            {
                ExpectEm64tAsm e;
                e.base_line = sec.body_start_line;
                e.opt_level = 0;
                auto flags_start = h.find("FLAGS:");
                if (flags_start != std::string::npos)
                {
                    auto flags_str = trim(std::string_view{h}.substr(flags_start + 6));
                    if (flags_str.find("-O1") != std::string::npos)
                        e.opt_level = 1;
                }

                if (!sec.body.empty())
                {
                    std::istringstream body_ss{sec.body};
                    std::string body_line;
                    while (std::getline(body_ss, body_line))
                    {
                        auto tl = trim(body_line);
                        if (starts_with(tl, "CONTAINS:"))
                        {
                            auto val = trim(std::string_view{tl}.substr(9));
                            if (!val.empty())
                                e.contains.push_back(std::string{val});
                        }
                    }
                }
                fx.em64t_asm_blocks.push_back(std::move(e));
            }
            else if (starts_with(h, "EXPECT-EM64T-OBJECT"))
            {
                ExpectEm64tObject e;
                e.base_line = sec.body_start_line;
                e.verify_sections = true;
                e.opt_level = 0;
                auto flags_start = h.find("FLAGS:");
                if (flags_start != std::string::npos)
                {
                    auto flags_str = trim(std::string_view{h}.substr(flags_start + 6));
                    if (flags_str.find("-no-section-check") != std::string::npos)
                        e.verify_sections = false;
                    if (flags_str.find("-O1") != std::string::npos || flags_str.find("OPT-LEVEL: 1") != std::string::npos)
                        e.opt_level = 1;
                }

                if (!sec.body.empty())
                {
                    std::istringstream body_ss{sec.body};
                    std::string body_line;
                    while (std::getline(body_ss, body_line))
                    {
                        auto tl = trim(body_line);
                        if (starts_with(tl, "RUN-EXIT:"))
                        {
                            auto val_str = trim(std::string_view{tl}.substr(9));
                            try
                            {
                                e.run_exit_code = std::stoi(std::string{val_str});
                            }
                            catch (...)
                            {
                            }
                        }
                        else if (starts_with(tl, "PIC:"))
                        {
                            auto val_str = trim(std::string_view{tl}.substr(4));
                            e.pic = (val_str == "true" || val_str == "1");
                        }
                        else if (starts_with(tl, "REQUIRE-RELA:"))
                        {
                            auto val_str = trim(std::string_view{tl}.substr(13));

                            auto sp = val_str.find(' ');
                            if (sp != std::string::npos)
                            {
                                RequiredRela rr;
                                rr.section = trim(val_str.substr(0, sp));
                                try
                                {
                                    rr.rel_type = static_cast<std::uint32_t>(std::stoul(trim(val_str.substr(sp + 1))));
                                }
                                catch (...)
                                {
                                }
                                e.required_relas.push_back(rr);
                            }
                        }
                    }
                }
                fx.em64t_object_blocks.push_back(std::move(e));
            }
            else if (starts_with(h, "EXPECT-EXECUTABLE"))
            {
                ExpectExecutable e;
                e.is_error = starts_with(h, "EXPECT-EXECUTABLE-ERRORS");
                e.base_line = sec.body_start_line;
                auto flags_start = h.find("FLAGS:");
                if (flags_start != std::string::npos)
                {
                    auto flags_str = trim(std::string_view{h}.substr(flags_start + 6));

                    auto target_pos = flags_str.find("-target");
                    if (target_pos != std::string::npos)
                    {
                        auto target_val = trim(std::string_view{flags_str}.substr(target_pos + 7));
                        if (target_val.starts_with("="))
                        {
                            auto remaining = std::string_view{target_val}.substr(1);
                            auto sp = remaining.find(' ');
                            if (sp != std::string::npos)
                                e.target_triple = std::string{trim(remaining.substr(0, sp))};
                            else
                                e.target_triple = std::string{trim(remaining)};
                        }
                        else if (!target_val.empty())
                        {
                            auto sp = target_val.find(' ');
                            if (sp != std::string::npos)
                                e.target_triple = std::string{trim(std::string_view{target_val}.substr(0, sp))};
                            else
                                e.target_triple = std::string{target_val};
                        }
                    }

                    if (flags_str.find("-fno-red-zone") != std::string::npos)
                        e.no_red_zone = true;
                    if (flags_str.find("-fno-simd") != std::string::npos)
                        e.no_simd = true;
                    if (flags_str.find("-fno-x87") != std::string::npos)
                        e.no_x87 = true;
                    if (flags_str.find("-fPIC") != std::string::npos || flags_str.find("-fPIE") != std::string::npos)
                        e.position_independent_code = true;
                    if (flags_str.find("-fno-omit-frame-pointer") != std::string::npos)
                        e.omit_frame_pointer = false;
                    else if (flags_str.find("-fomit-frame-pointer") != std::string::npos)
                        e.omit_frame_pointer = true;

                    auto mcmodel_pos = flags_str.find("-mcmodel=");
                    if (mcmodel_pos != std::string::npos)
                    {
                        auto after = std::string_view{flags_str}.substr(mcmodel_pos + 9);
                        auto sp = after.find(' ');
                        auto mcmodel_val = (sp != std::string::npos) ? after.substr(0, sp) : after;
                        e.code_model = dcc::target::TargetConfig::parse_code_model(trim(mcmodel_val));
                    }
                    else
                    {
                        mcmodel_pos = flags_str.find("-mcmodel ");
                        if (mcmodel_pos != std::string::npos)
                        {
                            auto after = std::string_view{flags_str}.substr(mcmodel_pos + 9);
                            auto sp = after.find(' ');
                            auto mcmodel_val = (sp != std::string::npos) ? after.substr(0, sp) : after;
                            e.code_model = dcc::target::TargetConfig::parse_code_model(trim(mcmodel_val));
                        }
                    }
                }
                fx.executable_blocks.push_back(std::move(e));
            }
            else if (starts_with(h, "EXPECT-REGISTRY"))
            {
                ExpectRegistry e;
                e.body = sec.body;
                e.base_line = sec.body_start_line;
                fx.registry_blocks.push_back(std::move(e));
            }
            else if (starts_with(h, "EXPECT-ERRORS"))
            {
                fx.errors_block_present = true;
                std::size_t i = 0;
                std::string_view body = sec.body;
                while (i < body.size())
                {
                    auto nl = body.find('\n', i);
                    auto end = (nl == std::string_view::npos) ? body.size() : nl;
                    std::string_view line = body.substr(i, end - i);
                    auto trimmed = trim(line);
                    if (!trimmed.empty())
                    {
                        auto first_colon = trimmed.find(':');
                        auto second_colon = (first_colon == std::string::npos) ? std::string::npos : trimmed.find(':', first_colon + 1);
                        if (second_colon == std::string::npos)
                            return std::nullopt;

                        ExpectError e;
                        e.file = trimmed.substr(0, first_colon);
                        try
                        {
                            e.line = std::stoi(trimmed.substr(first_colon + 1, second_colon - first_colon - 1));
                        }
                        catch (...)
                        {
                            return std::nullopt;
                        }

                        e.substring = trim(std::string_view{trimmed}.substr(second_colon + 1));
                        fx.errors.push_back(std::move(e));
                    }
                    i = (nl == std::string_view::npos) ? body.size() : nl + 1;
                }
            }
        }

        if (fx.entry.empty() && !fx.files.empty())
            fx.entry = fx.files.front().path;

        for (auto& a : fx.ast_blocks)
            if (a.target_file.empty())
                a.target_file = fx.entry;

        return fx;
    }

    struct Sandbox
    {
        fs::path root;

        Sandbox(fs::path p = {}) : root(std::move(p)) {}
        Sandbox(Sandbox&& other) noexcept : root(std::move(other.root)) { other.root.clear(); }

        Sandbox(const Sandbox&) = delete;
        Sandbox& operator=(const Sandbox&) = delete;
        Sandbox& operator=(Sandbox&&) = delete;

        ~Sandbox()
        {
            std::error_code ec;
            if (!root.empty())
                fs::remove_all(root, ec);
        }
    };

    std::optional<Sandbox> materialize(Fixture const& fx)
    {
        std::error_code ec;
        auto base = fs::temp_directory_path(ec);
        if (ec)
            return std::nullopt;

        auto tag = std::format("dcc-test-{}", std::chrono::steady_clock::now().time_since_epoch().count());
        auto root = base / tag;
        if (!fs::create_directories(root, ec))
            return std::nullopt;

        Sandbox sb{root};

        for (auto const& vf : fx.files)
        {
            if (vf.path.empty())
                return std::nullopt;

            {
                auto p = fs::path{vf.path};
                if (p.is_absolute())
                    return std::nullopt;

                auto norm = p.lexically_normal();
                if (norm.empty() || norm.native().find("..") != std::string::npos)
                    return std::nullopt;
            }

            auto target = root / vf.path;
            fs::create_directories(target.parent_path(), ec);

            std::ofstream out{target, std::ios::binary};
            if (!out)
                return std::nullopt;

            out.write(vf.contents.data(), static_cast<std::streamsize>(vf.contents.size()));
        }

        return sb;
    }

    std::string normalize(std::string_view s)
    {
        std::vector<std::string> lines;
        std::size_t i = 0;
        while (i < s.size())
        {
            auto nl = s.find('\n', i);
            auto end = (nl == std::string_view::npos) ? s.size() : nl;
            std::string_view line = s.substr(i, end - i);

            while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
                line.remove_suffix(1);

            lines.emplace_back(line);
            i = (nl == std::string_view::npos) ? s.size() : nl + 1;
        }

        while (!lines.empty() && lines.back().empty())
            lines.pop_back();

        std::string out;
        for (std::size_t k = 0; k < lines.size(); ++k)
        {
            out += lines[k];
            if (k + 1 < lines.size())
                out += '\n';
        }

        return out;
    }

    std::vector<std::string_view> split_lines(std::string_view s)
    {
        std::vector<std::string_view> lines;
        std::size_t i = 0;
        while (i < s.size())
        {
            auto nl = s.find('\n', i);
            auto end = (nl == std::string_view::npos) ? s.size() : nl;
            lines.push_back(s.substr(i, end - i));
            i = (nl == std::string_view::npos) ? s.size() : nl + 1;
        }
        return lines;
    }

    void print_diff(std::string_view label, std::string_view expected, std::string_view actual, std::size_t base_line)
    {
        std::println(std::cerr, "          === {} ===", label);
        auto exp_lines = split_lines(expected);
        auto act_lines = split_lines(actual);

        auto n = std::max(exp_lines.size(), act_lines.size());
        for (std::size_t i = 0; i < n; ++i)
        {
            std::string_view e = i < exp_lines.size() ? exp_lines[i] : std::string_view{"<missing>"};
            std::string_view a = i < act_lines.size() ? act_lines[i] : std::string_view{"<missing>"};
            if (e != a)
            {
                std::println(std::cerr, "          first diff at expected line {}:", base_line + i);
                std::println(std::cerr, "            expected: {}", e);
                std::println(std::cerr, "            actual:   {}", a);
                return;
            }
        }
    }

    struct CapturedError
    {
        std::string file;
        int line{};
        std::string message;
    };

    std::list<CapturedError> parse_diag_output(std::string_view text)
    {
        std::vector<std::string_view> lines;
        std::size_t i = 0;
        while (i < text.size())
        {
            auto nl = text.find('\n', i);
            auto end = (nl == std::string_view::npos) ? text.size() : nl;
            lines.push_back(text.substr(i, end - i));
            i = (nl == std::string_view::npos) ? text.size() : nl + 1;
        }

        CapturedError* current = nullptr;
        std::list<CapturedError> staged;
        for (auto line : lines)
        {
            auto t = trim(line);
            if (starts_with(t, "= note:"))
            {
                staged.push_back({});
                if (current)
                {
                    staged.back().file = current->file;
                    staged.back().line = current->line;
                }
                staged.back().message = std::string{"= note: "} + trim(std::string_view{t}.substr(7));
                current = &staged.back();
            }
            else if (starts_with(t, "= help:") || starts_with(t, "= fix:") || starts_with(t, "= "))
                ;
            else if (starts_with(t, "error:"))
            {
                staged.push_back({});
                staged.back().message = trim(std::string_view{t}.substr(6));
                current = &staged.back();
            }
            else if (starts_with(t, "note:"))
            {
                staged.push_back({});
                staged.back().message = trim(std::string_view{t}.substr(5));
                current = &staged.back();
            }
            else if (starts_with(t, "--> ") && current)
            {
                auto rest = std::string_view{t}.substr(4);
                auto last_colon = rest.rfind(':');
                if (last_colon == std::string::npos)
                    continue;

                auto second_last = rest.rfind(':', last_colon - 1);
                if (second_last == std::string::npos)
                    continue;

                current->file = std::string{rest.substr(0, second_last)};
                try
                {
                    current->line = std::stoi(std::string{rest.substr(second_last + 1, last_colon - second_last - 1)});
                }
                catch (...)
                {
                    continue;
                }
            }
        }

        return staged;
    }

    struct Stats
    {
        int checks = 0;
        int failed = 0;
        int skipped = 0;
    };

    bool run_fixture(fs::path const& path, Stats& stats)
    {
        ++stats.checks;

        std::ifstream in{path, std::ios::binary};
        if (!in)
        {
            ++stats.failed;
            std::println(std::cerr, "    FAIL  could not read fixture  ({}:1)", path.string());
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        auto contents = ss.str();

        auto fx_opt = parse_fixture(contents);
        if (!fx_opt || fx_opt->files.empty())
        {
            ++stats.failed;
            std::println(std::cerr, "    FAIL  malformed fixture  ({}:1)", path.string());
            return false;
        }
        auto& fx = *fx_opt;

        auto sb = materialize(fx);
        if (!sb)
        {
            ++stats.failed;
            std::println(std::cerr, "    FAIL  could not materialize sandbox  ({}:1)", path.string());
            return false;
        }

        dcc::sm::SourceManager sm;
        std::ostringstream diag_sink;
        dcc::diag::DiagnosticEngine diag{sm, diag_sink};
        diag.set_color(false);
        dcc::ast::AstContext ast_ctx;
        dcc::si::string_interner interner;

        auto parse_fn = [&](dcc::sm::FileId fid, dcc::ast::AstContext& ctx, dcc::diag::DiagnosticEngine& d) -> dcc::ast::TranslationUnit* {
            auto const* file = sm.get(fid);
            if (!file)
                return nullptr;

            dcc::lex::Lexer lexer{*file, interner};
            auto mode = fx.interactive_mode ? dcc::parser::ParseMode::Interactive : dcc::parser::ParseMode::Batch;
            dcc::parser::Parser parser{lexer, ctx, d, mode};
            return parser.parse();
        };

        dcc::sema::SemaOptions opts;
        opts.import_roots.push_back(sb->root);
        opts.interner = &interner;
        opts.injected_decls = fx.injected_decls;

        dcc::sema::SemaContext sema{sm, diag, ast_ctx, parse_fn, std::move(opts)};
        sema.analyze_entry(sb->root / fx.entry);

        auto make_module_path = [&](std::string_view mp_str) -> dcc::sema::ModulePath {
            std::vector<std::string_view> segs;
            std::string buf;
            for (std::size_t i = 0; i < mp_str.size(); ++i)
                if (i + 1 < mp_str.size() && mp_str[i] == ':' && mp_str[i + 1] == ':')
                {
                    segs.push_back(interner.intern(buf));
                    buf.clear();
                    ++i;
                }
                else
                    buf += mp_str[i];
            if (!buf.empty())
                segs.push_back(interner.intern(buf));
            return dcc::sema::ModulePath{std::move(segs)};
        };

        bool ok = true;

        if (fx.errors_block_present)
        {
            auto captured = parse_diag_output(diag_sink.str());
            for (auto const& want : fx.errors)
            {
                bool matched = false;
                for (auto const& got : captured)
                {
                    auto leaf = fs::path{got.file}.lexically_relative(sb->root).string();
                    if ((leaf == want.file || got.file == want.file) && got.line == want.line && got.message.find(want.substring) != std::string::npos)
                    {
                        matched = true;
                        break;
                    }
                }
                if (!matched)
                {
                    ok = false;
                    std::println(std::cerr, "    FAIL  expected error not seen: {}:{}: {}  ({}:1)", want.file, want.line, want.substring, path.string());
                    for (auto const& got : captured)
                        std::println(std::cerr, "          got error: {}:{}: {}", fs::path{got.file}.lexically_relative(sb->root).string(), got.line,
                                     got.message);
                }
            }
        }
        else if (diag.has_errors())
        {
            ok = false;
            std::println(std::cerr, "    FAIL  unexpected errors  ({}:1)", path.string());
            std::istringstream iss{diag_sink.str()};
            std::string line;
            while (std::getline(iss, line))
                std::println(std::cerr, "          | {}", line);
        }

        for (auto const& exp : fx.ast_blocks)
        {
            auto target = sb->root / exp.target_file;
            auto fid_opt = sm.find_by_path(target);
            if (!fid_opt)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  EXPECT-AST: file `{}` was not parsed  ({}:{})", exp.target_file, path.string(), exp.base_line);
                continue;
            }
            auto* tu = sema.graph().find_tu_for_file(*fid_opt);
            auto actual = tu ? dcc::ast::AstSerializer::dump(tu) : std::string{"<null-tu>\n"};
            auto a = normalize(actual);
            auto e = normalize(exp.body);
            if (a != e)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  AST mismatch for {}  ({}:{})", exp.target_file, path.string(), exp.base_line);
                print_diff("AST", e, a, exp.base_line);
            }
        }

        for (auto const& exp : fx.scope_blocks)
        {
            dcc::sema::ModulePath mp = make_module_path(exp.module_path);
            auto const* mod = sema.graph().find(mp);
            if (!mod)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  EXPECT-SCOPE: module `{}` not found  ({}:{})", exp.module_path, path.string(), exp.base_line);
                continue;
            }
            auto actual = dcc::sema::ScopeDumper::dump(*mod);
            auto a = normalize(actual);
            auto e = normalize(exp.body);
            if (a != e)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  scope mismatch for {}  ({}:{})", exp.module_path, path.string(), exp.base_line);
                print_diff("SCOPE", e, a, exp.base_line);
            }
        }

        for (auto const& exp : fx.type_blocks)
        {
            dcc::sema::ModulePath mp = make_module_path(exp.module_path);
            auto const* mod = sema.graph().find(mp);
            if (!mod)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  EXPECT-TYPES: module `{}` not found  ({}:{})", exp.module_path, path.string(), exp.base_line);
                continue;
            }

            auto actual = dcc::sema::TypeDumper::dump(*mod);
            auto a = normalize(actual);
            auto e = normalize(exp.body);
            if (a != e)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  type mismatch for {}  ({}:{})", exp.module_path, path.string(), exp.base_line);
                print_diff("TYPES", e, a, exp.base_line);
            }
        }

        for (auto const& exp : fx.body_blocks)
        {
            auto const* mod = [&]() -> dcc::sema::ModuleInfo const* {
                if (exp.module_path.empty())
                    return sema.graph().all().empty() ? nullptr : sema.graph().all().front().get();

                dcc::sema::ModulePath mp = make_module_path(exp.module_path);
                return sema.graph().find(mp);
            }();
            if (!mod)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  EXPECT-BODY: module `{}` not found  ({}:{})", exp.module_path.empty() ? "<entry>" : exp.module_path,
                             path.string(), exp.base_line);
                continue;
            }

            auto actual = dcc::sema::BodyDumper::dump(*mod);
            auto a = normalize(actual);
            auto e = normalize(exp.body);
            if (a != e)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  body mismatch for {}  ({}:{})", exp.module_path, path.string(), exp.base_line);
                print_diff("BODY", e, a, exp.base_line);
            }
        }

        for (auto const& exp : fx.inst_body_blocks)
        {
            auto const* mod = [&]() -> dcc::sema::ModuleInfo const* {
                if (exp.module_path.empty())
                    return sema.graph().all().empty() ? nullptr : sema.graph().all().front().get();

                dcc::sema::ModulePath mp = make_module_path(exp.module_path);
                return sema.graph().find(mp);
            }();

            if (!mod || !mod->tu)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  EXPECT-INSTANTIATED-BODY: module `{}` not found  ({}:{})",
                             exp.module_path.empty() ? "<entry>" : exp.module_path, path.string(), exp.base_line);
                continue;
            }

            std::string actual;
            auto& type_ctx = sema.types();

            auto resolve_arg_type = [&](dcc::ast::TypeExpr const* t) -> dcc::types::TypePtr {
                if (!t)
                    return nullptr;

                if (auto* pt = dcc::ast::node_cast<dcc::ast::PrimitiveType>(t))
                {
                    switch (pt->which)
                    {
                        case dcc::lex::TokenKind::Kwi8:
                            return type_ctx.int_t(8, true);
                        case dcc::lex::TokenKind::Kwi16:
                            return type_ctx.int_t(16, true);
                        case dcc::lex::TokenKind::Kwi32:
                            return type_ctx.int_t(32, true);
                        case dcc::lex::TokenKind::Kwi64:
                            return type_ctx.int_t(64, true);
                        case dcc::lex::TokenKind::Kwu8:
                            return type_ctx.int_t(8, false);
                        case dcc::lex::TokenKind::Kwu16:
                            return type_ctx.int_t(16, false);
                        case dcc::lex::TokenKind::Kwu32:
                            return type_ctx.int_t(32, false);
                        case dcc::lex::TokenKind::Kwu64:
                            return type_ctx.int_t(64, false);
                        case dcc::lex::TokenKind::Kwf32:
                            return type_ctx.float_t(32);
                        case dcc::lex::TokenKind::Kwf64:
                            return type_ctx.float_t(64);
                        case dcc::lex::TokenKind::KwBool:
                            return type_ctx.m_boolt();
                        case dcc::lex::TokenKind::KwVoid:
                            return type_ctx.m_voidt();
                        case dcc::lex::TokenKind::KwChar:
                            return type_ctx.m_chart();
                        default:
                            return nullptr;
                    }
                }
                return nullptr;
            };

            auto value_from_arg = [&](dcc::ast::TemplateArg const& arg, dcc::types::TypePtr canonical_type) -> std::optional<dcc::comptime::Value> {
                if (!arg.expr || !canonical_type)
                    return std::nullopt;

                if (auto* lit = dcc::ast::node_cast<dcc::ast::IntLiteralExpr>(arg.expr))
                {
                    if (canonical_type->kind == dcc::types::TypeKind::Int)
                        return dcc::comptime::Value::make_int(lit->value, canonical_type);
                    return std::nullopt;
                }
                if (auto* blit = dcc::ast::node_cast<dcc::ast::BoolLiteralExpr>(arg.expr))
                {
                    if (canonical_type->kind == dcc::types::TypeKind::Bool)
                        return dcc::comptime::Value::make_bool(blit->value, canonical_type);
                    if (canonical_type->kind == dcc::types::TypeKind::Int)
                        return dcc::comptime::Value::make_int(blit->value ? 1 : 0, canonical_type);
                    return std::nullopt;
                }
                if (auto* clit = dcc::ast::node_cast<dcc::ast::CharLiteralExpr>(arg.expr))
                {
                    if (canonical_type->kind == dcc::types::TypeKind::Char)
                        return dcc::comptime::Value::make_char(clit->codepoint, canonical_type);
                    if (canonical_type->kind == dcc::types::TypeKind::Int)
                        return dcc::comptime::Value::make_int(static_cast<std::int64_t>(clit->codepoint), canonical_type);
                    return std::nullopt;
                }
                if (auto* slit = dcc::ast::node_cast<dcc::ast::StringLiteralExpr>(arg.expr))
                {
                    return dcc::comptime::Value::make_string(std::string{slit->value}, canonical_type);
                }
                return std::nullopt;
            };

            for (auto* d : mod->tu->decls)
            {
                auto const* template_fn = dcc::ast::node_cast<dcc::ast::FuncDecl>(d);
                if (!template_fn || template_fn->template_params.empty())
                    continue;

                struct InstCollector
                {
                    struct Instantiation
                    {
                        std::string_view callee_name;
                        std::span<dcc::ast::TemplateArg const> args;
                    };
                    std::vector<Instantiation> instantiations;

                    void collect_expr(dcc::ast::Expr const* e)
                    {
                        if (!e)
                            return;
                        if (auto* ti = dcc::ast::node_cast<dcc::ast::TemplateInstExpr>(e))
                        {
                            std::string_view callee;
                            if (auto* ident = dcc::ast::node_cast<dcc::ast::IdentExpr>(ti->callee))
                                callee = ident->name;
                            instantiations.push_back({callee, ti->template_args});
                        }

                        switch (e->kind)
                        {
                            case dcc::ast::ExprKind::Call: {
                                auto& c = static_cast<dcc::ast::CallExpr const&>(*e);
                                collect_expr(c.callee);
                                for (auto* a : c.args)
                                    collect_expr(a);
                                break;
                            }
                            case dcc::ast::ExprKind::TemplateInst: {
                                auto& t = static_cast<dcc::ast::TemplateInstExpr const&>(*e);
                                collect_expr(t.callee);
                                break;
                            }
                            case dcc::ast::ExprKind::Unary: {
                                collect_expr(static_cast<dcc::ast::UnaryExpr const&>(*e).operand);
                                break;
                            }
                            case dcc::ast::ExprKind::Postfix: {
                                collect_expr(static_cast<dcc::ast::PostfixExpr const&>(*e).operand);
                                break;
                            }
                            case dcc::ast::ExprKind::Binary: {
                                auto& b = static_cast<dcc::ast::BinaryExpr const&>(*e);
                                collect_expr(b.lhs);
                                collect_expr(b.rhs);
                                break;
                            }
                            case dcc::ast::ExprKind::Block: {
                                collect_block(static_cast<dcc::ast::BlockExpr const&>(*e).body);
                                break;
                            }
                            case dcc::ast::ExprKind::If: {
                                auto& i = static_cast<dcc::ast::IfExpr const&>(*e);
                                collect_expr(i.condition);
                                collect_block(i.then_block);
                                collect_expr(i.else_branch);
                                break;
                            }
                            case dcc::ast::ExprKind::Cast: {
                                collect_expr(static_cast<dcc::ast::CastExpr const&>(*e).operand);
                                break;
                            }
                            case dcc::ast::ExprKind::Index: {
                                auto& idx = static_cast<dcc::ast::IndexExpr const&>(*e);
                                collect_expr(idx.object);
                                collect_expr(idx.index);
                                break;
                            }
                            case dcc::ast::ExprKind::FieldAccess: {
                                collect_expr(static_cast<dcc::ast::FieldAccessExpr const&>(*e).object);
                                break;
                            }
                            case dcc::ast::ExprKind::StructLiteral: {
                                auto& sl = static_cast<dcc::ast::StructLiteralExpr const&>(*e);
                                for (auto& f : sl.fields)
                                    collect_expr(f.value);
                                break;
                            }
                            case dcc::ast::ExprKind::Range: {
                                auto& r = static_cast<dcc::ast::RangeExpr const&>(*e);
                                collect_expr(r.start);
                                collect_expr(r.end);
                                break;
                            }
                            default:
                                break;
                        }
                    }

                    void collect_stmt(dcc::ast::Stmt const* s)
                    {
                        if (!s)
                            return;
                        switch (s->kind)
                        {
                            case dcc::ast::StmtKind::Expr:
                                collect_expr(static_cast<dcc::ast::ExprStmt const&>(*s).expr);
                                break;
                            case dcc::ast::StmtKind::Return:
                                collect_expr(static_cast<dcc::ast::ReturnStmt const&>(*s).value);
                                break;
                            case dcc::ast::StmtKind::DeclStmt: {
                                auto& ds = static_cast<dcc::ast::DeclStmt const&>(*s);
                                if (auto* vd = dcc::ast::node_cast<dcc::ast::VarDecl>(ds.decl))
                                    collect_expr(vd->init);
                                break;
                            }
                            case dcc::ast::StmtKind::While: {
                                auto& w = static_cast<dcc::ast::WhileStmt const&>(*s);
                                collect_expr(w.condition);
                                collect_block(w.body);
                                break;
                            }
                            case dcc::ast::StmtKind::DoWhile: {
                                auto& dw = static_cast<dcc::ast::DoWhileStmt const&>(*s);
                                collect_block(dw.body);
                                collect_expr(dw.condition);
                                break;
                            }
                            case dcc::ast::StmtKind::For: {
                                auto& f = static_cast<dcc::ast::ForStmt const&>(*s);
                                collect_stmt(f.init);
                                collect_expr(f.cond);
                                collect_expr(f.update);
                                collect_block(f.body);
                                break;
                            }
                            case dcc::ast::StmtKind::ForIn: {
                                auto& fi = static_cast<dcc::ast::ForInStmt const&>(*s);
                                collect_expr(fi.iterable);
                                collect_block(fi.body);
                                break;
                            }
                            case dcc::ast::StmtKind::StaticIf: {
                                auto& si = static_cast<dcc::ast::StaticIfStmt const&>(*s);
                                collect_expr(si.condition);
                                collect_block(si.then_block);
                                collect_stmt(si.else_branch);
                                break;
                            }
                            case dcc::ast::StmtKind::StaticMatch: {
                                auto& sm = static_cast<dcc::ast::StaticMatchStmt const&>(*s);
                                collect_expr(sm.operand);
                                break;
                            }
                            case dcc::ast::StmtKind::Defer: {
                                collect_stmt(static_cast<dcc::ast::DeferStmt const&>(*s).body);
                                break;
                            }
                            default:
                                break;
                        }
                    }

                    void collect_block(dcc::ast::Block const& b)
                    {
                        for (auto* s : b.stmts)
                            collect_stmt(s);
                        collect_expr(b.tail);
                    }
                };

                InstCollector collector;
                for (auto* dd : mod->tu->decls)
                    if (auto* fd = dcc::ast::node_cast<dcc::ast::FuncDecl>(dd))
                        if (fd->body)
                            collector.collect_block(*fd->body);

                bool tpl_has_pack = false;
                bool tpl_value_pack = false;
                std::size_t tpl_non_pack_count = template_fn->template_params.size();
                if (!template_fn->template_params.empty())
                {
                    auto const& last = template_fn->template_params.back();
                    tpl_has_pack = last.is_pack;
                    if (tpl_has_pack)
                    {
                        tpl_non_pack_count = template_fn->template_params.size() - 1;
                        tpl_value_pack = (last.value_type != nullptr);
                    }
                }

                std::set<std::vector<dcc::types::TypePtr>> seen;
                for (auto const& inst : collector.instantiations)
                {
                    auto const& args = inst.args;

                    if (tpl_value_pack)
                        if (!inst.callee_name.empty() && inst.callee_name != template_fn->name)
                            continue;

                    if (tpl_has_pack)
                    {
                        if (args.size() < tpl_non_pack_count)
                            continue;
                    }
                    else
                    {
                        if (args.size() != template_fn->template_params.size())
                            continue;
                    }

                    dcc::infer::TemplateBindings bindings{type_ctx};
                    bool valid = true;

                    for (std::size_t i = 0; i < tpl_non_pack_count; ++i)
                    {
                        auto const& tp = template_fn->template_params[i];
                        auto param_ty =
                            type_ctx.template_param_t(const_cast<dcc::ast::TemplateParam*>(std::addressof(tp)), tp.name, static_cast<std::uint32_t>(i));

                        if (tp.value_type)
                        {
                            auto canonical_type = resolve_arg_type(tp.value_type);
                            if (!canonical_type || !param_ty)
                            {
                                valid = false;
                                break;
                            }
                            auto val = value_from_arg(args[i], canonical_type);
                            if (!val)
                            {
                                valid = false;
                                break;
                            }
                            bindings.bind_value(static_cast<dcc::types::TemplateParamType const*>(param_ty), *val);
                        }
                        else
                        {
                            dcc::types::TypePtr actual_type = resolve_arg_type(args[i].type);

                            if (!param_ty || !actual_type)
                            {
                                valid = false;
                                break;
                            }
                            if (!bindings.deduce(param_ty, actual_type))
                            {
                                valid = false;
                                break;
                            }
                        }
                    }
                    if (!valid)
                        continue;

                    if (tpl_has_pack)
                    {
                        auto const& tp = template_fn->template_params.back();
                        auto* param_ty_ptr = type_ctx.template_param_t(const_cast<dcc::ast::TemplateParam*>(std::addressof(tp)), tp.name,
                                                                       static_cast<std::uint32_t>(tpl_non_pack_count));
                        auto const* pack_param_ty = static_cast<dcc::types::TemplateParamType const*>(param_ty_ptr);

                        if (tp.value_type)
                        {
                            auto canonical_type = resolve_arg_type(tp.value_type);
                            if (!canonical_type)
                            {
                                valid = false;
                                continue;
                            }
                            std::vector<dcc::comptime::Value> pack_values;
                            for (std::size_t i = tpl_non_pack_count; i < args.size(); ++i)
                            {
                                auto val = value_from_arg(args[i], canonical_type);
                                if (!val)
                                {
                                    valid = false;
                                    break;
                                }
                                pack_values.push_back(*val);
                            }
                            if (!valid)
                                continue;

                            if (!bindings.bind_value_pack(pack_param_ty, std::move(pack_values)))
                                continue;
                        }
                        else
                        {
                            std::vector<dcc::types::TypePtr> pack_types;
                            for (std::size_t i = tpl_non_pack_count; i < args.size(); ++i)
                            {
                                auto ty = resolve_arg_type(args[i].type);
                                if (!ty)
                                {
                                    valid = false;
                                    break;
                                }
                                pack_types.push_back(ty);
                            }
                            if (!valid)
                                continue;

                            if (!bindings.bind_pack(pack_param_ty, pack_types))
                                continue;
                        }
                    }

                    std::vector<dcc::types::TypePtr> key;
                    for (auto& a : args)
                    {
                        if (a.expr && !a.type)
                            key.push_back(reinterpret_cast<dcc::types::TypePtr>(a.expr));
                        else
                            key.push_back(resolve_arg_type(a.type));
                    }

                    if (!seen.insert(key).second)
                        continue;

                    auto result = dcc::sema::instantiate_with_bindings(*template_fn, bindings, ast_ctx, type_ctx, &diag);
                    if (result.decl)
                    {
                        dcc::sema::analyze_instantiated_body(const_cast<dcc::sema::ModuleInfo&>(*mod), *result.decl, diag, ast_ctx, type_ctx, sema.allocator());
                        actual += dcc::sema::BodyDumper::dump_decl(*result.decl);
                    }
                }
            }

            auto a = normalize(actual);
            auto e = normalize(exp.body);
            if (a != e)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  instantiated-body mismatch for {}  ({}:{})", exp.module_path, path.string(), exp.base_line);
                print_diff("INST-BODY", e, a, exp.base_line);
            }
        }

        for (auto const& exp : fx.ir_blocks)
        {
            auto const* mod = sema.graph().all().empty() ? nullptr : sema.graph().all().front().get();
            if (!mod)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  EXPECT-IR: no module found  ({}:{})", path.string(), exp.base_line);
                continue;
            }

            dcc::ir::IrContext ir_ctx;
            auto lowerer = std::make_unique<dcc::ir::lower::Lowerer>(ir_ctx, &sema.spec_registry(), &sema.graph(), exp.bounds_check, &sm, &sema.types());
            auto* ir_mod = lowerer->lower_module(*mod);
            auto actual = dcc::ir::IrSerializer::dump(ir_mod);
            auto a = normalize(actual);
            auto e = normalize(exp.body);
            if (a != e)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  IR mismatch  ({}:{})", path.string(), exp.base_line);
                print_diff("IR", e, a, exp.base_line);
            }
        }

        for ([[maybe_unused]] auto const& exp : fx.llvm_blocks)
        {
#if DCC_ENABLE_LLVM
            auto const* mod = sema.graph().all().empty() ? nullptr : sema.graph().all().front().get();
            if (!mod)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  EXPECT-LLVM: no module found  ({}:{})", path.string(), exp.base_line);
                continue;
            }

            dcc::target::TargetConfig target;
            if (!exp.target_triple.empty())
            {
                auto parsed = dcc::target::TargetConfig::parse_triple(exp.target_triple);
                if (!parsed)
                {
                    ok = false;
                    std::println(std::cerr, "    FAIL  EXPECT-LLVM: unsupported target triple '{}'  ({}:{})", exp.target_triple, path.string(), exp.base_line);
                    continue;
                }
                target = *parsed;
            }
            else
                target = dcc::target::TargetConfig::host_default();

            dcc::ir::IrContext ir_ctx{256 * 1024, &target};
            auto lowerer = std::make_unique<dcc::ir::lower::Lowerer>(ir_ctx, &sema.spec_registry(), &sema.graph(), exp.bounds_check, &sm, &sema.types());
            auto* ir_mod = lowerer->lower_module(*mod);

            target.no_red_zone = exp.no_red_zone;
            target.no_simd = exp.no_simd;
            target.no_x87 = exp.no_x87;
            target.position_independent_code = exp.position_independent_code;
            if (exp.code_model)
                target.code_model = *exp.code_model;

            dcc::backend::BackendOptions backend_opts;
            backend_opts.target = target;
            backend_opts.requested_artifacts = {dcc::backend::ArtifactKind::LlvmIrText};
            backend_opts.omit_frame_pointer = exp.omit_frame_pointer;
            backend_opts.emit_debug_info = exp.emit_debug_info;
            backend_opts.debug_format = exp.debug_format;
            if (exp.emit_debug_info)
                backend_opts.source_manager = &sm;

            auto backend = dcc::backend::make_llvm_backend();
            auto artifact = backend->emit(*ir_mod, backend_opts);

            std::string actual;
            if (artifact.llvm_ir_text)
                actual = *artifact.llvm_ir_text;
            else if (!artifact.diagnostics.empty())
                actual = artifact.diagnostics[0].message;

            if (exp.is_error)
            {
                if (artifact.llvm_ir_text)
                {
                    ok = false;
                    std::println(std::cerr, "    FAIL  EXPECT-LLVM-ERRORS: expected backend error, but emission succeeded  ({}:{})", path.string(),
                                 exp.base_line);
                    print_diff("LLVM-ERROR", exp.body, actual, exp.base_line);
                    continue;
                }
            }

            auto const body_provided = !trim(exp.body).empty();
            if (body_provided)
            {
                auto a = normalize(actual);
                auto e = normalize(exp.body);
                if (a != e)
                {
                    ok = false;
                    std::println(std::cerr, "    FAIL  LLVM mismatch  ({}:{})", path.string(), exp.base_line);
                    print_diff(exp.is_error ? "LLVM-ERROR" : "LLVM", e, a, exp.base_line);
                }
            }
            else if (exp.verify && !exp.is_error && !artifact.llvm_ir_text)
            {
                ok = false;
                std::string detail = actual.empty() ? "no output" : actual;
                std::println(std::cerr, "    FAIL  EXPECT-LLVM verify-only: expected valid IR, got backend error  ({}:{})", path.string(), exp.base_line);
                std::println(std::cerr, "          detail: {}", detail);
            }

            if (!exp.is_error && artifact.llvm_ir_text && exp.verify)
            {
                auto vf_path = fs::temp_directory_path() / std::format("dcc-verify-{}.ll", std::chrono::steady_clock::now().time_since_epoch().count());
                {
                    std::ofstream vf{vf_path, std::ios::binary};
                    if (vf)
                        vf.write(actual.data(), static_cast<std::streamsize>(actual.size()));
                }

                auto cmd = std::format("llvm-as -disable-output {} 2>&1", vf_path.string());
                auto* pipe = popen(cmd.c_str(), "r");
                std::string llvm_err;
                if (pipe)
                {
                    std::array<char, 4096> buf;
                    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
                        llvm_err += buf.data();
                    auto rc = pclose(pipe);
                    std::error_code ec;
                    fs::remove(vf_path, ec);

                    if (rc != 0)
                    {
                        ok = false;
                        std::println(std::cerr, "    FAIL  LLVM verification failed (llvm-as)  ({}:{})", path.string(), exp.base_line);
                        if (!llvm_err.empty())
                        {
                            std::istringstream iss{llvm_err};
                            std::string line;
                            while (std::getline(iss, line))
                                std::println(std::cerr, "          | {}", line);
                        }
                    }
                }
                else
                {
                    std::error_code ec;
                    fs::remove(vf_path, ec);
                    std::println(std::cerr, "    WARN  llvm-as not available, skipping verification  ({}:{})", path.string(), exp.base_line);
                }
            }
#else
            std::println("  SKIP  LLVM backend not available");
            ++stats.skipped;
            break;
#endif
        }

        for ([[maybe_unused]] auto const& exp : fx.executable_blocks)
        {
#if DCC_ENABLE_LLVM
            auto const* mod = sema.graph().all().empty() ? nullptr : sema.graph().all().front().get();
            if (!mod)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  EXPECT-EXECUTABLE: no module found  ({}:{})", path.string(), exp.base_line);
                continue;
            }

            dcc::target::TargetConfig target;
            if (!exp.target_triple.empty())
            {
                auto parsed = dcc::target::TargetConfig::parse_triple(exp.target_triple);
                if (!parsed)
                {
                    ok = false;
                    std::println(std::cerr, "    FAIL  EXPECT-EXECUTABLE: unsupported target triple '{}'  ({}:{})", exp.target_triple, path.string(),
                                 exp.base_line);
                    continue;
                }
                target = *parsed;
            }
            else
                target = dcc::target::TargetConfig::host_default();

            dcc::ir::IrContext ir_ctx{256 * 1024, &target};
            auto lowerer = std::make_unique<dcc::ir::lower::Lowerer>(ir_ctx, &sema.spec_registry(), &sema.graph(), false, &sm, &sema.types());
            auto* ir_mod = lowerer->lower_module(*mod);

            target.no_red_zone = exp.no_red_zone;
            target.no_simd = exp.no_simd;
            target.no_x87 = exp.no_x87;
            target.position_independent_code = exp.position_independent_code;
            if (exp.code_model)
                target.code_model = *exp.code_model;

            dcc::backend::BackendOptions backend_opts;
            backend_opts.target = target;
            backend_opts.requested_artifacts = {dcc::backend::ArtifactKind::ExecutableBytes};
            backend_opts.omit_frame_pointer = exp.omit_frame_pointer;

            auto backend = dcc::backend::make_llvm_backend();
            auto artifact = backend->emit(*ir_mod, backend_opts);

            if (exp.is_error)
            {
                if (artifact.executable_bytes)
                {
                    ok = false;
                    std::println(std::cerr, "    FAIL  EXPECT-EXECUTABLE-ERRORS: expected backend error, but emission succeeded  ({}:{})", path.string(),
                                 exp.base_line);
                    continue;
                }
                if (artifact.diagnostics.empty())
                {
                    ok = false;
                    std::println(std::cerr, "    FAIL  EXPECT-EXECUTABLE-ERRORS: expected backend error, but none produced  ({}:{})", path.string(),
                                 exp.base_line);
                }
            }
            else
            {
                if (!artifact.executable_bytes)
                {
                    ok = false;
                    std::println(std::cerr, "    FAIL  EXPECT-EXECUTABLE-VALID: no executable bytes produced  ({}:{})", path.string(), exp.base_line);
                    if (!artifact.diagnostics.empty())
                    {
                        for (auto const& d : artifact.diagnostics)
                            std::println(std::cerr, "          backend diagnostic: {}", d.message);
                    }
                    continue;
                }

                auto const& exe = *artifact.executable_bytes;

                if (exe.size() < 64)
                {
                    ok = false;
                    std::println(std::cerr, "    FAIL  EXPECT-EXECUTABLE-VALID: executable too small ({} bytes)  ({}:{})", exe.size(), path.string(),
                                 exp.base_line);
                    continue;
                }

                bool elf_valid = true;
                if (static_cast<int>(exe[0]) != 0x7f || static_cast<int>(exe[1]) != 'E' || static_cast<int>(exe[2]) != 'L' || static_cast<int>(exe[3]) != 'F')
                {
                    elf_valid = false;
                    std::println(std::cerr, "    FAIL  EXPECT-EXECUTABLE-VALID: bad ELF magic  ({}:{})", path.string(), exp.base_line);
                }

                if (static_cast<int>(exe[4]) != 2)
                {
                    elf_valid = false;
                    std::println(std::cerr, "    FAIL  EXPECT-EXECUTABLE-VALID: EI_CLASS != 2 (64-bit)  ({}:{})", path.string(), exp.base_line);
                }

                auto e_type =
                    static_cast<std::uint16_t>(static_cast<unsigned char>(exe[16])) | (static_cast<std::uint16_t>(static_cast<unsigned char>(exe[17])) << 8);
                if (e_type != 2)
                {
                    elf_valid = false;
                    std::println(std::cerr, "    FAIL  EXPECT-EXECUTABLE-VALID: e_type != ET_EXEC (2), got {}  ({}:{})", e_type, path.string(), exp.base_line);
                }

                auto e_machine =
                    static_cast<std::uint16_t>(static_cast<unsigned char>(exe[18])) | (static_cast<std::uint16_t>(static_cast<unsigned char>(exe[19])) << 8);
                if (e_machine != 0x3E)
                {
                    elf_valid = false;
                    std::println(std::cerr, "    FAIL  EXPECT-EXECUTABLE-VALID: e_machine != 0x3E (x86-64), got {:#x}  ({}:{})", e_machine, path.string(),
                                 exp.base_line);
                }

                if (!elf_valid)
                    ok = false;
            }
#else
            std::println("  SKIP  LLVM backend not available");
            ++stats.skipped;
            break;
#endif
        }

        for (auto const& exp : fx.em64t_object_blocks)
        {
            auto const* mod = sema.graph().all().empty() ? nullptr : sema.graph().all().front().get();
            if (!mod)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT: no module found  ({}:{})", path.string(), exp.base_line);
                continue;
            }

            dcc::target::TargetConfig target = dcc::target::TargetConfig::host_default();
            dcc::ir::IrContext ir_ctx{256 * 1024, &target};
            auto lowerer = std::make_unique<dcc::ir::lower::Lowerer>(ir_ctx, &sema.spec_registry(), &sema.graph(), false, &sm, &sema.types());
            auto* ir_mod = lowerer->lower_module(*mod);

            if (exp.pic)
                target.position_independent_code = true;

            dcc::backend::BackendOptions backend_opts;
            backend_opts.target = target;
            backend_opts.requested_artifacts = {dcc::backend::ArtifactKind::ObjectBytes};
            backend_opts.opt_level = static_cast<dcc::ir::pass::OptLevel>(exp.opt_level);

            auto backend = dcc::backend::make_em64t_backend();
            auto artifact = backend->emit(*ir_mod, backend_opts);

            if (!artifact.object_bytes)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT: no object bytes produced  ({}:{})", path.string(), exp.base_line);
                if (!artifact.diagnostics.empty())
                {
                    for (auto const& d : artifact.diagnostics)
                        std::println(std::cerr, "          backend diagnostic: {}", d.message);
                }
                continue;
            }

            auto const& obj = *artifact.object_bytes;

            if (obj.size() < 64)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT: object too small ({} bytes)  ({}:{})", obj.size(), path.string(), exp.base_line);
                continue;
            }

            bool elf_valid = true;
            if (static_cast<int>(obj[0]) != 0x7f || static_cast<int>(obj[1]) != 'E' || static_cast<int>(obj[2]) != 'L' || static_cast<int>(obj[3]) != 'F')
            {
                elf_valid = false;
                std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT: bad ELF magic  ({}:{})", path.string(), exp.base_line);
            }

            if (static_cast<int>(obj[4]) != 2)
            {
                elf_valid = false;
                std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT: EI_CLASS != 2 (64-bit)  ({}:{})", path.string(), exp.base_line);
            }

            if (exp.verify_sections && elf_valid && obj.size() >= 64)
            {
                auto rd_elf64 = [&](std::size_t off, unsigned bytes) -> std::uint64_t {
                    std::uint64_t v = 0;
                    for (unsigned i = 0; i < bytes && off + i < obj.size(); ++i)
                        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(obj[off + i])) << (i * 8);
                    return v;
                };

                std::uint64_t e_shoff = rd_elf64(40, 8);
                std::uint64_t e_shentsize = rd_elf64(58, 2);
                std::uint64_t e_shnum = rd_elf64(60, 2);
                std::uint64_t e_shstrndx = rd_elf64(62, 2);

                if (e_shentsize < 64 || e_shnum < 3 || e_shstrndx >= e_shnum)
                {
                    elf_valid = false;
                    std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT: malformed section headers (shnum={}, shentsize={}, shstrndx={})  ({}:{})", e_shnum,
                                 e_shentsize, e_shstrndx, path.string(), exp.base_line);
                }
                else
                {
                    std::uint64_t shdr_size = e_shentsize;
                    std::uint64_t shdr_table_end = e_shoff + e_shnum * shdr_size;
                    if (shdr_table_end > obj.size())
                    {
                        elf_valid = false;
                        std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT: section headers extend past end of file  ({}:{})", path.string(),
                                     exp.base_line);
                    }
                    else
                    {
                        std::uint64_t shstr_off = e_shoff + e_shstrndx * shdr_size;
                        std::uint64_t shstr_sh_offset = rd_elf64(static_cast<std::size_t>(shstr_off + 24), 8);
                        std::uint64_t shstr_sh_size = rd_elf64(static_cast<std::size_t>(shstr_off + 32), 8);
                        if (shstr_sh_offset + shstr_sh_size > obj.size())
                        {
                            elf_valid = false;
                            std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT: shstrtab extends past end of file  ({}:{})", path.string(), exp.base_line);
                        }
                        else
                        {
                            auto get_sec_name = [&](std::uint64_t sec_idx) -> std::string {
                                if (sec_idx >= e_shnum)
                                    return {};
                                std::uint64_t sdoff = e_shoff + sec_idx * shdr_size;
                                std::uint64_t name_off = rd_elf64(static_cast<std::size_t>(sdoff), 4);
                                if (name_off >= shstr_sh_offset + shstr_sh_size)
                                    return {};
                                std::string name;
                                for (std::uint64_t p = shstr_sh_offset + name_off;
                                     p < shstr_sh_offset + shstr_sh_size && static_cast<unsigned char>(obj[static_cast<std::size_t>(p)]) != 0; ++p)
                                    name += static_cast<char>(obj[static_cast<std::size_t>(p)]);
                                return name;
                            };

                            bool has_text = false, has_rodata = false, has_data = false, has_bss = false;
                            for (std::uint64_t si = 0; si < e_shnum; ++si)
                            {
                                auto sname = get_sec_name(si);
                                if (sname == ".text")
                                    has_text = true;
                                else if (sname == ".rodata")
                                    has_rodata = true;
                                else if (sname == ".data")
                                    has_data = true;
                                else if (sname == ".bss")
                                    has_bss = true;
                            }

                            if (!has_text)
                            {
                                elf_valid = false;
                                std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT: missing .text section  ({}:{})", path.string(), exp.base_line);
                            }
                            if (!has_rodata)
                            {
                                elf_valid = false;
                                std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT: missing .rodata section  ({}:{})", path.string(), exp.base_line);
                            }
                            if (!has_data)
                            {
                                elf_valid = false;
                                std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT: missing .data section  ({}:{})", path.string(), exp.base_line);
                            }
                            if (!has_bss)
                            {
                                elf_valid = false;
                                std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT: missing .bss section  ({}:{})", path.string(), exp.base_line);
                            }
                        }
                    }
                }
            }

            if (elf_valid && exp.pic && exp.verify_sections && obj.size() >= 64)
            {
                auto rd_elf64 = [&](std::size_t off, unsigned bytes) -> std::uint64_t {
                    std::uint64_t v = 0;
                    for (unsigned i = 0; i < bytes && off + i < obj.size(); ++i)
                        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(obj[static_cast<std::size_t>(off + i)])) << (i * 8);
                    return v;
                };
                std::uint64_t e_shoff_pic = rd_elf64(40, 8);
                std::uint64_t e_shentsize_pic = rd_elf64(58, 2);
                std::uint64_t e_shnum_pic = rd_elf64(60, 2);
                std::uint64_t e_shstrndx_pic = rd_elf64(62, 2);

                if (e_shentsize_pic >= 64 && e_shnum_pic >= 2 && e_shstrndx_pic < e_shnum_pic)
                {
                    std::uint64_t shstr_off_pic = e_shoff_pic + e_shstrndx_pic * e_shentsize_pic;
                    std::uint64_t shstr_sh_offset_pic = rd_elf64(static_cast<std::size_t>(shstr_off_pic + 24), 8);
                    std::uint64_t shstr_sh_size_pic = rd_elf64(static_cast<std::size_t>(shstr_off_pic + 32), 8);

                    auto get_sec_name_pic = [&](std::uint64_t sec_idx) -> std::string {
                        if (sec_idx >= e_shnum_pic)
                            return {};

                        std::uint64_t sdoff = e_shoff_pic + sec_idx * e_shentsize_pic;
                        std::uint64_t name_off = rd_elf64(static_cast<std::size_t>(sdoff), 4);
                        if (name_off >= shstr_sh_offset_pic + shstr_sh_size_pic)
                            return {};

                        std::string name;
                        for (std::uint64_t p = shstr_sh_offset_pic + name_off;
                             p < shstr_sh_offset_pic + shstr_sh_size_pic && static_cast<unsigned char>(obj[static_cast<std::size_t>(p)]) != 0; ++p)
                            name += static_cast<char>(obj[static_cast<std::size_t>(p)]);

                        return name;
                    };

                    constexpr std::uint32_t R_X86_64_64 = 1;
                    constexpr std::uint32_t R_X86_64_PC32 = 2;
                    constexpr std::uint32_t R_X86_64_PLT32 = 4;
                    constexpr std::uint32_t R_X86_64_GOTPCREL = 9;

                    auto is_allowed_text_reloc = [](std::uint32_t t) -> bool { return t == R_X86_64_PLT32 || t == R_X86_64_PC32 || t == R_X86_64_GOTPCREL; };

                    struct RelaCount
                    {
                        std::uint32_t type;
                        std::uint64_t count;
                    };
                    std::unordered_map<std::string, std::vector<RelaCount>> sec_rela_counts;

                    std::uint64_t sec_text_rela = 0, sec_rodata_rela = 0;
                    for (std::uint64_t si = 0; si < e_shnum_pic; ++si)
                    {
                        std::uint64_t sdoff = e_shoff_pic + si * e_shentsize_pic;
                        std::uint64_t sh_type = rd_elf64(static_cast<std::size_t>(sdoff + 4), 4);
                        std::uint64_t sh_info = rd_elf64(static_cast<std::size_t>(sdoff + 44), 4);

                        if (sh_type == 4 && sh_info > 0 && sh_info < e_shnum_pic)
                        {
                            auto tname = get_sec_name_pic(sh_info);
                            if (tname == ".text")
                                sec_text_rela = si;
                            else if (tname == ".rodata")
                                sec_rodata_rela = si;
                        }
                    }

                    auto check_section_relas = [&](std::uint64_t sec_rela_idx, std::string const& sec_name, bool forbid_abs64, bool restrict_types) -> bool {
                        if (sec_rela_idx == 0)
                            return true;

                        std::uint64_t rela_off = rd_elf64(static_cast<std::size_t>(e_shoff_pic + sec_rela_idx * e_shentsize_pic + 24), 8);
                        std::uint64_t rela_size = rd_elf64(static_cast<std::size_t>(e_shoff_pic + sec_rela_idx * e_shentsize_pic + 32), 8);
                        std::uint64_t num_relas = rela_size / 24;
                        bool ok = true;

                        for (std::uint64_t ri = 0; ri < num_relas; ++ri)
                        {
                            std::uint64_t roff = rela_off + ri * 24;
                            if (roff + 24 > obj.size())
                                break;

                            std::uint64_t r_info = rd_elf64(static_cast<std::size_t>(roff + 8), 8);
                            std::uint32_t r_type = static_cast<std::uint32_t>(r_info & 0xFFFFFFFF);

                            auto& counts = sec_rela_counts[sec_name];
                            auto it = std::ranges::find_if(counts, [&](auto const& c) { return c.type == r_type; });
                            if (it != counts.end())
                                it->count++;
                            else
                                counts.push_back({r_type, 1});

                            if (forbid_abs64 && r_type == R_X86_64_64)
                            {
                                ok = false;
                                elf_valid = false;
                                std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT PIC: R_X86_64_64 found in {}  ({}:{})", sec_name, path.string(),
                                             exp.base_line);
                            }

                            if (restrict_types && !is_allowed_text_reloc(r_type))
                            {
                                ok = false;
                                elf_valid = false;
                                std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT PIC: disallowed relocation type {} found in .text  ({}:{})", r_type,
                                             path.string(), exp.base_line);
                            }
                        }
                        return ok;
                    };

                    check_section_relas(sec_text_rela, ".text", true, true);
                    check_section_relas(sec_rodata_rela, ".rodata", true, false);

                    for (auto const& rr : exp.required_relas)
                    {
                        auto it = sec_rela_counts.find(rr.section);
                        if (it == sec_rela_counts.end())
                        {
                            elf_valid = false;
                            std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT PIC: no relocations found in {} to satisfy REQUIRE-RELA: {} {}  ({}:{})",
                                         rr.section, rr.section, rr.rel_type, path.string(), exp.base_line);
                            continue;
                        }
                        auto cit = std::ranges::find_if(it->second, [&](auto const& c) { return c.type == rr.rel_type; });
                        if (cit == it->second.end() || cit->count == 0)
                        {
                            elf_valid = false;
                            std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT PIC: REQUIRE-RELA: {} {} not found  ({}:{})", rr.section, rr.rel_type,
                                         path.string(), exp.base_line);
                        }
                    }
                }
            }

            if (!elf_valid)
                ok = false;

            if (elf_valid && exp.run_exit_code.has_value())
            {
                std::error_code ec;
                auto run_dir = fs::temp_directory_path(ec) / std::format("dcc-em64t-run-{}", std::chrono::steady_clock::now().time_since_epoch().count());
                if (ec)
                {
                    ok = false;
                    std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT: cannot create temp dir  ({}:{})", path.string(), exp.base_line);
                    continue;
                }

                if (!fs::create_directories(run_dir, ec))
                {
                    ok = false;
                    std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT: cannot create temp dir  ({}:{})", path.string(), exp.base_line);
                    continue;
                }

                auto obj_path = run_dir / "test.o";
                {
                    std::ofstream of{obj_path, std::ios::binary};
                    if (!of)
                    {
                        ok = false;
                        std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT: cannot write object file  ({}:{})", path.string(), exp.base_line);
                        fs::remove_all(run_dir, ec);
                        continue;
                    }
                    auto const* obj_data = reinterpret_cast<char const*>(obj.data());
                    of.write(obj_data, static_cast<std::streamsize>(obj.size()));
                }

                auto asm_path = run_dir / "start.s";
                {
                    std::ofstream of{asm_path};
                    if (!of)
                    {
                        ok = false;
                        std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT: cannot write harness assembly  ({}:{})", path.string(), exp.base_line);
                        fs::remove_all(run_dir, ec);
                        continue;
                    }
                    of << ".text\n"
                       << ".globl _start\n"
                       << "_start:\n"
                       << "    call dcc_main\n"
                       << "    mov %eax, %edi\n"
                       << "    mov $60, %eax\n"
                       << "    syscall\n";
                }

                auto harness_obj_path = run_dir / "start.o";
                auto exe_path = run_dir / "test_prog";

                auto asm_cmd = std::format("clang -x assembler -c {} -o {} 2>/dev/null", asm_path.string(), harness_obj_path.string());
                int asm_rc = std::system(asm_cmd.c_str());
                if (asm_rc != 0)
                {
                    asm_cmd = std::format("as {} -o {} 2>/dev/null", asm_path.string(), harness_obj_path.string());
                    asm_rc = std::system(asm_cmd.c_str());
                }
                if (asm_rc != 0)
                {
                    ok = false;
                    std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT: cannot assemble test harness (no assembler)  ({}:{})", path.string(),
                                 exp.base_line);
                    fs::remove_all(run_dir, ec);
                    continue;
                }

                auto link_cmd = std::format("ld.lld --static --no-dynamic-linker --fatal-warnings "
                                            "-e _start -o {} {} {} 2>/dev/null",
                                            exe_path.string(), obj_path.string(), harness_obj_path.string());
                int link_rc = std::system(link_cmd.c_str());
                if (link_rc != 0)
                {
                    ok = false;
                    std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT: linking failed  ({}:{})", path.string(), exp.base_line);
                    fs::remove_all(run_dir, ec);
                    continue;
                }

                auto run_cmd = std::format("{} 2>/dev/null", exe_path.string());
                int run_rc = std::system(run_cmd.c_str());

                int actual_exit;
#ifdef _WIN32
                actual_exit = run_rc;
#else
                if (WIFEXITED(run_rc))
                    actual_exit = WEXITSTATUS(run_rc);
                else
                    actual_exit = -1;
#endif

                if (actual_exit != *exp.run_exit_code)
                {
                    ok = false;
                    std::println(std::cerr, "    FAIL  EXPECT-EM64T-OBJECT: expected exit code {}, got {}  ({}:{})", *exp.run_exit_code, actual_exit,
                                 path.string(), exp.base_line);
                }

                fs::remove_all(run_dir, ec);
            }
        }

        for (auto const& exp : fx.em64t_asm_blocks)
        {
            auto const* mod = sema.graph().all().empty() ? nullptr : sema.graph().all().front().get();
            if (!mod)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  EXPECT-EM64T-ASM: no module found  ({}:{})", path.string(), exp.base_line);
                continue;
            }

            dcc::target::TargetConfig target = dcc::target::TargetConfig::host_default();
            dcc::ir::IrContext ir_ctx{256 * 1024, &target};
            auto lowerer = std::make_unique<dcc::ir::lower::Lowerer>(ir_ctx, &sema.spec_registry(), &sema.graph(), false, &sm, &sema.types());
            auto* ir_mod = lowerer->lower_module(*mod);

            dcc::backend::BackendOptions backend_opts;
            backend_opts.target = target;
            backend_opts.requested_artifacts = {dcc::backend::ArtifactKind::AsmText};
            backend_opts.opt_level = static_cast<dcc::ir::pass::OptLevel>(exp.opt_level);

            auto backend = dcc::backend::make_em64t_backend();
            auto artifact = backend->emit(*ir_mod, backend_opts);

            if (!artifact.asm_text)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  EXPECT-EM64T-ASM: no asm text produced  ({}:{})", path.string(), exp.base_line);
                if (!artifact.diagnostics.empty())
                {
                    for (auto const& d : artifact.diagnostics)
                        std::println(std::cerr, "          backend diagnostic: {}", d.message);
                }
                continue;
            }

            auto const& asm_text = *artifact.asm_text;

            for (auto const& pat : exp.contains)
                if (asm_text.find(pat) == std::string::npos)
                {
                    ok = false;
                    std::println(std::cerr, "    FAIL  EXPECT-EM64T-ASM: missing pattern '{}' in asm output  ({}:{})", pat, path.string(), exp.base_line);
                }

            std::error_code ec;
            auto asm_dir = fs::temp_directory_path(ec) / std::format("dcc-asm-test-{}", std::chrono::steady_clock::now().time_since_epoch().count());
            if (!ec && fs::create_directories(asm_dir, ec))
            {
                auto asm_path = asm_dir / "test.s";
                {
                    std::ofstream of{asm_path, std::ios::binary};
                    if (of)
                        of.write(asm_text.data(), static_cast<std::streamsize>(asm_text.size()));
                }

                int have_nasm = std::system("command -v nasm >/dev/null 2>&1");
                if (have_nasm != 0)
                    std::println("    WARN  nasm not available, skipping assembly check  ({}:{})", path.string(), exp.base_line);
                else
                {
                    auto nasm_cmd = std::format("nasm -f elf64 -o /dev/null {} 2>&1", asm_path.string());
                    auto* pipe = popen(nasm_cmd.c_str(), "r");
                    if (pipe)
                    {
                        std::string nasm_err;
                        std::array<char, 4096> buf;
                        while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
                            nasm_err += buf.data();
                        auto rc = pclose(pipe);

                        if (rc == 0)
                            ;
                        else
                        {
                            ok = false;
                            std::println(std::cerr, "    FAIL  EXPECT-EM64T-ASM: nasm assembly failed (exit code {})  ({}:{})", rc, path.string(),
                                         exp.base_line);
                            if (!nasm_err.empty())
                            {
                                std::istringstream iss{nasm_err};
                                std::string line;
                                while (std::getline(iss, line))
                                    std::println(std::cerr, "          nasm: {}", line);
                            }
                        }
                    }
                    else
                    {
                        ok = false;
                        std::println(std::cerr, "    FAIL  EXPECT-EM64T-ASM: cannot run nasm  ({}:{})", path.string(), exp.base_line);
                    }
                }

                fs::remove_all(asm_dir, ec);
            }
        }

        for (auto const& exp : fx.registry_blocks)
        {
            auto actual = sema.spec_registry().dump();
            auto a = normalize(actual);
            auto e = normalize(exp.body);
            if (a != e)
            {
                ok = false;
                std::println(std::cerr, "    FAIL  registry mismatch  ({}:{})", path.string(), exp.base_line);
                print_diff("REGISTRY", e, a, exp.base_line);
            }
        }

        if (!ok)
            ++stats.failed;

        return ok;
    }

    fs::path locate_cases_dir()
    {
        std::array<fs::path, 4> candidates{
            fs::path{"tests/cases"},
            fs::path{"cases"},
            fs::path{"../tests/cases"},
            fs::path{"../../tests/cases"},
        };

        for (auto const& c : candidates)
        {
            std::error_code ec;
            if (fs::is_directory(c, ec))
                return fs::canonical(c, ec);
        }

        std::error_code ec;
        auto cur = fs::current_path(ec);
        for (int i = 0; i < 6 && !cur.empty(); ++i)
        {
            auto candidate = cur / "tests" / "cases";
            if (fs::is_directory(candidate, ec))
                return fs::canonical(candidate, ec);

            auto parent = cur.parent_path();
            if (parent == cur)
                break;

            cur = parent;
        }

        return {};
    }

    std::vector<fs::path> collect_case_files(fs::path const& dir)
    {
        std::vector<fs::path> out;
        if (dir.empty())
            return out;

        std::error_code ec;
        for (auto const& entry : fs::recursive_directory_iterator(dir, ec))
        {
            if (ec)
                break;

            if (!entry.is_regular_file())
                continue;

            if (entry.path().extension() == ".dcc-test")
                out.push_back(entry.path());
        }

        std::ranges::sort(out);
        return out;
    }

    std::string case_label(fs::path const& file, fs::path const& dir)
    {
        std::error_code ec;
        auto rel = fs::relative(file, dir, ec);
        if (ec)
            return file.filename().string();

        auto s = rel.string();
        if (auto ext = rel.extension().string(); !ext.empty() && s.size() >= ext.size())
            s.resize(s.size() - ext.size());

        return s;
    }

} // namespace

int main()
{
    Stats stats;
    int cases_failed = 0;

    auto dir = locate_cases_dir();
    if (dir.empty())
    {
        std::println(std::cerr, "  FAIL  cases directory not found");

        return 1;
    }

    auto files = collect_case_files(dir);

    if (files.empty())
    {
        std::println(std::cerr, "    FAIL  no .dcc-test files");
        return 1;
    }

    for (auto const& f : files)
    {
        auto label = case_label(f, dir);
        std::println(" --- test: {} ---", label);

#if !DCC_ENABLE_LLVM
        if (label.starts_with("llvm/") || label == "llvm")
        {
            std::println("  SKIP  LLVM backend not available");
            ++stats.skipped;
            continue;
        }
#endif

        if (!run_fixture(f, stats))
            ++cases_failed;
    }

    if (cases_failed > 0)
    {
        std::println(std::cerr, "  RESULT  {}/{} checks failed", stats.failed, stats.checks);
        return 1;
    }

    if (stats.skipped > 0)
        std::println("  RESULT  {}/{} checks passed ({} skipped: LLVM disabled)", stats.checks, stats.checks, stats.skipped);
    else
        std::println("  RESULT  {}/{} checks passed", stats.checks, stats.checks);

    return 0;
}
