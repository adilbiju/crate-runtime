#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace crate {

struct ContainerState {
    std::string name;
    std::int64_t supervisor_pid{0};
    std::uint64_t supervisor_start_ticks{0};
    std::int64_t container_pid{0};
    std::string status{"running"};
    int exit_code{-1};
    std::int64_t started_at{0};
    std::int64_t finished_at{0};
    std::filesystem::path rootfs;
    std::filesystem::path log_path;
    std::string command_line;
};

std::filesystem::path state_directory();
std::filesystem::path state_path(const std::string& name);
void write_state(const ContainerState& state);
ContainerState read_state(const std::string& name);
std::vector<ContainerState> read_all_states();
bool process_matches(const ContainerState& state);
std::uint64_t process_start_ticks(std::int64_t pid);
std::string validate_name(const std::string& name);
std::string join_command(const std::vector<std::string>& command);

}  // namespace crate
