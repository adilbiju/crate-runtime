#include "crate/state.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#ifdef __linux__
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace crate {

std::string validate_name(const std::string& name) {
    if (name.empty() || name.size() > 64 || name == "." || name == "..") {
        throw std::runtime_error("container name must contain 1-64 safe characters");
    }
    if (!std::all_of(name.begin(), name.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
        })) {
        throw std::runtime_error("container name may contain only letters, digits, '.', '-', and '_'");
    }
    return name;
}

std::filesystem::path state_directory() {
    if (const char* configured = std::getenv("CRATE_STATE_DIR")) return configured;
#ifdef __linux__
    if (::geteuid() == 0) return "/run/crate";
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR")) {
        return std::filesystem::path(runtime) / "crate";
    }
    return std::filesystem::path("/tmp") / ("crate-" + std::to_string(::geteuid()));
#else
    return std::filesystem::temp_directory_path() / "crate";
#endif
}

std::filesystem::path state_path(const std::string& name) {
    return state_directory() / (validate_name(name) + ".state");
}

void write_state(const ContainerState& state) {
    const auto directory = state_directory();
    std::filesystem::create_directories(directory);
    const auto destination = state_path(state.name);
    const auto temporary = destination.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write state file: " + temporary);
    output << "name " << std::quoted(state.name) << '\n'
           << "pid " << state.supervisor_pid << '\n'
           << "start_ticks " << state.supervisor_start_ticks << '\n'
           << "container_pid " << state.container_pid << '\n'
           << "status " << std::quoted(state.status) << '\n'
           << "exit_code " << state.exit_code << '\n'
           << "started_at " << state.started_at << '\n'
           << "finished_at " << state.finished_at << '\n'
           << "rootfs " << std::quoted(state.rootfs.string()) << '\n'
           << "log " << std::quoted(state.log_path.string()) << '\n'
           << "command " << std::quoted(state.command_line) << '\n';
    output.close();
    if (!output) throw std::runtime_error("cannot flush state file: " + temporary);
    std::filesystem::rename(temporary, destination);
}

ContainerState read_state(const std::string& name) {
    std::ifstream input(state_path(name));
    if (!input) throw std::runtime_error("unknown container: " + name);
    ContainerState state;
    std::string key;
    while (input >> key) {
        if (key == "name") input >> std::quoted(state.name);
        else if (key == "pid") input >> state.supervisor_pid;
        else if (key == "start_ticks") input >> state.supervisor_start_ticks;
        else if (key == "container_pid") input >> state.container_pid;
        else if (key == "status") input >> std::quoted(state.status);
        else if (key == "exit_code") input >> state.exit_code;
        else if (key == "started_at") input >> state.started_at;
        else if (key == "finished_at") input >> state.finished_at;
        else if (key == "rootfs") { std::string value; input >> std::quoted(value); state.rootfs = value; }
        else if (key == "log") { std::string value; input >> std::quoted(value); state.log_path = value; }
        else if (key == "command") input >> std::quoted(state.command_line);
        else { std::string ignored; std::getline(input, ignored); }
    }
    if (!input.eof() || state.name != name) throw std::runtime_error("invalid state file for: " + name);
    return state;
}

std::vector<ContainerState> read_all_states() {
    std::vector<ContainerState> states;
    const auto directory = state_directory();
    if (!std::filesystem::exists(directory)) return states;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".state") continue;
        try { states.push_back(read_state(entry.path().stem().string())); }
        catch (const std::exception&) { }
    }
    std::sort(states.begin(), states.end(), [](const auto& left, const auto& right) {
        return left.started_at > right.started_at;
    });
    return states;
}

std::uint64_t process_start_ticks(std::int64_t pid) {
#ifdef __linux__
    std::ifstream input(std::filesystem::path("/proc") / std::to_string(pid) / "stat");
    std::string line;
    std::getline(input, line);
    const auto closing_name = line.rfind(')');
    if (closing_name == std::string::npos) return 0;
    std::istringstream fields(line.substr(closing_name + 2));
    std::string value;
    // Field 3 starts here; starttime is field 22.
    for (int field = 3; field <= 22; ++field) {
        if (!(fields >> value)) return 0;
    }
    try { return std::stoull(value); } catch (const std::exception&) { return 0; }
#else
    static_cast<void>(pid);
    return 0;
#endif
}

bool process_matches(const ContainerState& state) {
#ifdef __linux__
    if (state.supervisor_pid <= 0 || ::kill(static_cast<pid_t>(state.supervisor_pid), 0) != 0) {
        return false;
    }
    return state.supervisor_start_ticks != 0 &&
           process_start_ticks(state.supervisor_pid) == state.supervisor_start_ticks;
#else
    static_cast<void>(state);
    return false;
#endif
}

std::string join_command(const std::vector<std::string>& command) {
    std::ostringstream result;
    bool first = true;
    for (const auto& argument : command) {
        if (!first) result << ' ';
        first = false;
        if (argument.find_first_of(" \t\"'") == std::string::npos) result << argument;
        else result << std::quoted(argument);
    }
    return result.str();
}

}  // namespace crate
