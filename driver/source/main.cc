import std;

import dcc.sm;
import dcc.ast;
import dcc.ast.serializer;
import dcc.lex;
import dcc.si;
import dcc.parser;
import dcc.diag;
import dcc.sema;
import dcc.ir;
import dcc.ir.pass;
import dcc.ir.lower;

import dcc.target;
import dcc.config;
import dcc.backend;
import dcc.session;
import dcc.cli;
#if DCC_ENABLE_LLVM
import dcc.backend.llvm;
#endif
import dcc.backend.em64t;
import dcc.backend.i8086;
import dcc.backend.i8086.link;
import dcc.backend.object.archive;

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN
#define NOUSER
#define NOGDI
#include <windows.h>
#endif

#include "compdb.hh"

#include <array>
#include <cstdio>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace
{
    using dcc::cli::Arg;
    using dcc::cli::CliError;
    using dcc::cli::k_options;
    using dcc::cli::OptionSpec;
    using dcc::cli::Options;
    using dcc::cli::Phase;

    [[nodiscard]] dcc::target::TargetConfig resolve_target_or_exit(Options const& opts)
    {
        std::string error;
        auto target = dcc::cli::resolve_target_config(opts, error);
        if (!target)
        {
            std::println(std::cerr, "dcc: error: {}", error);
            std::exit(1);
        }
        return *target;
    }

    [[nodiscard]] int get_terminal_width()
    {
        if (char const* columns = std::getenv("COLUMNS"))
        {
            int parsed = 0;
            for (char const* p = columns; *p >= '0' && *p <= '9'; ++p)
                parsed = parsed * 10 + (*p - '0');

            if (parsed > 0)
                return parsed;
        }

#ifndef _WIN32
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) != -1)
            return w.ws_col;
