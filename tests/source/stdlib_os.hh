#pragma once

#include <sys/wait.h>
#include <unistd.h>

namespace os_test
{
    inline std::string quote(std::filesystem::path const& p)
    {
        std::string out = "'";
        for (char c : p.string())
            out += c == '\'' ? "'\\''" : std::string(1, c);

        return out + "'";
    }

    inline std::string read(std::filesystem::path const& p)
    {
        std::ifstream in{p};
        return {std::istreambuf_iterator<char>{in}, {}};
    }

    struct Run
    {
        int status;
        std::string out;
        std::string err;
    };

    inline Run run(std::string_view source, bool windows = false)
    {
        auto root = std::filesystem::canonical("/proc/self/exe").parent_path().parent_path().parent_path();
        auto dir =
            std::filesystem::temp_directory_path() / std::format("dcc-stdlib-os-{}-{}", getpid(), std::chrono::steady_clock::now().time_since_epoch().count());

        std::filesystem::create_directories(dir);
        struct Cleanup
        {
            std::filesystem::path p;
            ~Cleanup()
            {
                std::error_code ec;
                std::filesystem::remove_all(p, ec);
            }
        } cleanup{dir};

        auto src = dir / "main.dc", exe = dir / "program.exe", obj = dir / "main.o";
        {
            std::ofstream f{src};
            f << source;
        }
        std::string command = quote(root / "bin/dcc") + " -flibdcext " + (windows ? "windows -target x86_64-coff -c" : "linux -target x86_64-elf") + " -o " +
                              quote(windows ? obj : exe) + " " + quote(src);

        if (std::system(command.c_str()) != 0)
            return {-1, {}, "compilation failed"};

        if (windows)
        {
            auto env = std::getenv("MINGW_SYSROOT");
            auto mingw = std::filesystem::path{env ? env : "/opt/llvm-mingw"};
            command = quote(mingw / "bin/x86_64-w64-mingw32-clang") + " -nostdlib -Wl,--entry,_start -Wl,--subsystem,console -o " + quote(exe) + " " +
                      quote(obj) + " " + quote(root / "lib/libdcext.a") + " -lkernel32 -lws2_32 -ladvapi32 -lshell32";

            if (std::system(command.c_str()) != 0)
                return {-1, {}, "link failed"};
        }

        auto wine = std::getenv("WINE");
        command = "ulimit -c 0 && cd " + quote(dir) + " && timeout 30s " + (windows ? quote(wine ? wine : "wine") + " " : "") + quote(exe) +
                  " < /dev/null > stdout 2> stderr";
        int status = std::system(command.c_str());
        Run result{WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1, read(dir / "stdout"), read(dir / "stderr")};
        if (result.status != 0 && result.status != 42 && result.status != 134)
            std::println(std::cerr, "D program status {}: {}", result.status, result.err);

        return result;
    }

    inline std::string fixture(std::string_view name)
    {
        auto root = std::filesystem::canonical("/proc/self/exe").parent_path().parent_path().parent_path().parent_path();
        return read(root / "tests/stdlib" / name);
    }

} // namespace os_test
