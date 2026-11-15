module;

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" char** environ;
#endif

export module dcc.host.process;

import std;

export namespace dcc::host::process
{
    struct Command
    {
        std::vector<std::string> argv;
        std::map<std::string, std::string> env;
        std::filesystem::path cwd;
        std::filesystem::path stdin_file;
        std::filesystem::path stdout_file;
        std::size_t max_capture = 16u << 20;
        std::chrono::milliseconds timeout{300000};
        std::uint64_t max_file_size = 0;
        std::function<bool(std::string_view)> on_stdout;
        bool passthrough{};
        bool inherit_env{};
        bool own_group{};
        std::chrono::milliseconds grace{2000};
        std::atomic<bool> const* cancel{};
        std::size_t max_error_capture = 64u << 10;
    };

    struct Result
    {
        bool started{};
        int exit_code{-1};
        int signal{};
        bool timed_out{};
        bool aborted{};
        bool truncated{};
        bool err_truncated{};
        std::string out;
        std::string err;
        std::string error;

        [[nodiscard]] bool ok() const { return started && exit_code == 0 && error.empty() && !timed_out && !aborted && !truncated; }
    };

    [[nodiscard]] inline std::string windows_quote(std::string_view value)
    {
        std::string out = "\"";
        std::size_t slashes = 0;
        for (char c : value)
        {
            if (c == '\\')
                ++slashes;
            else
            {
                out.append(slashes * (c == '"' ? 2 : 1), '\\');
                slashes = 0;
                if (c == '"')
                    out += '\\';
                out += c;
            }
        }
        out.append(slashes * 2, '\\');
        return out + '"';
    }

    namespace detail
    {
        [[nodiscard]] inline bool cancelled(Command const& cmd)
        {
            return cmd.cancel && cmd.cancel->load(std::memory_order_relaxed);
        }

        inline void append_error(Result& result, Command const& cmd, std::string_view chunk)
        {
            auto room = cmd.max_error_capture > result.err.size() ? cmd.max_error_capture - result.err.size() : 0;
            if (chunk.size() > room)
                result.err_truncated = true;
            result.err.append(chunk.substr(0, room));
        }
    }

#if !defined(_WIN32)
    namespace detail
    {
        inline volatile std::sig_atomic_t s_forward_signal = 0;

        extern "C" inline void record_signal(int sig)
        {
            s_forward_signal = sig;
        }