#endif
        return 80;
    }

    [[nodiscard]] std::string spec_flag_text(OptionSpec const& spec)
    {
        std::string out;

        if (!spec.negated.empty() && spec.name.starts_with("-f") && spec.negated == std::format("-fno-{}", spec.name.substr(2)))
            out = std::format("-f[no-]{}", spec.name.substr(2));
        else if (!spec.negated.empty())
            out = std::format("{} | {}", spec.name, spec.negated);
        else
            out = std::string{spec.name};

        if (spec.arg == Arg::None)
            return out;

        if (spec.arg == Arg::Glued || spec.arg == Arg::Joined)
            return out + std::string{spec.metavar};

        if (spec.arg == Arg::Optional)
            return std::format("{} [{}]", out, spec.metavar);

        return std::format("{} {}", out, spec.metavar);
    }

    [[nodiscard]] std::string spec_desc_text(OptionSpec const& spec)
    {
        std::string out{spec.help};

        if (!spec.choices.empty())
            out += std::format(" (one of: {})", choice_list(spec));

        if (!spec.aliases.empty())
        {
            out += spec.aliases.size() == 1 ? " (alias: " : " (aliases: ";
            for (std::size_t k = 0; k < spec.aliases.size(); ++k)
            {
                if (k)
                    out += ", ";
                out += spec.aliases[k];
            }
            out += ")";
        }

        if (!spec.default_text.empty())
            out += std::format(" [default: {}]", spec.default_text);

        return out;
    }

    void print_usage()
    {
        int const term_width = get_terminal_width();
        int const flag_col_width = 26;
        int const desc_col_width = std::max(20, term_width - flag_col_width - 4);

        std::println("usage: dcc [options] <input-file>");
        std::println("       dcc -o <output> <input1.o> [<input2.o> ...] [link options]");
        std::println("       dcc --merge-compdb <out> [<entry files> ...]");
        std::println("");
        std::println("options:");

        for (auto const& spec : k_options)
        {
            auto const flag = spec_flag_text(spec);
            auto const desc_text = spec_desc_text(spec);

            if (flag.length() >= static_cast<std::size_t>(flag_col_width))
            {
                std::println("  {}", flag);
                std::print("{:<{}}", "", flag_col_width + 2);
            }
            else
                std::print("  {:<{}}", flag, flag_col_width);

            std::string_view desc = desc_text;
            bool first_line = true;

            while (!desc.empty())
            {
                if (!first_line)
                    std::print("{:<{}}", "", flag_col_width + 2);

                if (desc.length() <= static_cast<std::size_t>(desc_col_width))
                {
                    std::println("{}", desc);
                    break;
                }

                auto wrap_pos = desc.find_last_of(" \t", static_cast<std::size_t>(desc_col_width));
                if (wrap_pos == std::string_view::npos)
                    wrap_pos = static_cast<std::size_t>(desc_col_width);

                std::println("{}", desc.substr(0, wrap_pos));

                desc = desc.substr(wrap_pos);
                auto first_non_space = desc.find_first_not_of(" \t");
                if (first_non_space != std::string_view::npos)
                    desc = desc.substr(first_non_space);
                else
                    desc = "";

                first_line = false;
            }
        }
    }

    [[nodiscard]] std::filesystem::path output_base(Options const& opts, std::filesystem::path const& input_path)
    {
        if (!opts.output_file.empty())
        {
            auto ext = opts.output_file.extension().string();
            if (ext == ".ll" || ext == ".mir" || ext == ".s" || ext == ".o" || ext == ".a" || ext == ".so" || ext == ".dll")
            {
                auto base = opts.output_file;
                base.replace_extension("");
                return base;
            }

            return opts.output_file;
        }

        return input_path.stem();
    }

    [[nodiscard]] std::optional<std::filesystem::path> primary_output_path(Options const& opts, std::filesystem::path const& input_path,
                                                                           dcc::target::TargetConfig const& target)
    {
        auto base = output_base(opts, input_path);

        if (opts.emit_asm_only)
        {
            auto path = opts.output_file.empty() ? base : opts.output_file;
            if (opts.output_file.empty())
                path += ".s";
            return path;
        }

        if (opts.compile_only)
        {
            auto path = opts.output_file.empty() ? base : opts.output_file;
            if (opts.output_file.empty())
                path += ".o";
            return path;
        }

        if (opts.dump_llvm || opts.dump_mir)
            return opts.output_file.empty() ? std::nullopt : std::optional{opts.output_file};

        if (!opts.output_file.empty())
            return opts.output_file;

        if (opts.shared_library)
        {
            auto path = base;
            if (target.object_format == dcc::target::ObjectFormat::Coff)
                path += ".dll";
            else
                path += ".so";
            return path;
        }

        auto path = base;
        path.replace_extension("");
        return path;
    }

    [[nodiscard]] std::optional<dcc::backend::ArtifactKind> artifact_kind_for_extension(std::string_view ext)
    {
        if (ext == ".ll")
            return dcc::backend::ArtifactKind::LlvmIrText;
        if (ext == ".mir")
            return dcc::backend::ArtifactKind::MirText;
        if (ext == ".s")
            return dcc::backend::ArtifactKind::AsmText;
        if (ext == ".o")
            return dcc::backend::ArtifactKind::ObjectBytes;
        if (ext == ".a")
            return dcc::backend::ArtifactKind::ArchiveBytes;
        if (ext == ".so" || ext == ".dll")
            return dcc::backend::ArtifactKind::SharedLibraryBytes;
        return std::nullopt;
    }

    bool write_file(std::filesystem::path const& path, std::string_view content)
    {
        std::ofstream out{path, std::ios::binary};
        if (!out)
            return false;

        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        return true;
    }

    bool write_file(std::filesystem::path const& path, std::span<std::byte const> data)
    {
        std::ofstream out{path, std::ios::binary};
        if (!out)
            return false;

        out.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
        return true;
    }

    bool write_artifacts(dcc::backend::BackendArtifact const& artifact, Options const& opts, std::filesystem::path const& input_path,
                         dcc::target::TargetConfig const& target, std::filesystem::path* primary_out = nullptr)
    {
        auto base = output_base(opts, input_path);
        auto primary = primary_output_path(opts, input_path, target);

        if (primary_out)
            primary_out->clear();

        auto do_write = [&](std::filesystem::path const& path, auto const& content) -> bool {
            if (!write_file(path, content))
            {
                std::println(std::cerr, "dcc: error: cannot write to '{}'", path.string());
                return false;
            }
            return true;
        };

        if (opts.emit_asm_only)
        {
            if (artifact.asm_text)
            {
                if (primary_out)
                    *primary_out = *primary;
                return do_write(*primary, *artifact.asm_text);
            }
            return true;
        }

        if (opts.compile_only)
        {
            if (artifact.object_bytes)
            {
                if (primary_out)
                    *primary_out = *primary;
                return do_write(*primary, *artifact.object_bytes);
            }
            return true;
        }

        if (opts.dump_llvm || opts.dump_mir)
        {
            bool const has_llvm = artifact.llvm_ir_text.has_value();
            bool const has_mir = artifact.mir_text.has_value();

            if (opts.output_file.empty())
            {
                if (has_llvm)
                    std::print("{}", *artifact.llvm_ir_text);
                if (has_mir)
                    std::print("{}", *artifact.mir_text);
            }
            else if (has_llvm && has_mir)
            {
                auto dump_ext = opts.output_file.extension().string();
                if (dump_ext == ".mir")
                {
                    if (!do_write(opts.output_file, *artifact.mir_text))
                        return false;

                    if (primary_out)
                        *primary_out = opts.output_file;

                    auto llvm_path = base;
                    llvm_path += ".ll";
                    if (!do_write(llvm_path, *artifact.llvm_ir_text))
                        return false;
                }
                else
                {
                    if (!do_write(opts.output_file, *artifact.llvm_ir_text))
                        return false;

                    if (primary_out)
                        *primary_out = opts.output_file;

                    auto mir_path = base;
                    mir_path += ".mir";
                    if (!do_write(mir_path, *artifact.mir_text))
                        return false;
                }
            }
            else if (has_llvm)
            {
                if (!do_write(opts.output_file, *artifact.llvm_ir_text))
                    return false;

                if (primary_out)
                    *primary_out = opts.output_file;
            }
            else if (has_mir)
            {
                if (!do_write(opts.output_file, *artifact.mir_text))
                    return false;

                if (primary_out)
                    *primary_out = opts.output_file;
            }
            return true;
        }

        bool ok = true;
        bool mir_written_via_extension = false;
        bool asm_written_via_extension = false;
        bool obj_written_via_extension = false;
        bool llvm_written_via_extension = false;
        bool archive_written_via_extension = false;

        if (!opts.output_file.empty())
        {
            auto ext = opts.output_file.extension().string();
            auto kind = artifact_kind_for_extension(ext);
            if (kind)
            {
                switch (*kind)
                {
                    case dcc::backend::ArtifactKind::LlvmIrText:
                        if (artifact.llvm_ir_text)
                        {
                            if (primary_out)
                                *primary_out = opts.output_file;
                            if (!do_write(opts.output_file, *artifact.llvm_ir_text))
                                ok = false;
                        }
                        llvm_written_via_extension = true;
                        break;
                    case dcc::backend::ArtifactKind::MirText:
                        if (artifact.mir_text)
                        {
                            if (primary_out)
                                *primary_out = opts.output_file;
                            if (!do_write(opts.output_file, *artifact.mir_text))
                                ok = false;
                        }
                        mir_written_via_extension = true;
                        break;
                    case dcc::backend::ArtifactKind::AsmText:
                        if (artifact.asm_text)
                        {
                            if (primary_out)
                                *primary_out = opts.output_file;
                            if (!do_write(opts.output_file, *artifact.asm_text))
                                ok = false;
                        }
                        asm_written_via_extension = true;
                        break;
                    case dcc::backend::ArtifactKind::ObjectBytes:
                        if (artifact.object_bytes)
                        {
                            if (primary_out)
                                *primary_out = opts.output_file;
                            if (!do_write(opts.output_file, *artifact.object_bytes))
                                ok = false;
                        }
                        obj_written_via_extension = true;
                        break;
                    case dcc::backend::ArtifactKind::ArchiveBytes:
                        if (artifact.archive_bytes)
                        {
                            if (primary_out)
                                *primary_out = opts.output_file;
                            if (!do_write(opts.output_file, *artifact.archive_bytes))
                                ok = false;
                        }
                        archive_written_via_extension = true;
                        break;
                    case dcc::backend::ArtifactKind::SharedLibraryBytes:
                        if (artifact.shared_library_bytes)
                        {
                            if (primary_out)
                                *primary_out = opts.output_file;
                            if (!do_write(opts.output_file, *artifact.shared_library_bytes))
                                ok = false;
                        }
                        break;
                    default:
                        break;
                }
            }
            else if (artifact.executable_bytes)
            {
                if (primary_out)
                    *primary_out = opts.output_file;
                if (!do_write(opts.output_file, *artifact.executable_bytes))
                    ok = false;
                else
                {
                    std::error_code ec;
                    std::filesystem::permissions(opts.output_file,
                                                 std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                                 std::filesystem::perm_options::add, ec);
                }
            }
        }
        else if (artifact.executable_bytes)
        {
            auto path = *primary;
            if (primary_out)
                *primary_out = path;
            if (!do_write(path, *artifact.executable_bytes))
                ok = false;
            else
            {
                std::error_code ec;
                std::filesystem::permissions(path,
                                             std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                             std::filesystem::perm_options::add, ec);
            }
        }
        else if (artifact.shared_library_bytes)
        {
            auto path = *primary;
            if (primary_out)
                *primary_out = path;
            if (!do_write(path, *artifact.shared_library_bytes))
                ok = false;
        }

        if (artifact.llvm_ir_text && !llvm_written_via_extension)
        {
            if (opts.output_file.empty())
                std::print("{}", *artifact.llvm_ir_text);
            else
            {
                auto path = base;
                path += ".ll";
                if (!do_write(path, *artifact.llvm_ir_text))
                    ok = false;
            }
        }

        if (artifact.mir_text && !mir_written_via_extension)
        {
            if (opts.output_file.empty())
                std::print("{}", *artifact.mir_text);
            else
            {
                auto path = base;
                path += ".mir";
                if (!do_write(path, *artifact.mir_text))
                    ok = false;
            }
        }

        if (artifact.asm_text && !asm_written_via_extension)
        {
            if (opts.output_file.empty())
                std::print("{}", *artifact.asm_text);
            else
            {
                auto path = base;
                path += ".s";
                if (!do_write(path, *artifact.asm_text))
                    ok = false;
            }
        }

        if (artifact.object_bytes && !obj_written_via_extension)
        {
            auto path = base;
            if (path.extension().empty())
                path += ".o";
            if (!do_write(path, *artifact.object_bytes))
                ok = false;
        }

        if (artifact.archive_bytes && !archive_written_via_extension)
        {
            auto path = base;
            if (path.extension().empty())
                path += ".a";
            if (!do_write(path, *artifact.archive_bytes))
                ok = false;
        }

        return ok;
    }

    [[nodiscard]] std::vector<std::filesystem::path> collect_depfile_dependencies(dcc::sema::ModuleInfo const* root, dcc::sema::ModuleGraph const& graph,
                                                                                  dcc::sm::SourceManager const& sm)
    {
        std::vector<std::filesystem::path> deps;
        std::unordered_set<dcc::sm::FileId> seen_ids;
        std::unordered_set<std::string> seen_paths;

        auto push_unique = [&](dcc::sema::ModuleInfo const* m) -> bool {
            auto const* file = sm.get(m->file_id);
            if (!file || file->kind() != dcc::sm::FileKind::Disk)
                return false;

            if (m->file_id != dcc::sm::FileId::Invalid && !seen_ids.insert(m->file_id).second)
                return false;

            if (!seen_paths.insert(m->file_path.string()).second)
                return false;

            deps.push_back(m->file_path);
            return true;
        };

        if (root)
            push_unique(root);

        for (auto const& m : graph.all())
            push_unique(m.get());

        if (deps.size() > 1)
            std::sort(deps.begin() + 1, deps.end(), [](std::filesystem::path const& a, std::filesystem::path const& b) { return a.string() < b.string(); });

        return deps;
    }

    [[nodiscard]] std::string escape_make_path(std::string_view path, bool is_target, bool at_line_end)
    {
        std::string out;
        out.reserve(path.size() + 16);

        std::size_t i = 0;
        while (i < path.size())
        {
            char c = path[i];
            if (c == '\\')
            {
                std::size_t run = 0;
                while (i + run < path.size() && path[i + run] == '\\')
                    ++run;

                char next = (i + run < path.size()) ? path[i + run] : '\0';
                if (next == ' ' || next == '\t' || next == '#')
                    out.append(2 * run, '\\');
                else if (next == ':')
                    out.append(2 * run, '\\');
                else if (is_target && next == '%')
                    out.append(2 * run, '\\');
                else if (next == '\0')
                    out.append((is_target || !at_line_end) ? 2 * run : run, '\\');
                else
                    out.append(run, '\\');

                i += run;
                continue;
            }

            switch (c)
            {
                case '$':
                    out += "$$";
                    break;
                case '#':
                    out += "\\#";
                    break;
                case ' ':
                    out += "\\ ";
                    break;
                case '\t':
                    out += "\\\t";
                    break;
                case ':':
                    out += "\\:";
                    break;
                case '%':
                    out += is_target ? "\\%" : "%";
                    break;
                default:
                    out += c;
                    break;
            }
            ++i;
        }

        return out;
    }

    [[nodiscard]] std::optional<std::string> serialize_depfile_rule(std::filesystem::path const& target, std::span<std::filesystem::path const> deps)
    {
        auto bad_path = [](std::filesystem::path const& p) {
            auto s = p.string();
            return s.find('\n') != std::string::npos || s.find('\r') != std::string::npos || s.find('\t') != std::string::npos;
        };

        if (bad_path(target))
            return std::nullopt;

        std::string rule = escape_make_path(target.string(), true, true);
        rule += ':';

        for (std::size_t i = 0; i < deps.size(); ++i)
        {
            if (bad_path(deps[i]))
                return std::nullopt;

            rule += ' ';
            rule += escape_make_path(deps[i].string(), false, i + 1 == deps.size());
        }

        if (!deps.empty() && !deps.back().string().empty() && deps.back().string().back() == '\\')
            rule += ' ';

        rule += '\n';
        return rule;
    }

    [[nodiscard]] bool same_file_path(std::filesystem::path const& a, std::filesystem::path const& b)
    {
        std::error_code ec_a;
        std::error_code ec_b;
        auto ca = std::filesystem::weakly_canonical(a, ec_a);
        auto cb = std::filesystem::weakly_canonical(b, ec_b);
        return !ec_a && !ec_b && ca == cb;
    }

    bool write_file_atomic(std::filesystem::path const& dest, std::string_view content)
    {
        auto parent = dest.parent_path();
        static std::atomic<int> s_temp_seq{0};

#ifndef _WIN32
        for (int attempt = 0; attempt < 128; ++attempt)
        {
            auto tmp_path = parent / (dest.filename().string() + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(++s_temp_seq));

            int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
            if (fd < 0)
            {
                if (errno == EEXIST)
                    continue;

                return false;
            }

            std::size_t off = 0;
            while (off < content.size())
            {
                auto n = ::write(fd, content.data() + off, content.size() - off);
                if (n <= 0)
                {
                    if (n < 0 && errno == EINTR)
                        continue;

                    ::close(fd);
                    std::error_code rm_ec;
                    std::filesystem::remove(tmp_path, rm_ec);
                    return false;
                }
                off += static_cast<std::size_t>(n);
            }

            if (::close(fd) != 0)
            {
                std::error_code rm_ec;
                std::filesystem::remove(tmp_path, rm_ec);
                return false;
            }

            std::error_code ec;
            std::filesystem::rename(tmp_path, dest, ec);
            if (ec)
            {
                std::error_code rm_ec;
                std::filesystem::remove(tmp_path, rm_ec);
                return false;
            }

            return true;
        }
        return false;
#else
        for (int attempt = 0; attempt < 128; ++attempt)
        {
            auto tmp_path = parent / (dest.filename().string() + ".tmp." + std::to_string(::GetCurrentProcessId()) + "." + std::to_string(++s_temp_seq));

            HANDLE h = ::CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE)
            {
                DWORD err = ::GetLastError();
                if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS)
                    continue;
                return false;
            }

            std::size_t total = 0;
            bool ok = true;
            while (total < content.size())
            {
                DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(content.size() - total, static_cast<std::size_t>(0xFFFFFFFFu)));
                DWORD written = 0;
                if (!::WriteFile(h, content.data() + total, chunk, &written, nullptr) || written == 0)
                {
                    ok = false;
                    break;
                }
                total += written;
            }

            if (!ok)
            {
                ::CloseHandle(h);
                std::error_code rm_ec;
                std::filesystem::remove(tmp_path, rm_ec);
                return false;
            }

            if (!::CloseHandle(h))
            {
                std::error_code rm_ec;
                std::filesystem::remove(tmp_path, rm_ec);
                return false;
            }

            if (!::MoveFileExW(tmp_path.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                std::error_code rm_ec;
                std::filesystem::remove(tmp_path, rm_ec);
                return false;
            }

            return true;
        }
        return false;
