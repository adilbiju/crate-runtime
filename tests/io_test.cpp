#include "crate/io.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

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
    const auto path = std::filesystem::temp_directory_path() / "crate-io-test.txt";
    crate::detail::write_text(path, "hello");
    std::ifstream input(path);
    std::string value;
    input >> value;
    CHECK(value == "hello");
    std::filesystem::remove(path);

#ifdef __linux__
    bool rejected = false;
    try { crate::detail::write_text("/dev/full", "must fail"); }
    catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);
#endif

    return failures == 0 ? 0 : 1;
}