        inline std::optional<std::string> find_program(std::string const& name, std::string const& path_value)
        {
            if (name.find('/') != std::string::npos)
                return name;
            std::size_t pos = 0;
            while (pos <= path_value.size())
            {
                auto colon = path_value.find(':', pos);
                auto dir = path_value.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos);
                if (dir.empty())
                    dir = ".";
                auto candidate = dir + "/" + name;
                if (::access(candidate.c_str(), X_OK) == 0)
                {
                    struct stat st{};
                    if (::stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode))
                        return candidate;
                }
                if (colon == std::string::npos)
                    break;
                pos = colon + 1;
            }
            return std::nullopt;
        }
    }

    [[nodiscard]] inline Result run(Command const& cmd)
    {
        Result result;
        if (cmd.argv.empty())
        {
            result.error = "empty command";
            return result;
        }
        std::string path_value = "/usr/local/bin:/usr/bin:/bin";
        if (auto it = cmd.env.find("PATH"); it != cmd.env.end())
            path_value = it->second;
        else if (cmd.inherit_env)
            if (char const* parent = std::getenv("PATH"))
                path_value = parent;
        auto program = detail::find_program(cmd.argv[0], path_value);
        if (!program)
        {
            result.error = "cannot find " + cmd.argv[0];
            return result;
        }
        std::vector<std::string> env_strings;
        if (cmd.inherit_env)
            for (char** e = environ; e && *e; ++e)
            {
                std::string_view entry{*e};
                auto eq = entry.find('=');
                if (eq != std::string_view::npos && !cmd.env.contains(std::string{entry.substr(0, eq)}))
                    env_strings.emplace_back(entry);
            }
        for (auto const& [key, value] : cmd.env)
            env_strings.push_back(key + "=" + value);
        std::vector<char*> envp;
        for (auto& s : env_strings)
            envp.push_back(s.data());
        envp.push_back(nullptr);
        std::vector<std::string> argv_strings = cmd.argv;
        std::vector<char*> argv;
        for (auto& s : argv_strings)
            argv.push_back(s.data());
        argv.push_back(nullptr);

        int out_pipe[2] = {-1, -1};
        int err_pipe[2] = {-1, -1};
        int exec_pipe[2] = {-1, -1};
        bool const capture_out = cmd.stdout_file.empty() && !cmd.passthrough;
        bool const piped_err = !cmd.passthrough;
        if ((capture_out && ::pipe(out_pipe) != 0) || (piped_err && ::pipe(err_pipe) != 0) || ::pipe(exec_pipe) != 0)
        {
            result.error = "cannot create pipes";
            return result;
        }
        ::fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC);
        auto pid = ::fork();
        if (pid < 0)
        {
            result.error = "cannot fork";
            return result;
        }
        bool const foreground = cmd.passthrough && cmd.own_group && ::isatty(0) && ::tcgetpgrp(0) == ::getpgrp();
        if (pid == 0)
        {
            if (!cmd.passthrough)
                ::setsid();
            else if (cmd.own_group)
            {
                ::setpgid(0, 0);
                if (foreground)
                {
                    ::signal(SIGTTOU, SIG_IGN);
                    ::tcsetpgrp(0, ::getpgrp());
                }
                ::signal(SIGTTOU, SIG_DFL);
                ::signal(SIGINT, SIG_DFL);
                ::signal(SIGTERM, SIG_DFL);
                ::signal(SIGHUP, SIG_DFL);
            }
            if (!cmd.passthrough || !cmd.stdin_file.empty())
            {
                int in_fd = ::open(cmd.stdin_file.empty() ? "/dev/null" : cmd.stdin_file.c_str(), O_RDONLY);
                if (in_fd < 0)
                    ::_exit(126);
                ::dup2(in_fd, 0);
            }
            if (capture_out)
                ::dup2(out_pipe[1], 1);
            else if (!cmd.passthrough)
            {
                int fd = ::open(cmd.stdout_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (fd < 0)
                    ::_exit(126);
                ::dup2(fd, 1);
            }
            if (piped_err)
                ::dup2(err_pipe[1], 2);
            for (int fd = 3; fd < 256; ++fd)
                if (fd != exec_pipe[1])
                    ::close(fd);
            if (!cmd.cwd.empty() && ::chdir(cmd.cwd.c_str()) != 0)
                ::_exit(126);
            if (cmd.max_file_size > 0)
            {
                ::signal(SIGXFSZ, SIG_DFL);
                struct rlimit lim{};
                lim.rlim_cur = lim.rlim_max = static_cast<rlim_t>(cmd.max_file_size);
                ::setrlimit(RLIMIT_FSIZE, &lim);
            }
            ::execve(program->c_str(), argv.data(), envp.data());
            int err = errno;
            [[maybe_unused]] auto n = ::write(exec_pipe[1], &err, sizeof err);
            ::_exit(127);
        }
        ::close(exec_pipe[1]);
        if (capture_out)
            ::close(out_pipe[1]);
        if (piped_err)
            ::close(err_pipe[1]);
        int exec_errno = 0;
        if (::read(exec_pipe[0], &exec_errno, sizeof exec_errno) == static_cast<ssize_t>(sizeof exec_errno))
        {
            ::waitpid(pid, nullptr, 0);
            ::close(exec_pipe[0]);
            if (capture_out)
                ::close(out_pipe[0]);
            if (piped_err)
                ::close(err_pipe[0]);
            result.error = std::format("cannot run {}: {}", cmd.argv[0], ::strerror(exec_errno));
            return result;
        }
        ::close(exec_pipe[0]);
        result.started = true;

        auto deadline = std::chrono::steady_clock::now() + cmd.timeout;
        bool killed = false;
        auto kill_child = [&]
        {
            if (!killed)
            {
                ::kill(-pid, SIGKILL);
                ::kill(pid, SIGKILL);
                killed = true;
            }
        };
        if (cmd.passthrough && cmd.own_group)
        {
            ::setpgid(pid, pid);
            if (foreground)
            {
                auto previous = ::signal(SIGTTOU, SIG_IGN);
                ::tcsetpgrp(0, pid);
                ::signal(SIGTTOU, previous);
            }
            detail::s_forward_signal = 0;
            struct sigaction action{};
            action.sa_handler = detail::record_signal;
            ::sigemptyset(&action.sa_mask);
            struct sigaction old_int{};
            struct sigaction old_term{};
            struct sigaction old_hup{};
            ::sigaction(SIGINT, &action, &old_int);
            ::sigaction(SIGTERM, &action, &old_term);
            ::sigaction(SIGHUP, &action, &old_hup);
            int pass_status = 0;
            std::optional<std::chrono::steady_clock::time_point> kill_at;
            for (;;)
            {
                auto done = ::waitpid(pid, &pass_status, WNOHANG);
                if (done == pid)
                    break;
                if (done < 0 && errno != EINTR)
                    break;
                if (int sig = detail::s_forward_signal; sig != 0)
                {
                    detail::s_forward_signal = 0;
                    ::kill(-pid, sig);
                }
                auto now = std::chrono::steady_clock::now();
                if (!kill_at && detail::cancelled(cmd))
                {
                    result.aborted = true;
                    ::kill(-pid, SIGTERM);
                    kill_at = now + cmd.grace;
                }
                if (!kill_at && now >= deadline)
                {
                    result.timed_out = true;
                    ::kill(-pid, SIGTERM);
                    kill_at = now + cmd.grace;
                }
                if (kill_at && now >= *kill_at)
                {
                    kill_child();
                    ::waitpid(pid, &pass_status, 0);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (result.timed_out && !killed)
                ::kill(-pid, SIGKILL);
            if (foreground)
            {
                auto previous = ::signal(SIGTTOU, SIG_IGN);
                ::tcsetpgrp(0, ::getpgrp());
                ::signal(SIGTTOU, previous);
            }
            ::sigaction(SIGINT, &old_int, nullptr);
            ::sigaction(SIGTERM, &old_term, nullptr);
            ::sigaction(SIGHUP, &old_hup, nullptr);
            if (WIFEXITED(pass_status))
                result.exit_code = WEXITSTATUS(pass_status);
            else if (WIFSIGNALED(pass_status))
                result.signal = WTERMSIG(pass_status);
            return result;
        }
        if (cmd.passthrough)
        {
            int pass_status = 0;
            for (;;)
            {
                auto done = ::waitpid(pid, &pass_status, WNOHANG);
                if (done == pid)
                    break;
                if (done < 0 && errno != EINTR)
                    break;
                if (detail::cancelled(cmd))
                {
                    result.aborted = true;
                    kill_child();
                    ::waitpid(pid, &pass_status, 0);
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    result.timed_out = true;
                    kill_child();
                    ::waitpid(pid, &pass_status, 0);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (WIFEXITED(pass_status))
                result.exit_code = WEXITSTATUS(pass_status);
            else if (WIFSIGNALED(pass_status))
                result.signal = WTERMSIG(pass_status);
            return result;
        }
        std::array<pollfd, 2> fds{};
        std::size_t nfds = 0;
        int out_index = -1;
        int err_index = 0;
        if (capture_out)
        {
            fds[nfds++] = {out_pipe[0], POLLIN, 0};
            out_index = 0;
            err_index = 1;
        }
        fds[nfds++] = {err_pipe[0], POLLIN, 0};
        std::array<char, 65536> buffer{};
        std::size_t open_fds = nfds;
        while (open_fds > 0)
        {
            auto now = std::chrono::steady_clock::now();
            if (detail::cancelled(cmd))
            {
                result.aborted = true;
                kill_child();
                break;
            }
            if (now >= deadline)
            {
                result.timed_out = true;
                kill_child();
                break;
            }
            auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            int ready = ::poll(fds.data(), nfds, static_cast<int>(std::min<long long>(wait_ms, cmd.cancel ? 50 : 1000)));
            if (ready < 0 && errno != EINTR)
                break;
            for (std::size_t i = 0; i < nfds; ++i)
            {
                if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                    continue;
                auto n = ::read(fds[i].fd, buffer.data(), buffer.size());
                if (n <= 0)
                {
                    if (n < 0 && errno == EINTR)
                        continue;
                    ::close(fds[i].fd);
                    fds[i].fd = -1;
                    --open_fds;
                    continue;
                }
                std::string_view chunk{buffer.data(), static_cast<std::size_t>(n)};
                if (static_cast<int>(i) == out_index)
                {
                    if (cmd.on_stdout)
                    {
                        if (!cmd.on_stdout(chunk))
                        {
                            result.aborted = true;
                            kill_child();
                        }
                    }
                    else if (result.out.size() + chunk.size() > cmd.max_capture)
                    {
                        result.truncated = true;
                        kill_child();
                    }
                    else
                        result.out.append(chunk);
                }
                else if (static_cast<int>(i) == err_index)
                    detail::append_error(result, cmd, chunk);
            }
            if (killed)
                break;
        }
        for (auto& f : fds)
            if (f.fd >= 0)
                ::close(f.fd);
        int status = 0;
        ::waitpid(pid, &status, 0);
        if (WIFEXITED(status))
            result.exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            result.signal = WTERMSIG(status);
        return result;
    }
#else
    namespace detail
    {
        struct Handle
        {
            HANDLE value{INVALID_HANDLE_VALUE};
            Handle() = default;
            explicit Handle(HANDLE h) : value{h} {}
            Handle(Handle const&) = delete;
            Handle& operator=(Handle const&) = delete;
            ~Handle() { reset(); }
            void reset(HANDLE h = INVALID_HANDLE_VALUE)
            {
                if (value && value != INVALID_HANDLE_VALUE)
                    ::CloseHandle(value);
                value = h;
            }
            explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
        };

        inline std::wstring wide(std::string_view text)
        {
            if (text.empty())
                return {};
            auto n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (!n)
                throw std::runtime_error{"invalid UTF-8 in process configuration"};
            std::wstring out(static_cast<std::size_t>(n), L'\0');
            ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), n);
            return out;
        }

        inline std::string utf8(std::wstring_view text)
        {
            if (text.empty())
                return {};
            auto n = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            if (!n)
                throw std::runtime_error{"invalid UTF-16 in process configuration"};
            std::string out(static_cast<std::size_t>(n), '\0');
            ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
            return out;
        }

        inline std::string failure(std::string_view operation)
        {
            auto code = ::GetLastError();
            return std::format("{}: {} (Windows error {})", operation, std::system_category().message(static_cast<int>(code)), code);
        }

        struct CaseLess
        {
            bool operator()(std::wstring const& a, std::wstring const& b) const
            {
                return ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
            }
        };

        inline std::wstring variable(wchar_t const* name)
        {
            auto n = ::GetEnvironmentVariableW(name, nullptr, 0);
            if (!n)
                return {};
            std::wstring value(n, L'\0');
            auto got = ::GetEnvironmentVariableW(name, value.data(), n);
            if (!got || got >= n)
                return {};
            value.resize(got);
            return value;
        }

        inline std::optional<std::string> find_program(std::string const& name, std::string const& path_value,
                                                      std::filesystem::path const& cwd = {})
        {
            namespace fs = std::filesystem;
            auto check = [&](fs::path p) -> std::optional<std::string>
            {
                if (p.is_relative())
                    p = (cwd.empty() ? fs::current_path() : fs::absolute(cwd)) / p;
                auto extension = p.extension().wstring();
                std::ranges::transform(extension, extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                std::vector<fs::path> candidates;
                if (p.has_extension())
                    candidates.push_back(p);
                if (extension != L".exe" && extension != L".com")
                    candidates.push_back(fs::path{p} += L".exe");
                for (auto const& candidate : candidates)
                {
                    std::error_code ec;
                    if (fs::is_regular_file(candidate, ec))
                        return utf8(candidate.lexically_normal().native());
                }
                return std::nullopt;
            };
            if (name.find_first_of("/\\:") != std::string::npos)
                return check(fs::path{wide(name)});
            std::size_t pos = 0;
            while (pos <= path_value.size())
            {
                auto end = path_value.find(';', pos);
                auto dir = path_value.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"')
                    dir = dir.substr(1, dir.size() - 2);
                if (auto found = check(fs::path{wide(dir)} / wide(name)))
                    return found;
                if (end == std::string::npos)
                    break;
                pos = end + 1;
            }
            return std::nullopt;
        }

        inline std::atomic<DWORD> control_event{};
        inline BOOL WINAPI record_control(DWORD event)
        {
            if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
                return FALSE;
            control_event.store(event + 1);
            return TRUE;
        }

        struct Attributes
        {
            LPPROC_THREAD_ATTRIBUTE_LIST list{};
            bool initialized{};
            ~Attributes()
            {
                if (initialized)
                    ::DeleteProcThreadAttributeList(list);
                if (list)
                    ::HeapFree(::GetProcessHeap(), 0, list);
            }
        };
    }

    [[nodiscard]] inline Result run(Command const& cmd)
    {
        Result result;
        try
        {
            if (cmd.argv.empty() || cmd.argv.front().empty())
                throw std::runtime_error{"empty command"};
            std::map<std::wstring, std::wstring, detail::CaseLess> env;
            if (cmd.inherit_env)
            {
                auto block = ::GetEnvironmentStringsW();
                if (!block)
                    throw std::runtime_error{detail::failure("read environment")};
                for (auto p = block; *p; p += std::wcslen(p) + 1)
                {
                    std::wstring entry{p};
                    auto eq = entry.find(L'=', 1);
                    if (eq != std::wstring::npos)
                        env[entry.substr(0, eq)] = entry.substr(eq + 1);
                }
                ::FreeEnvironmentStringsW(block);
            }
            else
                for (auto name : {L"SystemRoot", L"TEMP", L"TMP"})
                    if (auto value = detail::variable(name); !value.empty())
                        env[name] = std::move(value);
            for (auto const& [key, value] : cmd.env)
            {
                if (key.empty() || key.find_first_of("=\0", 0, 2) != std::string::npos || value.find('\0') != std::string::npos)
                    throw std::runtime_error{"invalid process environment entry"};
                env[detail::wide(key)] = detail::wide(value);
            }
            auto path = env.find(L"PATH");
            auto path_value = path == env.end() ? detail::utf8(detail::variable(L"SystemRoot")) + "/System32" : detail::utf8(path->second);
            auto program = detail::find_program(cmd.argv[0], path_value, cmd.cwd);
            if (!program)
                throw std::runtime_error{"cannot find " + cmd.argv[0]};
            auto extension = std::filesystem::path{detail::wide(*program)}.extension().wstring();
            std::ranges::transform(extension, extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (extension == L".bat" || extension == L".cmd" || extension == L".ps1" || extension == L".sh")
                throw std::runtime_error{"script requires an explicit interpreter: " + *program};
            std::string arguments;
            for (auto const& arg : cmd.argv)
            {
                if (arg.find('\0') != std::string::npos)
                    throw std::runtime_error{"NUL in process argument"};
                if (!arguments.empty())
                    arguments += ' ';
                arguments += windows_quote(arg);
            }
            auto filename = std::filesystem::path{detail::wide(*program)}.filename().wstring();
            std::ranges::transform(filename, filename.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (filename == L"cmd.exe")
            {
                if (cmd.argv.size() != 5 || cmd.argv[1] != "/d" || cmd.argv[2] != "/s" || cmd.argv[3] != "/c")
                    throw std::runtime_error{"cmd.exe requires /d /s /c followed by one explicit command string"};
                arguments = windows_quote(*program) + " /d /s /c \"" + cmd.argv[4] + "\"";
            }
            auto line = detail::wide(arguments);
            if (line.size() >= 32767)
                throw std::runtime_error{"Windows command line exceeds 32766 UTF-16 code units"};
            std::vector<wchar_t> environment;
            for (auto const& [key, value] : env)
            {
                environment.insert(environment.end(), key.begin(), key.end());
                environment.push_back(L'=');
                environment.insert(environment.end(), value.begin(), value.end());
                environment.push_back(L'\0');
            }
            environment.push_back(L'\0');
            if (environment.size() == 1)
                environment.push_back(L'\0');

            detail::Handle job{::CreateJobObjectW(nullptr, nullptr)};
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!job || !::SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof limits))
                throw std::runtime_error{detail::failure("create child job")};
            SECURITY_ATTRIBUTES security{sizeof security, nullptr, TRUE};
            detail::Handle input, output, error, out_read, err_read;
            auto file = [&](detail::Handle& handle, std::filesystem::path const& name, bool writing)
            {
                handle.reset(::CreateFileW(name.c_str(), writing ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           &security, writing ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
                if (!handle)
                    throw std::runtime_error{detail::failure("open process stream")};
            };
            auto inherit = [&](detail::Handle& handle, DWORD id, bool writing)
            {
                auto source = ::GetStdHandle(id);
                HANDLE copy{};
                if (source && source != INVALID_HANDLE_VALUE && ::DuplicateHandle(::GetCurrentProcess(), source, ::GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS))
                    handle.reset(copy);
                else
                    file(handle, L"NUL", writing);
            };
            auto pipe = [&](detail::Handle& read, detail::Handle& write)
            {
                HANDLE r{}, w{};
                if (!::CreatePipe(&r, &w, &security, 0))
                    throw std::runtime_error{detail::failure("create process pipe")};
                read.reset(r);
                write.reset(w);
                if (!::SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0))
                    throw std::runtime_error{detail::failure("protect pipe reader")};
            };
            if (!cmd.stdin_file.empty())
                file(input, cmd.stdin_file, false);
            else if (cmd.passthrough)
                inherit(input, STD_INPUT_HANDLE, false);
            else
                file(input, L"NUL", false);
            if (!cmd.stdout_file.empty())
                file(output, cmd.stdout_file, true);
            else if (cmd.passthrough)
                inherit(output, STD_OUTPUT_HANDLE, true);
            else
                pipe(out_read, output);
            if (cmd.passthrough)
                inherit(error, STD_ERROR_HANDLE, true);
            else
                pipe(err_read, error);

            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof startup;
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = input.value;
            startup.StartupInfo.hStdOutput = output.value;
            startup.StartupInfo.hStdError = error.value;
            detail::Attributes attributes;
            SIZE_T bytes{};
            ::InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes);
            attributes.list = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(::HeapAlloc(::GetProcessHeap(), 0, bytes));
            if (!attributes.list || !::InitializeProcThreadAttributeList(attributes.list, 2, 0, &bytes))
                throw std::runtime_error{detail::failure("initialize process attributes")};
            attributes.initialized = true;
            std::array<HANDLE, 3> handles{input.value, output.value, error.value};
            if (!::UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(), sizeof handles, nullptr, nullptr) ||
                !::UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job.value, sizeof job.value, nullptr, nullptr))
                throw std::runtime_error{detail::failure("configure child inheritance/job (Windows 10 or newer required)")};
            startup.lpAttributeList = attributes.list;
            PROCESS_INFORMATION child{};
            auto executable = detail::wide(*program);
            if (!::CreateProcessW(executable.c_str(), line.data(), nullptr, nullptr, TRUE,
                                  EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP,
                                  environment.data(), cmd.cwd.empty() ? nullptr : cmd.cwd.c_str(), &startup.StartupInfo, &child))
                throw std::runtime_error{detail::failure("cannot run " + *program)};
            detail::Handle process{child.hProcess}, thread{child.hThread};
            result.started = true;
            input.reset();
            output.reset();
            error.reset();
            detail::control_event.store(0);
            bool const controls = cmd.own_group && ::SetConsoleCtrlHandler(detail::record_control, TRUE);
            struct Restore
            {
                bool active;
                ~Restore() { if (active) ::SetConsoleCtrlHandler(detail::record_control, FALSE); }
            } restore{controls};
            auto deadline = std::chrono::steady_clock::now() + cmd.timeout;
            std::optional<std::chrono::steady_clock::time_point> kill_at;
            bool killed = false;
            auto kill = [&]
            {
                if (!killed)
                {
                    ::TerminateJobObject(job.value, 1);
                    killed = true;
                }
            };
            auto drain = [&](detail::Handle& stream, bool stdout_stream)
            {
                for (int turn = 0; stream && turn < 32; ++turn)
                {
                    DWORD available{};
                    if (!::PeekNamedPipe(stream.value, nullptr, 0, nullptr, &available, nullptr))
                    {
                        if (::GetLastError() != ERROR_BROKEN_PIPE)
                        {
                            result.error = detail::failure("read child pipe");
                            kill();
                        }
                        stream.reset();
                        break;
                    }
                    if (!available)
                        break;
                    std::array<char, 65536> buffer{};
                    DWORD count{};
                    if (!::ReadFile(stream.value, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &count, nullptr))
                    {
                        result.error = detail::failure("read child output");
                        kill();
                        stream.reset();
                        break;
                    }
                    std::string_view chunk{buffer.data(), count};
                    if (stdout_stream && cmd.on_stdout)
                    {
                        if (!cmd.on_stdout(chunk))
                        {
                            result.aborted = true;
                            kill();
                        }
                    }
                    else if (stdout_stream)
                    {
                        if (chunk.size() > cmd.max_capture - result.out.size())
                        {
                            result.truncated = true;
                            kill();
                        }
                        else
                            result.out += chunk;
                    }
                    else
                        detail::append_error(result, cmd, chunk);
                }
            };
            for (;;)
            {
                drain(out_read, true);
                drain(err_read, false);
                auto done = ::WaitForSingleObject(process.value, 0);
                if (done == WAIT_OBJECT_0 && !out_read && !err_read)
                {
                    if (cmd.max_file_size && !cmd.stdout_file.empty())
                    {
                        std::error_code ec;
                        auto size = std::filesystem::file_size(cmd.stdout_file, ec);
                        if (!ec && size > cmd.max_file_size)
                            result.truncated = true;
                    }
                    break;
                }
                if (done == WAIT_FAILED)
                {
                    result.error = detail::failure("wait for child");
                    kill();
                }
                auto now = std::chrono::steady_clock::now();
                if (detail::control_event.exchange(0) != 0 || (!kill_at && now >= deadline))
                {
                    result.timed_out = now >= deadline;
                    result.aborted = !result.timed_out;
                    if (controls && ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, child.dwProcessId))
                        kill_at = now + cmd.grace;
                    else
                        kill();
                }
                if (!killed && !kill_at && detail::cancelled(cmd))
                {
                    result.aborted = true;
                    kill();
                }
                if (kill_at && now >= *kill_at)
                    kill();
                if (cmd.max_file_size && !cmd.stdout_file.empty())
                {
                    std::error_code ec;
                    auto size = std::filesystem::file_size(cmd.stdout_file, ec);
                    if (!ec && size > cmd.max_file_size)
                    {
                        result.truncated = true;
                        kill();
                    }
                }
                if (killed)
                {
                    if (::WaitForSingleObject(process.value, 5000) != WAIT_OBJECT_0)
                        result.error = "child did not exit after job termination";
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            DWORD code{};
            if (!::GetExitCodeProcess(process.value, &code))
                throw std::runtime_error{detail::failure("read child exit status")};
            result.exit_code = static_cast<int>(code);
        }
        catch (std::exception const& error)
        {
            result.error = error.what();
        }
        return result;
    }

#endif
    [[nodiscard]] inline std::string linker_path(std::string const& name)
    {
#ifdef _WIN32
        std::wstring buffer(32768, L'\0');
        auto size = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size && size < buffer.size())
        {
            buffer.resize(size);
            auto path = std::filesystem::path{buffer}.parent_path() / detail::wide(name + ".exe");
            std::error_code ec;
            if (std::filesystem::is_regular_file(path, ec))
                return detail::utf8(path.native());
        }
        return name + ".exe";
#else
        return name;
#endif
    }
}