#endif
    }

    int merge_compdb(Options const& opts)
    {
        std::map<std::pair<std::string, std::string>, compdb::Entry> entries;
        for (auto const& path : opts.input_files)
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec))
            {
                std::println(std::cerr, "dcc: error: cannot read compilation database entry '{}'", path.string());
                return 1;
            }
            std::ifstream in(path, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (!in.is_open() || in.bad())
            {
                std::println(std::cerr, "dcc: error: cannot read compilation database entry '{}'", path.string());
                return 1;
            }
            auto entry = compdb::Parser(content).parse();
            if (!entry)
            {
                std::println(std::cerr, "dcc: error: invalid compilation database entry '{}': expected directory, file, arguments (string array), and output",
                             path.string());
                return 1;
            }
            entry->directory = compdb::absolute(entry->directory);
            entry->file = compdb::absolute(entry->file, entry->directory);
            entry->output = compdb::absolute(entry->output, entry->directory);
            auto key = std::pair{entry->file, entry->output};
            entries.insert_or_assign(std::move(key), std::move(*entry));
        }
        std::string content = "[";
        bool first = true;
        for (auto const& [key, entry] : entries)
        {
            if (!first)
                content += ',';
            first = false;
            content += "\n" + compdb::serialize(entry);
        }
        content += "\n]\n";
        if (!write_file_atomic(*opts.merge_compdb, content))
        {
            std::println(std::cerr, "dcc: error: cannot write compilation database '{}'", opts.merge_compdb->string());
            return 1;
        }
        return 0;
    }

    bool emit_compdb(Options const& opts, std::filesystem::path const& input, std::filesystem::path const& output, dcc::sema::ModuleInfo const* root,
                     dcc::sema::ModuleGraph const& graph, dcc::sm::SourceManager const& sm, int argc, char** argv)
    {
        if (!opts.compdb_entry)
            return true;
        if (output.empty())
        {
            std::println(std::cerr, "dcc: error: --compdb-entry requires a filesystem output artifact");
            return false;
        }
        auto deps = collect_depfile_dependencies(root, graph, sm);
        for (auto const& dep : deps)
        {
            if (same_file_path(*opts.compdb_entry, dep))
            {
                std::println(std::cerr, "dcc: error: --compdb-entry destination '{}' is the same as an input source file '{}'", opts.compdb_entry->string(),
                             dep.string());
                return false;
            }
        }
        compdb::Entry entry{compdb::absolute(std::filesystem::current_path()), compdb::absolute(input), {}, compdb::absolute(output)};
        for (int i = 0; i < argc; ++i)
            entry.arguments.emplace_back(argv[i]);
        auto content = compdb::serialize(entry) + "\n";
        if (!compdb::Parser(content).parse())
        {
            std::println(std::cerr, "dcc: error: cannot encode compilation database entry '{}': paths and arguments must be valid UTF-8",
                         opts.compdb_entry->string());
            return false;
        }
        if (!write_file_atomic(*opts.compdb_entry, content))
        {
            std::println(std::cerr, "dcc: error: cannot write compilation database entry '{}'", opts.compdb_entry->string());
            return false;
        }
        return true;
    }

    bool emit_depfile(std::filesystem::path const& depfile_path, std::filesystem::path const& target_path, dcc::sema::ModuleInfo const* root,
                      dcc::sema::ModuleGraph const& graph, dcc::sm::SourceManager const& sm)
    {
        if (target_path.empty())
        {
            std::println(std::cerr, "dcc: error: --depfile requires a filesystem output artifact");
            return false;
        }

        auto deps = collect_depfile_dependencies(root, graph, sm);

        if (same_file_path(depfile_path, target_path))
        {
            std::println(std::cerr, "dcc: error: --depfile destination '{}' is the same as the output artifact", depfile_path.string());
            return false;
        }

        for (auto const& dep : deps)
        {
            if (same_file_path(depfile_path, dep))
            {
                std::println(std::cerr, "dcc: error: --depfile destination '{}' is the same as an input source file '{}'", depfile_path.string(), dep.string());
                return false;
            }
        }

        auto rule = serialize_depfile_rule(target_path, deps);
        if (!rule)
        {
            std::println(std::cerr, "dcc: error: cannot write dependency file '{}': a path contains a newline, carriage-return, or tab character",
                         depfile_path.string());
            return false;
        }

        if (!write_file_atomic(depfile_path, *rule))
        {
            std::println(std::cerr, "dcc: error: cannot write dependency file '{}'", depfile_path.string());
            return false;
        }

        return true;
    }

    [[nodiscard]] std::set<dcc::backend::ArtifactKind> desired_artifacts(Options const& opts)
    {
        std::set<dcc::backend::ArtifactKind> kinds;

        if (opts.emit_asm_only)
        {
            kinds.insert(dcc::backend::ArtifactKind::AsmText);
            return kinds;
        }

        if (opts.compile_only)
        {
            kinds.insert(dcc::backend::ArtifactKind::ObjectBytes);
            return kinds;
        }

        if (opts.dump_llvm)
            kinds.insert(dcc::backend::ArtifactKind::LlvmIrText);
        if (opts.dump_mir)
            kinds.insert(dcc::backend::ArtifactKind::MirText);

        if (!kinds.empty())
            return kinds;

        if (opts.shared_library)
        {
            kinds.insert(dcc::backend::ArtifactKind::SharedLibraryBytes);
            return kinds;
        }

        if (!opts.output_file.empty())
        {
            auto ext = opts.output_file.extension().string();
            auto kind = artifact_kind_for_extension(ext);
            if (kind)
                kinds.insert(*kind);
            else
                kinds.insert(dcc::backend::ArtifactKind::ExecutableBytes);
            return kinds;
        }

        kinds.insert(dcc::backend::ArtifactKind::ExecutableBytes);
        return kinds;
    }

    [[nodiscard]] bool backend_needed(Options const& opts)
    {
        if (opts.dump_llvm || opts.dump_mir || opts.compile_only || opts.emit_asm_only)
            return true;

        if (!opts.output_file.empty())
        {
            auto ext = opts.output_file.extension().string();
            if (ext == ".ll" || ext == ".mir" || ext == ".s" || ext == ".o" || ext == ".a")
                return true;

            return true;
        }

        return !opts.dump_ir;
    }

    [[nodiscard]] bool writes_output_artifact(Options const& opts)
    {
        if (!backend_needed(opts))
            return false;

        if (opts.compile_only || opts.emit_asm_only)
            return true;

        if (opts.dump_llvm || opts.dump_mir)
            return !opts.output_file.empty();

        return true;
    }

    [[nodiscard]] bool is_object_file(std::filesystem::path const& p)
    {
        return p.extension() == ".o";
    }

    [[nodiscard]] std::string shell_quote(std::string const& s)
    {
#ifdef _WIN32
        bool needs_quotes = s.empty();
        for (char c : s)
            if (c == ' ' || c == '\t' || c == '"' || c == '&' || c == '|' || c == '<' || c == '>' || c == '^')
                needs_quotes = true;

        if (!needs_quotes)
            return s;

        std::string out{'"'};
        std::size_t i = 0;
        while (i < s.size())
        {
            std::size_t slashes = 0;
            while (i < s.size() && s[i] == '\\')
            {
                ++slashes;
                ++i;
            }

            if (i == s.size())
            {
                out.append(slashes * 2, '\\');
                break;
            }

            if (s[i] == '"')
            {
                out.append(slashes * 2 + 1, '\\');
                out += '"';
                ++i;
                continue;
            }

            out.append(slashes, '\\');
            out += s[i];
            ++i;
        }

        out += '"';
        return out;
#else
        std::string out{'\''};
        for (char c : s)
        {
            if (c == '\'')
                out += "'\\''";
            else
                out += c;
        }
        out += '\'';
        return out;
#endif
    }

    constexpr std::string_view float_enabled_marker = "__dcc_libdcext_abi_requires_float_enabled";
    constexpr std::string_view float_disabled_marker = "__dcc_libdcext_abi_requires_float_disabled";

    void add_libdcext_abi_guard(dcc::ir::IrContext& ctx, dcc::ir::IrModule& module, dcc::target::TargetConfig const& target)
    {
        auto name = target.float_available() ? float_enabled_marker : float_disabled_marker;
        auto* byte = ctx.int_t(8, false);
        if (target.object_format == dcc::target::ObjectFormat::Coff)
        {
            auto text = std::format(" /include:{} ", name);
            auto* type = ctx.array_t(byte, text.size());
            auto* init = ctx.aggregate(type);
            for (unsigned char c : std::span(reinterpret_cast<unsigned char const*>(text.data()), text.size()))
                init->values.push_back(ctx.int_const(byte, c));
            auto* directive = ctx.global("__dcc_libdcext_abi_directive", type, init, true);
            directive->section = ".drectve";
            directive->retain = true;
            module.globals.push_back(directive);
            return;
        }
        dcc::ir::IrGlobal* marker = nullptr;
        for (auto* global : module.globals)
            if (global->name == name)
                marker = global;
        if (!marker)
        {
            marker = ctx.global(name, byte);
            marker->is_declaration = true;
            marker->linkage = dcc::ir::Linkage::External;
            module.globals.push_back(marker);
        }
        auto* pointer = ctx.pointer_to(byte);
        auto* reference = ctx.global("__dcc_libdcext_abi_reference", pointer, ctx.global_ref(marker, pointer), true);
        reference->section = ".data.rel.ro.dcc.libdcext.abi";
        reference->retain = true;
        module.globals.push_back(reference);
    }

    std::pair<bool, bool> libdcext_markers(std::filesystem::path const& path)
    {
        std::ifstream input(path, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        return {bytes.find(float_enabled_marker) != std::string::npos, bytes.find(float_disabled_marker) != std::string::npos};
    }

    [[nodiscard]] std::string libdcext_library_name(dcc::target::TargetConfig const& target, std::string_view backend_name)
    {
        return std::format("dcext-{}-{}", dcc::target::os_name(target.os), backend_name);
    }

    [[nodiscard]] bool require_libdcext_archive(Options const& opts, dcc::target::TargetConfig const& target, std::filesystem::path const& prefix,
                                                std::optional<bool> source_float = std::nullopt)
    {
        auto filename = std::format("lib{}.a", libdcext_library_name(target, opts.backend_name));
        std::vector<std::filesystem::path> paths;
        for (auto const& path : opts.library_paths)
            paths.emplace_back(path);

        paths.push_back(prefix / "lib");
        std::string searched;
        for (auto const& path : paths)
        {
            std::error_code ec;
            if (std::filesystem::is_regular_file(path / filename, ec) && !ec)
            {
                auto [enabled, disabled] = libdcext_markers(path / filename);
                bool need_enabled = source_float == true;
                bool need_disabled = source_float == false;
                if (!source_float)
                    for (auto const& object : opts.input_files)
                    {
                        auto [object_enabled, object_disabled] = libdcext_markers(object);
                        need_enabled |= object_enabled;
                        need_disabled |= object_disabled;
                    }
                if ((need_enabled && disabled && !enabled) || (need_disabled && enabled && !disabled))
                {
                    auto missing = need_enabled && !enabled ? float_enabled_marker : float_disabled_marker;
                    std::println(std::cerr,
                                 "dcc: error: libdcext floating-point ABI mismatch: '{}' lacks {}; input mode is selected by -fsimd/-fno-simd on x86_64 or "
                                 "-fx87/-fno-x87 on x86; match those compilation flags to the archive selected by -flibdcext {}",
                                 (path / filename).string(), missing, dcc::target::os_name(target.os));
                    return false;
                }
                return true;
            }

            if (!searched.empty())
                searched += ", ";
            searched += (path / filename).string();
        }

        std::println(std::cerr, "dcc: error: missing libdcext archive '{}'; searched: {}", filename, searched);
        return false;
    }

    [[nodiscard]] std::vector<std::string> explicit_linker_args(Options const& opts)
    {
        std::vector<std::string> args;

        if (!opts.linker_script.empty())
            args.push_back(std::format("--script={}", opts.linker_script));

        if (!opts.entry_symbol.empty())
            args.push_back(std::format("--entry={}", opts.entry_symbol));

        if (opts.gc_sections)
            args.emplace_back("--gc-sections");

        for (auto const& extra : opts.linker_args)
            args.push_back(extra);

        return args;
    }

    [[nodiscard]] std::optional<dcc::backend::i8086::LinkOptions> i8086_link_options(Options const& opts, dcc::target::TargetConfig const& target)
    {
        if (target.arch != dcc::target::Arch::I8086)
        {
            if (opts.link_base || opts.stack_reserve)
            {
                std::println(std::cerr, "dcc: error: {} applies only to target 'i8086-binary' (target: '{}')", opts.link_base ? "-fbase" : "-fstack-reserve",
                             target.triple);
                return std::nullopt;
            }
            return dcc::backend::i8086::LinkOptions{};
        }
        dcc::backend::i8086::LinkOptions link;
        link.model = target.code_model;
        if (opts.link_base)
        {
            auto base = dcc::backend::i8086::parse_link_base(*opts.link_base);
            if (!base)
            {
                std::println(std::cerr, "dcc: error: {}", base.error());
                return std::nullopt;
            }
            link.base = *base;
        }
        if (opts.stack_reserve)
        {
            auto reserve = dcc::backend::i8086::parse_stack_reserve(*opts.stack_reserve);
            if (!reserve)
            {
                std::println(std::cerr, "dcc: error: {}", reserve.error());
                return std::nullopt;
            }
            link.stack_reserve = *reserve;
        }
        return link;
    }

    [[nodiscard]] std::vector<std::string> i8086_link_args(Options const& opts)
    {
        std::vector<std::string> args;
        for (auto const& dir : opts.library_paths)
            args.push_back(std::format("-L{}", dir));
        for (auto const& lib : opts.libraries)
            args.push_back(std::format("-l{}", lib));
        for (auto const& extra : explicit_linker_args(opts))
            args.push_back(extra);
        return args;
    }

    int run_i8086_link_mode(Options const& opts, dcc::target::TargetConfig const& target)
    {
        if (opts.libdcext)
        {
            std::println(std::cerr, "dcc: error: -flibdcext is not available for target 'i8086-binary'");
            return 1;
        }
        auto link = i8086_link_options(opts, target);
        if (!link)
            return 1;
        std::error_code ec;
        std::vector<std::string> objects;
        for (auto const& in : opts.input_files)
        {
            auto canon = std::filesystem::canonical(in, ec);
            if (ec)
            {
                std::println(std::cerr, "dcc: error: cannot find input file '{}'", in.string());
                return 1;
            }
            objects.push_back(canon.string());
        }
        std::filesystem::path output = opts.output_file.empty() ? std::filesystem::path{objects.front()}.replace_extension(".bin").filename() : opts.output_file;
        for (auto const& in : opts.input_files)
            if (same_file_path(output, in))
            {
                std::println(std::cerr, "dcc: error: output file '{}' is the same as an input object file", output.string());
                return 1;
            }
        auto linked = dcc::backend::i8086::link_flat_binary(objects, output.string(), *link, i8086_link_args(opts));
        if (!linked)
        {
            std::println(std::cerr, "dcc: error: linking failed:\n{}", linked.error());
            return 1;
        }
        return 0;
    }

    int run_link_mode(Options const& opts, char** argv)
    {
        if (opts.compile_only)
        {
            std::println(std::cerr, "dcc: error: -c cannot be used in link mode (all inputs are object files); pass a .dc source to -c, or link without -c");
            return 1;
        }

        if (opts.emit_asm_only)
        {
            std::println(std::cerr, "dcc: error: -S cannot be used in link mode (all inputs are already-compiled object files)");
            return 1;
        }

        if (opts.shared_library)
        {
            std::println(std::cerr, "dcc: error: -shared is not supported in link mode; link mode produces executables only");
            return 1;
        }

        dcc::target::TargetConfig target = resolve_target_or_exit(opts);
        bool const i8086 = target.arch == dcc::target::Arch::I8086;

        std::vector<std::string> compile_only;
        for (auto const& flag : opts.compile_only_flags)
            if (!i8086 || !flag.starts_with("-mcmodel"))
                compile_only.push_back(flag);
        if (!compile_only.empty())
        {
            std::string list;
            for (std::size_t k = 0; k < compile_only.size(); ++k)
            {
                if (k)
                    list += ", ";
                list += compile_only[k];
            }
            std::println(std::cerr, "dcc: error: option(s) {} only apply when compiling sources and cannot be used in link mode", list);
            return 1;
        }

        if (i8086)
            return run_i8086_link_mode(opts, target);

        if (!i8086_link_options(opts, target))
            return 1;

        if (target.object_format == dcc::target::ObjectFormat::Coff)
        {
            std::println(std::cerr,
                         "dcc: error: link mode does not support COFF targets (target: '{}'); executable linking is currently only supported for x86_64-elf",
                         target.triple);
            return 1;
        }

        if (target.triple != "x86_64-elf")
        {
            std::println(std::cerr, "dcc: error: executable linking is currently only supported for x86_64-elf (target: '{}')", target.triple);
            return 1;
        }

        std::error_code ec;
        std::vector<std::string> objects;
        objects.reserve(opts.input_files.size());
        for (auto const& in : opts.input_files)
        {
            auto canon = std::filesystem::canonical(in, ec);
            if (ec)
            {
                std::println(std::cerr, "dcc: error: cannot find input file '{}'", in.string());
                return 1;
            }
            objects.push_back(canon.string());
        }

        std::filesystem::path output_path;
        if (!opts.output_file.empty())
            output_path = opts.output_file;
        else
            output_path = std::filesystem::path{objects.front()}.replace_extension("").filename();

        for (auto const& in : opts.input_files)
        {
            if (same_file_path(output_path, in))
            {
                std::println(std::cerr, "dcc: error: output file '{}' is the same as an input object file", output_path.string());
                return 1;
            }
        }

        auto prefix = dcc::config::current_prefix(argv).path;
        if (opts.libdcext && !require_libdcext_archive(opts, target, prefix))
            return 1;

        std::string cmd = "ld.lld --static --no-dynamic-linker --fatal-warnings -o ";
        cmd += shell_quote(output_path.string());
        for (auto const& obj : objects)
        {
            cmd += " ";
            cmd += shell_quote(obj);
        }

        for (auto const& dir : opts.library_paths)
        {
            cmd += " ";
            cmd += shell_quote(std::format("-L{}", dir));
        }

        for (auto const& lib : opts.libraries)
        {
            cmd += " ";
            cmd += shell_quote(std::format("-l{}", lib));
        }

        for (auto const& extra : explicit_linker_args(opts))
        {
            cmd += " ";
            cmd += shell_quote(extra);
        }

        if (opts.libdcext)
        {
            cmd += " -L";
            cmd += shell_quote((prefix / "lib").string());
            cmd += " -l";
            cmd += libdcext_library_name(target, opts.backend_name);
        }

        cmd += " 2>&1";

        std::array<char, 4096> buf{};
        std::string captured;
        auto* pipe = popen(cmd.c_str(), "r");
        if (!pipe)
        {
            std::println(std::cerr, "dcc: error: failed to invoke linker");
            return 1;
        }
        while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
            captured += buf.data();

        int rc = pclose(pipe);
        if (rc != 0)
        {
            if (!captured.empty())
                std::println(std::cerr, "dcc: error: {}", std::string{"linking failed:"} + std::string(1, static_cast<char>(10)) + captured);
            else
                std::println(std::cerr, "dcc: error: linker failed with unknown error");
            return 1;
        }

        std::filesystem::permissions(output_path, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::add, ec);
        return 0;
    }

} // anonymous namespace

