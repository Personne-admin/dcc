module;

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

export module dpm.process;

import std;

export namespace dpm::process
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
    };

    struct Result
    {
        bool started{};
        int exit_code{-1};
        int signal{};
        bool timed_out{};
        bool aborted{};
        bool truncated{};
        std::string out;
        std::string err;
        std::string error;

        [[nodiscard]] bool ok() const { return started && exit_code == 0 && !timed_out && !aborted && !truncated; }
    };

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
    } // namespace detail

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
            if (now >= deadline)
            {
                result.timed_out = true;
                kill_child();
                break;
            }
            auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            int ready = ::poll(fds.data(), nfds, static_cast<int>(std::min<long long>(wait_ms, 1000)));
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
                {
                    if (result.err.size() < (64u << 10))
                        result.err.append(chunk.substr(0, (64u << 10) - result.err.size()));
                }
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
} // namespace dpm::process
