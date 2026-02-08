#include "crate/config.hpp"
#include "crate/state.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace crate {
namespace {

std::string trim(std::string value) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

bool parse_bool(const std::string& value) {
    if (value == "true" || value == "yes" || value == "1") return true;
    if (value == "false" || value == "no" || value == "0") return false;
    throw std::runtime_error("invalid boolean: " + value);
}

std::uint64_t parse_u64(const std::string& value, const std::string& field) {
    std::uint64_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error("invalid " + field + ": " + value);
    }
    return result;
}

std::uint32_t parse_u32(const std::string& value, const std::string& field) {
    const auto result = parse_u64(value, field);
    if (result > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(field + " is out of range: " + value);
    }
    return static_cast<std::uint32_t>(result);
}

bool option_requires_value(const std::string& option) {
    return option == "--config" || option == "--rootfs" || option == "--cgroup-root" ||
           option == "--name" || option == "--hostname" || option == "--memory" ||
           option == "--cpu-quota" || option == "--cpu" || option == "--cpu-period" ||
           option == "--pids" || option == "--uid" || option == "--gid" ||
           option == "--env" || option == "--bind" || option == "--network" ||
           option == "--bridge" || option == "--ip" || option == "--gateway" ||
           option == "--log";
}

std::pair<std::string, std::string> parse_assignment(const std::string& value) {
    const auto separator = value.find('=');
    if (separator == std::string::npos || separator == 0) {
        throw std::runtime_error("expected KEY=VALUE, got: " + value);
    }
    return {value.substr(0, separator), value.substr(separator + 1)};
}

void apply_setting(Config& config, const std::string& raw_key, const std::string& raw_value) {
    const auto key = trim(raw_key);
    const auto value = trim(raw_value);
    if (key == "rootfs") config.rootfs = value;
    else if (key == "cgroup_root") config.cgroup_root = value;
    else if (key == "name") config.name = value;
    else if (key == "hostname") config.hostname = value;
    else if (key == "memory") config.memory_bytes = parse_size(value);
    else if (key == "cpu_quota") { config.cpu_quota_us = parse_u64(value, key); config.cpu_percent.reset(); }
    else if (key == "cpu") { config.cpu_percent = parse_u64(value, key); config.cpu_quota_us.reset(); }
    else if (key == "cpu_period") config.cpu_period_us = parse_u64(value, key);
    else if (key == "pids") config.pids_max = parse_u64(value, key);
    else if (key == "read_only") config.read_only = parse_bool(value);
    else if (key == "userns") config.user_namespace = parse_bool(value);
    else if (key == "network") {
        if (value == "none") config.network = NetworkMode::none;
        else if (value == "host") config.network = NetworkMode::host;
        else if (value == "bridge") config.network = NetworkMode::bridge;
        else throw std::runtime_error("network must be none, host, or bridge");
    }
    else if (key == "bridge") config.bridge_name = value;
    else if (key == "ip") config.ip_address = value;
    else if (key == "gateway") config.gateway = value;
    else if (key == "detach") config.detach = parse_bool(value);
    else if (key == "log") config.log_path = value;
    else if (key == "keep_environment") config.keep_environment = parse_bool(value);
    else if (key == "uid") config.uid = parse_u32(value, key);
    else if (key == "gid") config.gid = parse_u32(value, key);
    else if (key == "env") config.environment.push_back(parse_assignment(value));
    else if (key == "bind") config.binds.push_back(parse_bind(value));
    else throw std::runtime_error("unknown config key: " + key);
}

std::string require_value(int& index, int argc, char** argv, const std::string& option) {
    if (++index >= argc) throw std::runtime_error(option + " requires a value");
    return argv[index];
}

}  // namespace

std::uint64_t parse_size(const std::string& text) {
    if (text.empty()) throw std::runtime_error("size cannot be empty");
    std::string number = text;
    std::uint64_t multiplier = 1;
    const char suffix = static_cast<char>(std::toupper(static_cast<unsigned char>(text.back())));
    if (suffix == 'K' || suffix == 'M' || suffix == 'G' || suffix == 'T') {
        number.pop_back();
        if (suffix == 'K') multiplier = 1024ULL;
        if (suffix == 'M') multiplier = 1024ULL * 1024ULL;
        if (suffix == 'G') multiplier = 1024ULL * 1024ULL * 1024ULL;
        if (suffix == 'T') multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    }
    const auto amount = parse_u64(number, "size");
    if (amount > std::numeric_limits<std::uint64_t>::max() / multiplier) {
        throw std::runtime_error("size is too large: " + text);
    }
    return amount * multiplier;
}

