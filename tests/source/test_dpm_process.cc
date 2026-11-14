import std;
import dcc.config;
import dpm.process;

#define main harness_main
#include "harness.hh"
#undef main

namespace
{
    char** arguments;
    std::string executable;
    using namespace std::chrono_literals;

    dpm::process::Command child(std::vector<std::string> args)
    {
        dpm::process::Command command;
        command.argv = {executable};
        command.argv.insert(command.argv.end(), args.begin(), args.end());
        command.timeout = 5s;
        return command;
    }
}

TEST_CASE("windows command line quoting preserves crt boundaries")
{
    CHECK_EQ(dpm::process::windows_quote(""), "\"\"");
    CHECK_EQ(dpm::process::windows_quote("two words"), "\"two words\"");
    CHECK_EQ(dpm::process::windows_quote("a\"b"), "\"a\\\"b\"");
    CHECK_EQ(dpm::process::windows_quote("C:\\tail\\"), "\"C:\\tail\\\\\"");
    CHECK_EQ(dpm::process::windows_quote("a\\\"b"), "\"a\\\\\\\"b\"");
    CHECK_EQ(dpm::process::windows_quote("a;&|%"), "\"a;&|%\"");
}

TEST_CASE("native process arguments environment streams and status")
{
    auto command = child({"--echo", "", "two words", "a\"b", "tail\\", "a;&|%", "\xE5\xB7\xA5\xE5\x85\xB7"});
    auto result = dpm::process::run(command);
    REQUIRE(result.ok());
    CHECK_EQ(result.out, "0:\n9:two words\n3:a\"b\n5:tail\\\n5:a;&|%\n6:\xE5\xB7\xA5\xE5\x85\xB7\n");
    command = child({"--env"});
    command.env["DPM_PROCESS_TEST"] = "hello";
    result = dpm::process::run(command);
    CHECK(result.ok());
    CHECK_EQ(result.out, "hello");
    command = child({"--exit"});
    result = dpm::process::run(command);
    CHECK(result.started);
    CHECK_EQ(result.exit_code, 17);
    command.argv[0] += "-missing";
    result = dpm::process::run(command);
    CHECK(!result.started);
    CHECK(!result.error.empty());
}

TEST_CASE("native process drains both pipes and bounds capture")
{
    auto command = child({"--flood"});
    auto result = dpm::process::run(command);
    REQUIRE(result.ok());
    CHECK_EQ(result.out.size(), 200000u);
    CHECK_EQ(result.err.size(), 65536u);
    command.max_capture = 100;
    result = dpm::process::run(command);
    CHECK(result.truncated);
    CHECK(!result.ok());
    command.max_capture = 1000000;
    std::size_t bytes = 0;
    command.on_stdout = [&](std::string_view data) { bytes += data.size(); return true; };
    result = dpm::process::run(command);
    CHECK(result.ok());
    CHECK_EQ(bytes, 200000u);
    command.on_stdout = [](std::string_view) { return false; };
    result = dpm::process::run(command);
    CHECK(result.aborted);
}

TEST_CASE("native process timeout and child working directory")
{
    auto command = child({"--sleep"});
    command.timeout = 30ms;
    auto result = dpm::process::run(command);
    CHECK(result.started);
    CHECK(result.timed_out);
    command = child({"--cwd"});
    command.cwd = std::filesystem::temp_directory_path();
    result = dpm::process::run(command);
    CHECK(result.ok());
    CHECK_EQ(result.out, std::filesystem::canonical(command.cwd).generic_string());
}

int main(int argc, char** argv)
{
    arguments = argv;
    if (argc > 1)
    {
        std::string_view mode{argv[1]};
        if (mode == "--echo")
            for (int i = 2; i < argc; ++i)
                std::print("{}:{}\n", std::strlen(argv[i]), argv[i]);
        else if (mode == "--env")
            std::print("{}", std::getenv("DPM_PROCESS_TEST") ? std::getenv("DPM_PROCESS_TEST") : "");
        else if (mode == "--exit")
            return 17;
        else if (mode == "--flood")
        {
            std::print("{}", std::string(200000, 'a'));
            std::print(std::cerr, "{}", std::string(200000, 'b'));
        }
        else if (mode == "--sleep")
            std::this_thread::sleep_for(std::chrono::seconds(10));
        else if (mode == "--cwd")
            std::print("{}", std::filesystem::canonical(std::filesystem::current_path()).generic_string());
        return 0;
    }
    executable = dcc::config::detect_exe_path(arguments).string();
    return test::run();
}
