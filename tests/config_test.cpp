#include "crate/config.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

crate::Config parse(std::vector<std::string> values) {
    std::vector<char*> arguments;
    for (auto& value : values) arguments.push_back(value.data());
    return crate::parse_run_arguments(static_cast<int>(arguments.size()), arguments.data());
}

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
    CHECK(crate::parse_size("1K") == 1024);
    CHECK(crate::parse_size("64M") == 64ULL * 1024ULL * 1024ULL);
    CHECK(crate::parse_size("2G") == 2ULL * 1024ULL * 1024ULL * 1024ULL);

    const auto ro = crate::parse_bind("/host/data:/data:ro");
    CHECK(ro.source == "/host/data");
    CHECK(ro.destination == "/data");
    CHECK(ro.read_only);

    const auto rw = crate::parse_bind("/host/file:/etc/file");
    CHECK(!rw.read_only);

    bool rejected = false;
    try { static_cast<void>(crate::parse_bind("bad")); }
    catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);

    const auto config = parse({"crate", "run", "--rootfs", "/", "--cpu", "25",
                               "--cpu-period", "200000", "/bin/echo", "hello"});
    CHECK(config.cpu_quota_us == 50000);
    CHECK(config.command.size() == 2);
    CHECK(config.command.front() == "/bin/echo");

    rejected = false;
    try { static_cast<void>(parse({"crate", "run", "--rootfs", "/", "--log", "/tmp/x", "--", "/bin/true"})); }
    catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);

    const auto application_config = parse({"crate", "run", "--rootfs", "/", "--",
                                           "/bin/app", "--config", "/missing/app.conf"});
    CHECK(application_config.command.size() == 3);
    CHECK(application_config.command[1] == "--config");

    const auto positional_application = parse({"crate", "run", "--rootfs", "/",
                                               "/bin/app", "--config", "/missing/app.conf"});
    CHECK(positional_application.command.size() == 3);

    rejected = false;
    try { static_cast<void>(parse({"crate", "run", "--rootfs", "/", "--uid",
                                   "4294967296", "--", "/bin/true"})); }
    catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);

    const auto oversized_config = std::filesystem::temp_directory_path() / "crate-oversized-id.conf";
    {
        std::ofstream output(oversized_config);
        output << "rootfs=/\nuid=4294967296\n";
    }
    rejected = false;
    try { static_cast<void>(parse({"crate", "run", "--config", oversized_config.string(),
                                   "--", "/bin/true"})); }
    catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);
    std::filesystem::remove(oversized_config);

    std::cout << "configuration tests passed\n";
    return failures == 0 ? 0 : 1;
}