BindMount parse_bind(const std::string& text) {
    const auto first = text.find(':');
    if (first == std::string::npos || first == 0) {
        throw std::runtime_error("bind must be SOURCE:DESTINATION[:ro]");
    }
    const auto second = text.find(':', first + 1);
    BindMount bind{text.substr(0, first), text.substr(first + 1, second - first - 1), false};
    if (bind.destination.empty() || !bind.destination.is_absolute() ||
        bind.destination.lexically_normal().string().starts_with("/../")) {
        throw std::runtime_error("bind destination must be an absolute path inside the container");
    }
    if (second != std::string::npos) {
        const auto mode = text.substr(second + 1);
        if (mode != "ro" && mode != "rw") throw std::runtime_error("bind mode must be ro or rw");
        bind.read_only = mode == "ro";
    }
    return bind;
}

void load_config_file(const std::filesystem::path& path, Config& config) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open config file: " + path.string());
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(line);
        if (line.empty() || line.starts_with('#')) continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            throw std::runtime_error(path.string() + ":" + std::to_string(line_number) + ": expected key=value");
        }
        try {
            apply_setting(config, line.substr(0, separator), line.substr(separator + 1));
        } catch (const std::exception& error) {
            throw std::runtime_error(path.string() + ":" + std::to_string(line_number) + ": " + error.what());
        }
    }
}

Config parse_run_arguments(int argc, char** argv) {
    Config config;
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--" || !argument.starts_with('-')) break;
        if (argument == "--config") {
            load_config_file(require_value(index, argc, argv, "--config"), config);
        } else if (option_requires_value(argument)) {
            if (++index >= argc) throw std::runtime_error(argument + " requires a value");
        }
    }

    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--") {
            for (++index; index < argc; ++index) config.command.emplace_back(argv[index]);
            break;
        }
        if (argument == "--config") { ++index; continue; }
        if (argument == "--rootfs") config.rootfs = require_value(index, argc, argv, argument);
        else if (argument == "--cgroup-root") config.cgroup_root = require_value(index, argc, argv, argument);
        else if (argument == "--name") config.name = require_value(index, argc, argv, argument);
        else if (argument == "--hostname") config.hostname = require_value(index, argc, argv, argument);
        else if (argument == "--memory") config.memory_bytes = parse_size(require_value(index, argc, argv, argument));
        else if (argument == "--cpu-quota") {
            config.cpu_quota_us = parse_u64(require_value(index, argc, argv, argument), "CPU quota");
            config.cpu_percent.reset();
        }
        else if (argument == "--cpu") {
            const auto percent = parse_u64(require_value(index, argc, argv, argument), "CPU percentage");
            if (percent == 0 || percent > 10000) throw std::runtime_error("--cpu must be between 1 and 10000 percent");
            config.cpu_percent = percent;
            config.cpu_quota_us.reset();
        }
        else if (argument == "--cpu-period") config.cpu_period_us = parse_u64(require_value(index, argc, argv, argument), "CPU period");
        else if (argument == "--pids") config.pids_max = parse_u64(require_value(index, argc, argv, argument), "PID limit");
        else if (argument == "--uid") config.uid = parse_u32(require_value(index, argc, argv, argument), "UID");
        else if (argument == "--gid") config.gid = parse_u32(require_value(index, argc, argv, argument), "GID");
        else if (argument == "--env") config.environment.push_back(parse_assignment(require_value(index, argc, argv, argument)));
        else if (argument == "--bind") config.binds.push_back(parse_bind(require_value(index, argc, argv, argument)));
        else if (argument == "--network") {
            const auto mode = require_value(index, argc, argv, argument);
            if (mode == "none") config.network = NetworkMode::none;
            else if (mode == "host") config.network = NetworkMode::host;
            else if (mode == "bridge") config.network = NetworkMode::bridge;
            else throw std::runtime_error("--network must be none, host, or bridge");
        }
        else if (argument == "--bridge") config.bridge_name = require_value(index, argc, argv, argument);
        else if (argument == "--ip") config.ip_address = require_value(index, argc, argv, argument);
        else if (argument == "--gateway") config.gateway = require_value(index, argc, argv, argument);
        else if (argument == "--log") config.log_path = require_value(index, argc, argv, argument);
        else if (argument == "--read-only") config.read_only = true;
        else if (argument == "--userns") config.user_namespace = true;
        else if (argument == "--keep-env") config.keep_environment = true;
        else if (argument == "--detach" || argument == "-d") config.detach = true;
        else if (!argument.starts_with('-')) {
            for (; index < argc; ++index) config.command.emplace_back(argv[index]);
            break;
        } else throw std::runtime_error("unknown option: " + argument);
    }

    if (config.rootfs.empty()) throw std::runtime_error("--rootfs is required");
    if (config.command.empty()) throw std::runtime_error("a command is required after --");
    if (config.cpu_period_us < 1000 || config.cpu_period_us > 1000000) {
        throw std::runtime_error("CPU period must be between 1000 and 1000000 microseconds");
    }
    if (config.cpu_percent) {
        if (*config.cpu_percent == 0 || *config.cpu_percent > 10000) {
            throw std::runtime_error("CPU percentage must be between 1 and 10000");
        }
        config.cpu_quota_us = config.cpu_period_us * *config.cpu_percent / 100;
    }
    if (config.user_namespace && (config.uid != 0 || config.gid != 0)) {
        throw std::runtime_error("--userns currently maps only container UID/GID 0");
    }
    if (config.network == NetworkMode::bridge) {
        if (config.user_namespace) throw std::runtime_error("bridge networking requires rootful mode");
        if (config.ip_address.empty() || config.ip_address.find('/') == std::string::npos) {
            throw std::runtime_error("bridge networking requires --ip ADDRESS/PREFIX");
        }
        if (config.gateway.empty()) throw std::runtime_error("bridge networking requires --gateway ADDRESS");
    }
    if (!config.log_path.empty() && !config.detach) {
        throw std::runtime_error("--log requires --detach");
    }
    if (config.name.empty()) {
        const auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        config.name = "crate-" + std::to_string(ticks);
    }
    validate_name(config.name);
    config.rootfs = std::filesystem::absolute(config.rootfs);
    if (!config.log_path.empty()) config.log_path = std::filesystem::absolute(config.log_path);
    return config;
}