auto main(int argc, char** argv) -> int
{
    Options opts;
    try
    {
        opts = dcc::cli::parse_args(argc, argv);
    }
    catch (CliError const& error)
    {
        std::println(std::cerr, "dcc: error: {}", error.message);
        return 1;
    }

    if (opts.merge_compdb && !opts.help)
        return merge_compdb(opts);

    if (opts.print_linker_script && !opts.help)
    {
        auto target = resolve_target_or_exit(opts);
        if (target.arch != dcc::target::Arch::I8086)
        {
            std::println(std::cerr, "dcc: error: --print-linker-script applies only to target 'i8086-binary' (target: '{}')", target.triple);
            return 1;
        }
        auto link = i8086_link_options(opts, target);
        if (!link)
            return 1;
        std::print("{}", dcc::backend::i8086::linker_script(*link));
        return 0;
    }

    if (opts.help || opts.input_files.empty())
    {
        print_usage();
        return opts.help ? 0 : 1;
    }

    bool const any_object = std::ranges::any_of(opts.input_files, is_object_file);
    bool const all_objects = std::ranges::all_of(opts.input_files, is_object_file);

    if (any_object && !all_objects)
    {
        std::println(std::cerr, "dcc: error: cannot mix source files and object files in one invocation; compile sources with -c first, then link the objects");
        return 1;
    }

    if (all_objects)
    {
        if (opts.compdb_entry)
        {
            std::println(std::cerr, "dcc: error: --compdb-entry requires source compilation");
            return 1;
        }
        return run_link_mode(opts, argv);
    }

    if (opts.input_files.size() > 1)
    {
        std::println(std::cerr, "dcc: error: multiple input files are not supported in compile mode; compile each file with -c and link the objects");
        return 1;
    }

    if (opts.compile_only && opts.emit_asm_only)
    {
        std::println(std::cerr, "dcc: error: cannot specify both -c and -S");
        return 1;
    }

    if (opts.shared_library)
    {
        if (opts.compile_only || opts.emit_asm_only)
        {
            std::println(std::cerr, "dcc: error: -shared is mutually exclusive with -c and -S");
            return 1;
        }

        if (!opts.position_independent_code)
        {
            std::println(std::cerr, "dcc: warning: -shared implies -fPIC");
            opts.position_independent_code = true;
        }
    }

    if (opts.compdb_entry && !writes_output_artifact(opts))
    {
        std::println(std::cerr, "dcc: error: --compdb-entry requires a filesystem output artifact");
        return 1;
    }

    if (opts.depfile && !writes_output_artifact(opts))
    {
        std::println(std::cerr, "dcc: error: --depfile requires a filesystem output artifact");
        return 1;
    }

    std::error_code ec;
    auto input_path = std::filesystem::canonical(opts.input_files.front(), ec);
    if (ec)
    {
        std::println(std::cerr, "dcc: error: cannot find input file '{}'", opts.input_files.front().string());
        return 1;
    }

    if (opts.compdb_entry)
    {
        auto output = primary_output_path(opts, input_path, resolve_target_or_exit(opts));
        if (same_file_path(*opts.compdb_entry, input_path) || (output && same_file_path(*opts.compdb_entry, *output)) ||
            (opts.depfile && same_file_path(*opts.compdb_entry, *opts.depfile)))
        {
            std::println(std::cerr, "dcc: error: --compdb-entry destination '{}' conflicts with an input, output, or dependency file",
                         opts.compdb_entry->string());
            return 1;
        }
    }

    if (opts.depfile)
    {
        if (same_file_path(*opts.depfile, input_path))
        {
            std::println(std::cerr, "dcc: error: --depfile destination '{}' is the same as an input source file '{}'", opts.depfile->string(),
                         input_path.string());
            return 1;
        }

        auto dep_target = resolve_target_or_exit(opts);

        auto dep_target_path = primary_output_path(opts, input_path, dep_target);
        if (dep_target_path && same_file_path(*opts.depfile, *dep_target_path))
        {
            std::println(std::cerr, "dcc: error: --depfile destination '{}' is the same as the output artifact", opts.depfile->string());
            return 1;
        }
    }

    dcc::session::CompilerSession session;
    for (auto const& mapping : opts.file_prefix_maps)
    {
        auto separator = mapping.find('=');
        if (separator == std::string::npos)
        {
            std::println(std::cerr, "dcc: error: -ffile-prefix-map requires OLD=NEW");
            return 1;
        }
        session.source_manager().add_file_prefix_map(mapping.substr(0, separator), mapping.substr(separator + 1));
    }


    auto prefix = dcc::config::current_prefix(argv).path;

    dcc::session::CompileOptions compile_opts;
    compile_opts.arena_initial_size = 256 * 1024;
    compile_opts.injected_decls = std::move(opts.injected_decls);

    compile_opts.target = resolve_target_or_exit(opts);
    compile_opts.target.function_sections = opts.function_sections;
    compile_opts.target.data_sections = opts.data_sections;
    compile_opts.target.no_simd = !opts.simd;
    compile_opts.target.no_x87 = !opts.x87;

    compile_opts.import_roots.push_back(input_path.parent_path());

    for (auto& p : opts.import_paths)
    {
        auto canonical = std::filesystem::weakly_canonical(p, ec);
        if (!ec)
            compile_opts.import_roots.push_back(std::move(canonical));
    }

    if (opts.libdcext)
        compile_opts.import_roots.push_back(prefix / "include");

    compile_opts.inject_libdcext_prelude = opts.libdcext;
    compile_opts.restricted_check = opts.restricted_check;

    bool const measure = std::getenv("DCC_BENCH_STATS") != nullptr;
    auto phase_start = std::chrono::steady_clock::now();
    auto phase = [&](std::string_view name) {
        auto now = std::chrono::steady_clock::now();
        if (measure)
            std::println(std::cerr, "DCC_BENCH phase {} {}", name, std::chrono::duration<double>(now - phase_start).count());
        phase_start = now;
    };
    auto result = session.analyze_entry(input_path, compile_opts);
    phase("frontend");
    auto* module = result.module;

    if (result.has_errors)
        return 1;

    if (!module)
    {
        std::println(std::cerr, "dcc: error: internal error");
        return 1;
    }

    if (opts.dump_ast && module->tu)
        std::println("{}", dcc::ast::AstSerializer::dump(module->tu));

    bool need_backend = backend_needed(opts);
    if (opts.dump_ir || need_backend)
    {
        if (compile_opts.target.arch == dcc::target::Arch::I8086 && need_backend && opts.backend_name == "llvm")
        {
            std::println(std::cerr, "dcc: error: LLVM backend does not support target '{}'; use -fbackend custom", compile_opts.target.triple);
            return 1;
        }

        auto* sema = session.sema_context();
        if (!sema)
        {
            std::println(std::cerr, "dcc: error: internal error (no sema context)");
            return 1;
        }

        dcc::ir::IrContext ir_ctx{256 * 1024, &compile_opts.target};
        auto lowerer = std::make_unique<dcc::ir::lower::Lowerer>(ir_ctx, &sema->spec_registry(), &sema->graph(), opts.bounds_check, &session.source_manager(),
                                                                 &sema->types(), opts.restricted_check, opts.partial_eval, opts.far_check);
        lowerer->set_whole_program(opts.whole_program);
        auto* ir_mod = lowerer->lower_module(*module);
        if (!lowerer->lower_errors().empty())
        {
            for (auto const& message : lowerer->lower_errors())
                std::println(std::cerr, "dcc: error: {}", message);
            return 1;
        }

        if (opts.libdcext && need_backend)
            add_libdcext_abi_guard(ir_ctx, *ir_mod, compile_opts.target);

        phase("lowering");
        if (measure)
            dcc::ir::pass::benchmark_stats(*ir_mod, "before");

        if (opts.dump_ir)
            std::println("{}", dcc::ir::IrSerializer::dump(ir_mod));

        if (need_backend)
        {
            phase_start = std::chrono::steady_clock::now();
            dcc::target::TargetConfig target = resolve_target_or_exit(opts);

            target.function_sections = opts.function_sections;
            target.data_sections = opts.data_sections;
            target.no_red_zone = !opts.red_zone;
            target.no_simd = !opts.simd;
            target.no_x87 = !opts.x87;
            target.no_stack_protector = !opts.stack_protector;
            target.no_stack_probe = !opts.stack_probe;
            target.position_independent_code = opts.position_independent_code;
            if (opts.code_model)
                target.code_model = *opts.code_model;

            if (!opts.target_cpu.empty())
            {
                if (target.arch == dcc::target::Arch::X86_64 || target.arch == dcc::target::Arch::X86)
                {
                    if (!dcc::target::TargetConfig::is_x86_cpu_allowed(opts.target_cpu))
                    {
                        std::println(std::cerr, "dcc: error: unknown CPU '{}' for target arch", opts.target_cpu);
                        return 1;
                    }
                }
                target.cpu = opts.target_cpu;
            }

            if (opts.shared_library)
            {
                if (target.object_format != dcc::target::ObjectFormat::Elf && target.object_format != dcc::target::ObjectFormat::Coff)
                {
                    std::println(std::cerr, "dcc: error: shared library output is only supported for ELF targets");
                    return 1;
                }
            }

            if (!i8086_link_options(opts, target))
                return 1;

            auto kinds = desired_artifacts(opts);
            if (kinds.empty())
            {
                std::println(std::cerr, "dcc: error: no output artifact requested; use -fdump-llvm, -fdump-mir, -S, -c, or -o");
                return 1;
            }

            dcc::backend::BackendOptions backend_opts;
            backend_opts.target = target;
            backend_opts.requested_artifacts = kinds;
            backend_opts.emit_debug_info = opts.emit_debug_info;
            backend_opts.debug_format = opts.debug_format;
            backend_opts.omit_frame_pointer = opts.omit_frame_pointer;
            backend_opts.opt_level = opts.opt_level;
            backend_opts.source_manager = &session.source_manager();

            for (auto const& dir : opts.library_paths)
                backend_opts.library_paths.push_back(dir);

            for (auto const& lib : opts.libraries)
                backend_opts.libraries.push_back(lib);

            for (auto const& extra : explicit_linker_args(opts))
                backend_opts.linker_args.push_back(extra);

            bool const links_output =
                kinds.contains(dcc::backend::ArtifactKind::ExecutableBytes) || kinds.contains(dcc::backend::ArtifactKind::SharedLibraryBytes);
            if (opts.whole_program && opts.libdcext && links_output)
            {
                std::println(std::cerr, "dcc: error: -fwhole-program with -flibdcext cannot link an executable or shared library: the libdcext archive's "
                                        "compiled std modules would duplicate the whole-program definitions; compile with -c or -S and link only "
                                        "libdcext's assembly objects (memory.asm.o, crt0.asm.o, syscall.asm.o) yourself");
                return 1;
            }

            if (opts.libdcext && links_output)
            {
                if (!require_libdcext_archive(opts, target, prefix, target.float_available()))
                    return 1;
                backend_opts.library_paths.push_back((prefix / "lib").string());
                backend_opts.libraries.push_back(libdcext_library_name(target, opts.backend_name));
            }

            if (opts.backend_name == "llvm")
            {
                if (kinds.contains(dcc::backend::ArtifactKind::MirText))
                {
                    std::println(std::cerr, "dcc: error: LLVM backend does not support MIR output");
                    return 1;
                }

                if (kinds.contains(dcc::backend::ArtifactKind::SharedLibraryBytes))
                {
                    std::println(std::cerr, "dcc: error: LLVM backend does not support shared library output; use -fbackend custom");
                    return 1;
                }

#if DCC_ENABLE_LLVM
                bool need_archive_wrap = kinds.contains(dcc::backend::ArtifactKind::ArchiveBytes);
                if (need_archive_wrap)
                {
                    if (target.object_format == dcc::target::ObjectFormat::Coff)
                    {
                        std::println(std::cerr, "dcc: error: static archive output is not supported for COFF target");
                        return 1;
                    }
                    backend_opts.requested_artifacts.erase(dcc::backend::ArtifactKind::ArchiveBytes);
                    backend_opts.requested_artifacts.insert(dcc::backend::ArtifactKind::ObjectBytes);
                }

                auto backend = dcc::backend::make_llvm_backend();
                auto artifact = backend->emit(*ir_mod, backend_opts);
                phase("backend");
                std::ignore = dcc::backend::validate_requested_artifacts(backend_opts.requested_artifacts, artifact);

                if (!artifact.diagnostics.empty())
                {
                    for (auto const& d : artifact.diagnostics)
                        std::println(std::cerr, "dcc: error: backend: {}", d.message);

                    return 1;
                }

                if (need_archive_wrap)
                {
                    if (artifact.object_bytes)
                    {
                        std::string member_name{ir_mod->name};
                        if (member_name.empty())
                            member_name = "module";
                        member_name += ".o";

                        std::vector<std::uint8_t> obj_data;
                        obj_data.reserve(artifact.object_bytes->size());
                        for (auto b : *artifact.object_bytes)
                            obj_data.push_back(static_cast<std::uint8_t>(b));

                        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> archive_members;
                        archive_members.emplace_back(std::move(member_name), std::move(obj_data));
                        auto archive_data = dcc::backend::object::write_archive_elf(archive_members);

                        std::vector<std::byte> archive_bytes;
                        archive_bytes.reserve(archive_data.size());
                        for (auto b : archive_data)
                            archive_bytes.push_back(static_cast<std::byte>(b));
                        artifact.archive_bytes = std::move(archive_bytes);
                    }

                    artifact.object_bytes.reset();
                }

                std::filesystem::path primary_output;
                if (!write_artifacts(artifact, opts, input_path, target, &primary_output))
                    return 1;

                if (!emit_compdb(opts, input_path, primary_output, module, sema->graph(), session.source_manager(), argc, argv))
                    return 1;

                if (opts.depfile && !emit_depfile(*opts.depfile, primary_output, module, sema->graph(), session.source_manager()))
                    return 1;
#else
                std::println(std::cerr, "dcc: error: LLVM support not compiled into this build of dcc");
                return 1;
#endif
            }
            else if (opts.backend_name == "custom")
            {
                if (opts.emit_debug_info)
                    std::println(std::cerr, "dcc: warning: debug information is not supported by the custom backend");

                if (kinds.contains(dcc::backend::ArtifactKind::LlvmIrText))
                {
                    std::println(std::cerr, "dcc: error: custom backend does not support LLVM IR output");
                    return 1;
                }

                auto link = i8086_link_options(opts, target);
                if (!link)
                    return 1;
                bool const link_i8086 =
                    target.arch == dcc::target::Arch::I8086 && backend_opts.requested_artifacts.contains(dcc::backend::ArtifactKind::ExecutableBytes);
                if (link_i8086)
                {
                    backend_opts.requested_artifacts.erase(dcc::backend::ArtifactKind::ExecutableBytes);
                    backend_opts.requested_artifacts.insert(dcc::backend::ArtifactKind::ObjectBytes);
                }

                auto backend = target.arch == dcc::target::Arch::I8086 ? dcc::backend::make_i8086_backend() : dcc::backend::make_em64t_backend();
                auto artifact = backend->emit(*ir_mod, backend_opts);
                phase("backend");
                std::ignore = dcc::backend::validate_requested_artifacts(backend_opts.requested_artifacts, artifact);

                if (link_i8086 && artifact.diagnostics.empty() && artifact.object_bytes)
                {
                    auto dir = std::filesystem::temp_directory_path(ec) /
                               std::format("dcc-i8086-{}", std::chrono::steady_clock::now().time_since_epoch().count());
                    std::filesystem::create_directories(dir, ec);
                    auto object_path = dir / "module.o";
                    auto binary_path = dir / "module.bin";
                    {
                        std::ofstream out{object_path, std::ios::binary};
                        out.write(reinterpret_cast<char const*>(artifact.object_bytes->data()), static_cast<std::streamsize>(artifact.object_bytes->size()));
                    }
                    auto linked = dcc::backend::i8086::link_flat_binary({object_path.string()}, binary_path.string(), *link, i8086_link_args(opts));
                    if (!linked)
                        artifact.diagnostics.push_back(dcc::backend::BackendDiagnostic{{}, std::format("linking failed:\n{}", linked.error())});
                    else
                    {
                        std::ifstream in{binary_path, std::ios::binary};
                        std::vector<char> bytes{std::istreambuf_iterator<char>(in), {}};
                        std::vector<std::byte> binary(bytes.size());
                        std::ranges::transform(bytes, binary.begin(), [](char c) { return static_cast<std::byte>(c); });
                        artifact.executable_bytes = std::move(binary);
                        artifact.object_bytes.reset();
                    }
                    std::filesystem::remove_all(dir, ec);
                }

                if (!artifact.diagnostics.empty())
                {
                    for (auto const& d : artifact.diagnostics)
                        std::println(std::cerr, "dcc: error: backend: {}", d.message);

                    return 1;
                }

                std::filesystem::path primary_output;
                if (!write_artifacts(artifact, opts, input_path, target, &primary_output))
                    return 1;

                if (!emit_compdb(opts, input_path, primary_output, module, sema->graph(), session.source_manager(), argc, argv))
                    return 1;

                if (opts.depfile && !emit_depfile(*opts.depfile, primary_output, module, sema->graph(), session.source_manager()))
                    return 1;
            }
        }
    }

    return 0;
}
