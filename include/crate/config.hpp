#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace crate {

struct BindMount {
    std::filesystem::path source;
    std::filesystem::path destination;
    bool read_only{false};
};

enum class NetworkMode { none, host, bridge };

struct Config {
    std::filesystem::path rootfs;
    std::filesystem::path cgroup_root{"/sys/fs/cgroup"};
    std::string name;
    std::string hostname{"crate"};
    std::optional<std::uint64_t> memory_bytes;
    std::optional<std::uint64_t> cpu_quota_us;
    std::optional<std::uint64_t> cpu_percent;
    std::uint64_t cpu_period_us{100000};
    std::optional<std::uint64_t> pids_max;
    bool read_only{false};
    bool user_namespace{false};
    NetworkMode network{NetworkMode::none};
    bool keep_environment{false};
    bool detach{false};
    std::filesystem::path log_path;
    std::string bridge_name{"crate0"};
    std::string ip_address;
    std::string gateway;
    std::uint32_t uid{0};
    std::uint32_t gid{0};
    std::vector<std::pair<std::string, std::string>> environment;
    std::vector<BindMount> binds;
    std::vector<std::string> command;
};

Config parse_run_arguments(int argc, char** argv);
void load_config_file(const std::filesystem::path& path, Config& config);
std::uint64_t parse_size(const std::string& text);
BindMount parse_bind(const std::string& text);
std::string network_mode_name(NetworkMode mode);
std::string usage();

}  // namespace crate
