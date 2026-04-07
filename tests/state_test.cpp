#include "crate/state.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char* expression, int line) {
    if (condition) return;
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
    ++failures;
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

}  // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() / "crate-state-test";
    std::filesystem::remove_all(directory);
#ifdef _WIN32
    _putenv_s("CRATE_STATE_DIR", directory.string().c_str());
#else
    ::setenv("CRATE_STATE_DIR", directory.c_str(), 1);
#endif
    crate::ContainerState expected;
    expected.name = "test-one";
    expected.supervisor_pid = 42;
    expected.supervisor_start_ticks = 99;
    expected.container_pid = 43;
    expected.status = "exited";
    expected.exit_code = 7;
    expected.rootfs = "/a rootfs";
    expected.log_path = directory / "test-one.log";
    expected.command_line = "/bin/sh -c \"hello world\"";
    crate::write_state(expected);

    const auto actual = crate::read_state("test-one");
    CHECK(actual.name == expected.name);
    CHECK(actual.supervisor_pid == 42);
    CHECK(actual.exit_code == 7);
    CHECK(actual.rootfs == expected.rootfs);
    CHECK(actual.command_line == expected.command_line);
    CHECK(crate::read_all_states().size() == 1);

    std::filesystem::remove_all(directory);
    std::cout << "state tests passed\n";
    return failures == 0 ? 0 : 1;
}