std::string network_mode_name(NetworkMode mode) {
    if (mode == NetworkMode::host) return "host";
    if (mode == NetworkMode::bridge) return "bridge";
    return "none";
}

std::string usage() {
    return R"(Crate - a small Linux container runtime

Usage:
  crate run [options] -- COMMAND [ARG...]
  crate ps [--all]
  crate stop NAME [--timeout SECONDS]
  crate logs NAME [--follow]
  crate check

Options:
  --rootfs PATH          Container root filesystem (required)
  --cgroup-root PATH     cgroup v2 mount/delegation root
  --config PATH          Load key=value settings from a file
  --name NAME            Cgroup/container name (generated by default)
  --hostname NAME        UTS hostname (default: crate)
  --memory SIZE          Memory limit; supports K, M, G, T suffixes
  --cpu-quota USEC       CPU time allowed per period
  --cpu-period USEC      CPU period (default: 100000)
  --cpu PERCENT          CPU share as a percentage
  --pids COUNT           Maximum process count
  --uid UID              UID used for the command (default: 0)
  --gid GID              GID used for the command (default: 0)
  --env KEY=VALUE        Set an environment variable (repeatable)
  --bind SRC:DST[:ro]    Add a bind mount (repeatable)
  --read-only            Remount the root filesystem read-only
  --userns               Map the caller to root in a new user namespace
  --network MODE         none, host, or bridge
  --bridge NAME          Host bridge for bridge networking (default: crate0)
  --ip ADDRESS/PREFIX    Container address for bridge networking
  --gateway ADDRESS      Default gateway and bridge address
  -d, --detach           Run in the background
  --log PATH             Detached stdout/stderr log path
  --keep-env             Preserve the caller's environment
  -h, --help             Show this help
)";
}

}  // namespace crate
