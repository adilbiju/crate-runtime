#include "crate/runtime.hpp"
#include "crate/io.hpp"
#include "crate/state.hpp"

#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <linux/mount.h>
#include <net/if.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace crate {

#ifdef __linux__
namespace {

constexpr std::size_t kStackSize = 1024 * 1024;
constexpr int kRuntimeFailure = 125;
volatile sig_atomic_t g_container_pid = -1;
volatile sig_atomic_t g_command_pid = -1;

[[noreturn]] void fail(const std::string& operation) {
    throw std::runtime_error(operation + ": " + std::strerror(errno));
}

using detail::write_text;

std::int64_t unix_time() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

int execute(const std::vector<std::string>& command, bool quiet = false) {
    const pid_t child = ::fork();
    if (child < 0) fail("fork " + command.front());
    if (child == 0) {
        if (quiet) {
            const int null_fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
            if (null_fd >= 0) {
                ::dup2(null_fd, STDOUT_FILENO);
                ::dup2(null_fd, STDERR_FILENO);
                ::close(null_fd);
            }
        }
        std::vector<char*> arguments;
        arguments.reserve(command.size() + 1);
        for (const auto& part : command) arguments.push_back(const_cast<char*>(part.c_str()));
        arguments.push_back(nullptr);
        ::execvp(arguments.front(), arguments.data());
        _exit(127);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) fail("wait for " + command.front());
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

void execute_checked(const std::vector<std::string>& command) {
    const int status = execute(command);
    if (status != 0) {
        throw std::runtime_error("command failed (" + std::to_string(status) + "): " +
                                 join_command(command));
    }
}

std::string safe_name(std::string name) {
    if (name.empty()) name = "container-" + std::to_string(::getpid());
    for (char& ch : name) {
        const auto byte = static_cast<unsigned char>(ch);
        if (!std::isalnum(byte) && ch != '-' && ch != '_') ch = '-';
    }
    if (name == "." || name == "..") throw std::runtime_error("invalid container name");
    return name;
}

class Cgroup {
public:
    explicit Cgroup(const Config& config) {
        if (!config.memory_bytes && !config.cpu_quota_us && !config.pids_max) return;
        const auto root = std::filesystem::absolute(config.cgroup_root);
        if (!std::filesystem::exists(root / "cgroup.controllers")) {
            throw std::runtime_error("cgroup v2 is required (missing /sys/fs/cgroup/cgroup.controllers)");
        }

        std::string controllers;
        if (config.memory_bytes) controllers += "+memory ";
        if (config.cpu_quota_us) controllers += "+cpu ";
        if (config.pids_max) controllers += "+pids ";
        // Enabling an already-enabled controller is harmless on cgroup v2.
        write_text(root / "cgroup.subtree_control", controllers);

        const auto base = root / "crate";
        std::filesystem::create_directory(base);
        // Controllers must be delegated at every non-leaf level. Enabling them
        // only at the cgroup mount makes controls available on `base`, but not
        // on the per-container group created beneath it.
        write_text(base / "cgroup.subtree_control", controllers);
        path_ = base / safe_name(config.name);
        if (!std::filesystem::create_directory(path_)) {
            throw std::runtime_error("cgroup already exists: " + path_.string());
        }
        try {
            if (config.memory_bytes) write_text(path_ / "memory.max", std::to_string(*config.memory_bytes));
            if (config.cpu_quota_us) {
                write_text(path_ / "cpu.max", std::to_string(*config.cpu_quota_us) + " " +
                           std::to_string(config.cpu_period_us));
            }
            if (config.pids_max) write_text(path_ / "pids.max", std::to_string(*config.pids_max));
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
            path_.clear();
            throw;
        }
    }

    Cgroup(const Cgroup&) = delete;
    Cgroup& operator=(const Cgroup&) = delete;

    ~Cgroup() {
        if (path_.empty()) return;
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    void add(pid_t pid) const {
        if (!path_.empty()) write_text(path_ / "cgroup.procs", std::to_string(pid));
    }

private:
    std::filesystem::path path_;
};

class Network {
public:
    Network(const Config& config, pid_t container_pid) {
        if (config.network != NetworkMode::bridge) return;
        const auto suffix = std::to_string(container_pid);
        host_interface_ = ("cr" + suffix).substr(0, IFNAMSIZ - 1);
        peer_interface_ = ("cp" + suffix).substr(0, IFNAMSIZ - 1);

        if (execute({"ip", "link", "show", "dev", config.bridge_name}, true) != 0) {
            execute_checked({"ip", "link", "add", config.bridge_name, "type", "bridge"});
        }
        const auto slash = config.ip_address.find('/');
        const auto bridge_address = config.gateway + config.ip_address.substr(slash);
        execute_checked({"ip", "addr", "replace", bridge_address, "dev", config.bridge_name});
        execute_checked({"ip", "link", "set", config.bridge_name, "up"});
        execute({"ip", "link", "del", host_interface_}, true);
        execute_checked({"ip", "link", "add", host_interface_, "type", "veth", "peer", "name",
                         peer_interface_});
        try {
            execute_checked({"ip", "link", "set", host_interface_, "master", config.bridge_name});
            execute_checked({"ip", "link", "set", host_interface_, "up"});
            execute_checked({"ip", "link", "set", peer_interface_, "netns",
                             std::to_string(container_pid)});
        } catch (...) {
            execute({"ip", "link", "del", host_interface_}, true);
            host_interface_.clear();
            throw;
        }
    }

    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;
    ~Network() {
        if (!host_interface_.empty()) {
            try { execute({"ip", "link", "del", host_interface_}, true); }
            catch (const std::exception&) { }
        }
    }

    const std::string& peer_interface() const { return peer_interface_; }

private:
    std::string host_interface_;
    std::string peer_interface_;
};

void bring_loopback_up() {
    const int socket_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) fail("open network control socket");
    struct ifreq request {};
    std::strncpy(request.ifr_name, "lo", IFNAMSIZ - 1);
    if (::ioctl(socket_fd, SIOCGIFFLAGS, &request) != 0) {
        ::close(socket_fd);
        fail("read loopback flags");
    }
    request.ifr_flags = static_cast<short>(request.ifr_flags | IFF_UP | IFF_RUNNING);
    if (::ioctl(socket_fd, SIOCSIFFLAGS, &request) != 0) {
        ::close(socket_fd);
        fail("bring loopback up");
    }
    ::close(socket_fd);
}

void configure_network(const Config& config, const std::string& peer_interface) {
    if (config.network == NetworkMode::host) return;
    bring_loopback_up();
    if (config.network != NetworkMode::bridge) return;
    execute_checked({"ip", "link", "set", peer_interface, "name", "eth0"});
    execute_checked({"ip", "addr", "add", config.ip_address, "dev", "eth0"});
    execute_checked({"ip", "link", "set", "eth0", "up"});
    execute_checked({"ip", "route", "add", "default", "via", config.gateway});
}

class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) : fd_(fd) {}
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    ~ScopedFd() { if (fd_ >= 0) ::close(fd_); }
    int get() const { return fd_; }

private:
    int fd_;
};

struct MountTarget {
    ScopedFd parent;
    ScopedFd target;
    std::string leaf;
    bool directory{false};

    std::filesystem::path descriptor_path() const {
        return std::filesystem::path("/proc/self/fd") / std::to_string(target.get());
    }

    ScopedFd reopen_after_mount() const {
        int flags = O_PATH | O_NOFOLLOW | O_CLOEXEC;
        if (directory) flags |= O_DIRECTORY;
        const int fd = ::openat(parent.get(), leaf.c_str(), flags);
        if (fd < 0) fail("reopen mounted bind target");
        return ScopedFd(fd);
    }
};

ScopedFd open_directory_beneath(int parent_fd, const std::string& component,
                                const std::filesystem::path& display_path) {
    int fd = ::openat(parent_fd, component.c_str(),
                      O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) {
        if (::mkdirat(parent_fd, component.c_str(), 0755) != 0 && errno != EEXIST) {
            fail("create bind target directory " + display_path.string());
        }
        fd = ::openat(parent_fd, component.c_str(),
                      O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (fd < 0) {
        throw std::runtime_error("bind destination contains a symlink or non-directory: " +
                                 display_path.string());
    }
    return ScopedFd(fd);
}

MountTarget create_mount_target(const std::filesystem::path& rootfs,
                                const std::filesystem::path& relative,
                                const std::filesystem::path& source) {
    struct stat source_metadata {};
    if (::stat(source.c_str(), &source_metadata) != 0) fail("stat " + source.string());

    ScopedFd current(::open(rootfs.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (current.get() < 0) fail("open rootfs " + rootfs.string());

    std::vector<std::string> components;
    for (const auto& component : relative) {
        const auto value = component.string();
        if (value.empty() || value == "." || value == "..") {
            throw std::runtime_error("invalid bind destination: " + relative.string());
        }
        components.push_back(value);
    }
    if (components.empty()) throw std::runtime_error("bind destination cannot be the container root");

    std::filesystem::path display_path = rootfs;
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        display_path /= components[index];
        current = open_directory_beneath(current.get(), components[index], display_path);
    }

    const auto& leaf = components.back();
    display_path /= leaf;
    if (S_ISDIR(source_metadata.st_mode)) {
        auto target = open_directory_beneath(current.get(), leaf, display_path);
        return MountTarget{std::move(current), std::move(target), leaf, true};
    }

    int target_fd = ::openat(current.get(), leaf.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (target_fd < 0 && errno == ENOENT) {
        const int created = ::openat(current.get(), leaf.c_str(),
                                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (created < 0) fail("create bind target " + display_path.string());
        ::close(created);
        target_fd = ::openat(current.get(), leaf.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    }
    if (target_fd < 0) fail("open bind target " + display_path.string());

    struct stat target_metadata {};
    if (::fstat(target_fd, &target_metadata) != 0) {
        ::close(target_fd);
        fail("inspect bind target " + display_path.string());
    }
    if (S_ISLNK(target_metadata.st_mode) || S_ISDIR(target_metadata.st_mode)) {
        ::close(target_fd);
        throw std::runtime_error("bind file target is a symlink or directory: " + display_path.string());
    }
    return MountTarget{std::move(current), ScopedFd(target_fd), leaf, false};
}

void setup_bind_mounts(const Config& config) {
    for (const auto& bind : config.binds) {
        const auto source = std::filesystem::absolute(bind.source);
        auto relative = bind.destination.lexically_normal().relative_path();
        if (relative.empty() || *relative.begin() == "..") {
            throw std::runtime_error("bind destination escapes the rootfs: " + bind.destination.string());
        }
        auto target = create_mount_target(config.rootfs, relative, source);
        const auto destination = target.descriptor_path();
        if (::mount(source.c_str(), destination.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
            fail("bind mount " + source.string());
        }
        if (bind.read_only) {
            auto mounted_target = target.reopen_after_mount();
            struct mount_attr attributes {};
            attributes.attr_set = MOUNT_ATTR_RDONLY;
#ifdef SYS_mount_setattr
            if (::syscall(SYS_mount_setattr, mounted_target.get(), "",
                          AT_EMPTY_PATH | AT_RECURSIVE, &attributes,
                          sizeof(attributes)) != 0) {
                fail("make bind recursively read-only " + bind.destination.string());
            }
#else
            errno = ENOSYS;
            fail("make bind recursively read-only " + bind.destination.string());
#endif
        }
    }
}

void bind_device(const std::filesystem::path& device_root, const char* name) {
    const auto source = std::filesystem::path("/dev") / name;
    const auto target = device_root / name;
    const int fd = ::open(target.c_str(), O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0) fail("create device target " + target.string());
    ::close(fd);
    if (::mount(source.c_str(), target.c_str(), nullptr, MS_BIND, nullptr) != 0) {
        fail("bind device " + source.string());
    }
}

void setup_devices(const std::filesystem::path& rootfs) {
    const auto device_root = rootfs / "dev";
    std::filesystem::create_directories(device_root);
    if (::mount("tmpfs", device_root.c_str(), "tmpfs", MS_NOSUID | MS_NOEXEC,
                "mode=755") != 0) {
        fail("mount /dev");
    }
    for (const char* name : {"null", "zero", "full", "random", "urandom", "tty"}) {
        bind_device(device_root, name);
    }

    const auto pts = device_root / "pts";
    std::filesystem::create_directories(pts);
    if (::mount("devpts", pts.c_str(), "devpts", MS_NOSUID | MS_NOEXEC,
                "newinstance,ptmxmode=0666,mode=0620") != 0) {
        fail("mount /dev/pts");
    }
    if (::symlink("pts/ptmx", (device_root / "ptmx").c_str()) != 0) fail("create /dev/ptmx");
    if (::symlink("/proc/self/fd", (device_root / "fd").c_str()) != 0) fail("create /dev/fd");
    if (::symlink("/proc/self/fd/0", (device_root / "stdin").c_str()) != 0) fail("create /dev/stdin");
    if (::symlink("/proc/self/fd/1", (device_root / "stdout").c_str()) != 0) fail("create /dev/stdout");
    if (::symlink("/proc/self/fd/2", (device_root / "stderr").c_str()) != 0) fail("create /dev/stderr");

    const auto shm = device_root / "shm";
    std::filesystem::create_directories(shm);
    if (::mount("tmpfs", shm.c_str(), "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC,
                "mode=1777") != 0) {
        fail("mount /dev/shm");
    }
}

void setup_filesystem(const Config& config) {
    if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) fail("make mounts private");
    if (::mount(config.rootfs.c_str(), config.rootfs.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
        fail("bind rootfs");
    }

    setup_bind_mounts(config);
    const auto proc_target = config.rootfs / "proc";
    std::filesystem::create_directories(proc_target);
    if (::mount("proc", proc_target.c_str(), "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV,
                nullptr) != 0) {
        fail("mount /proc");
    }
    setup_devices(config.rootfs);
    const auto old_root = config.rootfs / ".crate-old-root";
    std::filesystem::create_directory(old_root);
    if (::chdir(config.rootfs.c_str()) != 0) fail("chdir rootfs");
    if (::syscall(SYS_pivot_root, ".", ".crate-old-root") != 0) fail("pivot_root");
    if (::chdir("/") != 0) fail("chdir /");
    if (::umount2("/.crate-old-root", MNT_DETACH) != 0) fail("unmount old root");
    if (::rmdir("/.crate-old-root") != 0) fail("remove old root mountpoint");

    std::filesystem::create_directories("/tmp");
    if (::mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777") != 0) {
        fail("mount /tmp");
    }
    if (config.read_only &&
        ::mount(nullptr, "/", nullptr, MS_BIND | MS_REMOUNT | MS_RDONLY, nullptr) != 0) {
        fail("remount rootfs read-only");
    }
}

void drop_privileges(const Config& config) {
    for (int capability = 0; capability <= CAP_LAST_CAP; ++capability) {
        if (::prctl(PR_CAPBSET_DROP, capability, 0, 0, 0) != 0 && errno != EINVAL) {
            fail("drop capability bounding set");
        }
    }
    if (::setgroups(0, nullptr) != 0 && errno != EPERM) fail("clear supplementary groups");
    if (::setgid(config.gid) != 0) fail("setgid");
    if (::setuid(config.uid) != 0) fail("setuid");

    __user_cap_header_struct header{};
    __user_cap_data_struct data[2]{};
    header.version = _LINUX_CAPABILITY_VERSION_3;
    header.pid = 0;
    if (::syscall(SYS_capset, &header, &data) != 0) fail("clear capabilities");
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) fail("set no_new_privs");
}

void configure_environment(const Config& config) {
    if (!config.keep_environment) {
        if (::clearenv() != 0) fail("clear environment");
        ::setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
        ::setenv("HOME", config.uid == 0 ? "/root" : "/", 1);
        ::setenv("HOSTNAME", config.hostname.c_str(), 1);
    }
    for (const auto& [key, value] : config.environment) {
        if (::setenv(key.c_str(), value.c_str(), 1) != 0) fail("set environment variable " + key);
    }
}

void init_signal_handler(int signal) {
    const auto pid = g_command_pid;
    if (pid > 0) ::kill(-pid, signal);
}

void parent_signal_handler(int signal) {
    const auto pid = g_container_pid;
    if (pid > 0) ::kill(pid, signal);
}

void install_handler(int signal, void (*handler)(int)) {
    struct sigaction action {};
    action.sa_handler = handler;
    ::sigemptyset(&action.sa_mask);
    if (::sigaction(signal, &action, nullptr) != 0) fail("sigaction");
}

void report_exec_failure(int fd, int error_number) {
    if (fd < 0) return;
    const auto* bytes = reinterpret_cast<const char*>(&error_number);
    std::size_t written = 0;
    while (written < sizeof(error_number)) {
        const ssize_t count = ::write(fd, bytes + written, sizeof(error_number) - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        written += static_cast<std::size_t>(count);
    }
    ::close(fd);
}

int run_init(const Config& config, int exec_status_fd) {
    install_handler(SIGINT, init_signal_handler);
    install_handler(SIGTERM, init_signal_handler);
    install_handler(SIGHUP, init_signal_handler);
    install_handler(SIGQUIT, init_signal_handler);

    const pid_t child = ::fork();
    if (child < 0) fail("fork command");
    if (child == 0) {
        if (::setpgid(0, 0) != 0) fail("create command process group");
        drop_privileges(config);
        configure_environment(config);
        std::vector<char*> arguments;
        arguments.reserve(config.command.size() + 1);
        for (const auto& item : config.command) arguments.push_back(const_cast<char*>(item.c_str()));
        arguments.push_back(nullptr);
        ::execvp(arguments.front(), arguments.data());
        const int exec_error = errno;
        report_exec_failure(exec_status_fd, exec_error);
        std::cerr << "crate: exec " << config.command.front() << ": " << std::strerror(exec_error) << '\n';
        _exit(exec_error == ENOENT ? 127 : 126);
    }
    ::close(exec_status_fd);
    g_command_pid = child;

    int main_status = kRuntimeFailure << 8;
    bool main_exited = false;
    while (true) {
        int status = 0;
        const pid_t reaped = ::waitpid(-1, &status, 0);
        if (reaped < 0) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) break;
            fail("waitpid");
        }
        if (reaped == child) {
            main_status = status;
            main_exited = true;
            g_command_pid = -1;
        }
        if (main_exited) {
            // Terminate any daemons left behind by the main process, then reap them.
            ::kill(-1, SIGKILL);
        }
    }
    if (WIFEXITED(main_status)) return WEXITSTATUS(main_status);
    if (WIFSIGNALED(main_status)) return 128 + WTERMSIG(main_status);
    return kRuntimeFailure;
}

struct ChildContext {
    const Config* config;
    int start_fd;
    int exec_status_fd;
};

struct StartMessage {
    char peer_interface[IFNAMSIZ]{};
};

int decode_wait_status(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return kRuntimeFailure;
}

int enter_container(const Config& config, const std::string& peer_interface, int exec_status_fd) {
    if (::sethostname(config.hostname.c_str(), config.hostname.size()) != 0) {
        fail("sethostname");
    }
    configure_network(config, peer_interface);
    setup_filesystem(config);
    return run_init(config, exec_status_fd);
}

int run_user_namespace_supervisor(const Config& config, const std::string& peer_interface,
                                  int exec_status_fd) {
    int flags = CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID;
    if (config.network != NetworkMode::host) flags |= CLONE_NEWNET;
    if (::unshare(flags) != 0) fail("create namespaces inside user namespace");

    install_handler(SIGINT, init_signal_handler);
    install_handler(SIGTERM, init_signal_handler);
    install_handler(SIGHUP, init_signal_handler);
    install_handler(SIGQUIT, init_signal_handler);

    const pid_t child = ::fork();
    if (child < 0) fail("fork namespace init");
    if (child == 0) return enter_container(config, peer_interface, exec_status_fd);
    ::close(exec_status_fd);
    if (::setpgid(child, child) != 0 && errno != EACCES) fail("group namespace init");
    g_command_pid = child;

    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) fail("wait for namespace init");
    }
    g_command_pid = -1;
    return decode_wait_status(status);
}

int child_entry(void* opaque) {
    const auto& context = *static_cast<ChildContext*>(opaque);
    try {
        StartMessage message{};
        ssize_t count;
        do { count = ::read(context.start_fd, &message, sizeof(message)); } while (count < 0 && errno == EINTR);
        ::close(context.start_fd);
        if (count != static_cast<ssize_t>(sizeof(message))) {
            throw std::runtime_error("parent failed during container setup");
        }
        const std::string peer_interface(message.peer_interface);
        if (context.config->user_namespace) {
            return run_user_namespace_supervisor(*context.config, peer_interface,
                                                 context.exec_status_fd);
        }
        return enter_container(*context.config, peer_interface, context.exec_status_fd);
    } catch (const std::exception& error) {
        report_exec_failure(context.exec_status_fd, ECANCELED);
        std::cerr << "crate: container setup: " << error.what() << '\n';
        return kRuntimeFailure;
    }
}

void write_user_mapping(pid_t pid) {
    const auto proc = std::filesystem::path("/proc") / std::to_string(pid);
    const auto setgroups = proc / "setgroups";
    if (std::filesystem::exists(setgroups)) write_text(setgroups, "deny");
    write_text(proc / "uid_map", "0 " + std::to_string(::getuid()) + " 1");
    write_text(proc / "gid_map", "0 " + std::to_string(::getgid()) + " 1");
}

void install_parent_handlers() {
    install_handler(SIGINT, parent_signal_handler);
    install_handler(SIGTERM, parent_signal_handler);
    install_handler(SIGHUP, parent_signal_handler);
    install_handler(SIGQUIT, parent_signal_handler);
}

}  // namespace

int run_impl(const Config& config, int ready_fd) {
    if (::geteuid() != 0 && !config.user_namespace) {
        throw std::runtime_error("root privileges are required unless --userns is used");
    }
    if (!std::filesystem::is_directory(config.rootfs)) {
        throw std::runtime_error("rootfs is not a directory: " + config.rootfs.string());
    }
    try {
        const auto previous = read_state(config.name);
        if (previous.status == "running" && process_matches(previous)) {
            throw std::runtime_error("container is already running: " + config.name);
        }
    } catch (const std::runtime_error& error) {
        if (std::string(error.what()).find("unknown container:") == std::string::npos) throw;
    }

    int start_pipe[2];
    if (::pipe2(start_pipe, O_CLOEXEC) != 0) fail("pipe2");
    int exec_status_pipe[2];
    if (::pipe2(exec_status_pipe, O_CLOEXEC) != 0) {
        ::close(start_pipe[0]);
        ::close(start_pipe[1]);
        fail("exec status pipe");
    }
    std::vector<char> stack(kStackSize);
    ChildContext context{&config, start_pipe[0], exec_status_pipe[1]};
    int flags = SIGCHLD;
    if (config.user_namespace) {
        // User mappings must be installed before the child creates namespaces
        // owned by that user namespace (notably the PID namespace used by /proc).
        flags |= CLONE_NEWUSER;
    } else {
        flags |= CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC;
        if (config.network != NetworkMode::host) flags |= CLONE_NEWNET;
    }

    const pid_t child = ::clone(child_entry, stack.data() + stack.size(), flags, &context);
    if (child < 0) {
        ::close(start_pipe[0]);
        ::close(start_pipe[1]);
        ::close(exec_status_pipe[0]);
        ::close(exec_status_pipe[1]);
        fail("clone namespaces");
    }
    ::close(start_pipe[0]);
    ::close(exec_status_pipe[1]);
    g_container_pid = child;

    std::unique_ptr<Cgroup> cgroup;
    std::unique_ptr<Network> network;
    bool state_written = false;
    ContainerState state;
    state.name = config.name;
    state.supervisor_pid = ::getpid();
    state.supervisor_start_ticks = process_start_ticks(::getpid());
    state.container_pid = child;
    state.status = "running";
    state.started_at = unix_time();
    state.rootfs = config.rootfs;
    state.log_path = config.log_path;
    state.command_line = join_command(config.command);
    try {
        if (config.user_namespace) write_user_mapping(child);
        cgroup = std::make_unique<Cgroup>(config);
        cgroup->add(child);
        network = std::make_unique<Network>(config, child);
        StartMessage message{};
        if (!network->peer_interface().empty()) {
            std::strncpy(message.peer_interface, network->peer_interface().c_str(), IFNAMSIZ - 1);
        }
        if (::write(start_pipe[1], &message, sizeof(message)) != static_cast<ssize_t>(sizeof(message))) {
            fail("release container child");
        }
        ::close(start_pipe[1]);
        start_pipe[1] = -1;
        install_parent_handlers();

        int exec_error = 0;
        std::size_t received = 0;
        auto* bytes = reinterpret_cast<char*>(&exec_error);
        while (received < sizeof(exec_error)) {
            const ssize_t count = ::read(exec_status_pipe[0], bytes + received,
                                         sizeof(exec_error) - received);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) fail("read exec status");
            if (count == 0) break;
            received += static_cast<std::size_t>(count);
        }
        ::close(exec_status_pipe[0]);
        exec_status_pipe[0] = -1;
        if (received != 0) {
            if (received != sizeof(exec_error)) {
                throw std::runtime_error("container returned an incomplete exec status");
            }
            if (exec_error == ECANCELED) {
                throw std::runtime_error("container setup failed before exec");
            }
            // Detached callers must not be told startup succeeded when exec
            // failed. Foreground callers, however, should wait for PID 1 and
            // return the command process's conventional 126/127 exit status.
            if (ready_fd >= 0) {
                throw std::runtime_error("cannot execute " + config.command.front() + ": " +
                                         std::strerror(exec_error));
            }
        }
        write_state(state);
        state_written = true;
        if (ready_fd >= 0) {
            const char ready = 1;
            if (::write(ready_fd, &ready, 1) != 1) fail("notify detached parent");
            ::close(ready_fd);
            ready_fd = -1;
        }

        int status = 0;
        while (::waitpid(child, &status, 0) < 0) {
            if (errno != EINTR) fail("wait for container");
        }
        g_container_pid = -1;
        const int exit_code = decode_wait_status(status);
        state.status = "exited";
        state.exit_code = exit_code;
        state.finished_at = unix_time();
        write_state(state);
        network.reset();
        cgroup.reset();
        return exit_code;
    } catch (...) {
        if (ready_fd >= 0) ::close(ready_fd);
        if (start_pipe[1] >= 0) ::close(start_pipe[1]);
        if (exec_status_pipe[0] >= 0) ::close(exec_status_pipe[0]);
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
        g_container_pid = -1;
        if (state_written) {
            state.status = "failed";
            state.exit_code = kRuntimeFailure;
            state.finished_at = unix_time();
            try { write_state(state); } catch (const std::exception&) { }
        }
        network.reset();
        cgroup.reset();
        throw;
    }
}

int run(const Config& config) {
    return run_impl(config, -1);
}

int run_detached(const Config& original) {
    Config config = original;
    std::filesystem::create_directories(state_directory());
    if (config.log_path.empty()) config.log_path = state_directory() / (config.name + ".log");

    int ready_pipe[2];
    if (::pipe2(ready_pipe, O_CLOEXEC) != 0) fail("create detach pipe");
    const pid_t supervisor = ::fork();
    if (supervisor < 0) fail("fork detached supervisor");
    if (supervisor == 0) {
        ::close(ready_pipe[0]);
        if (::setsid() < 0) _exit(kRuntimeFailure);
        const int input = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        const int log = ::open(config.log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (input < 0 || log < 0) _exit(kRuntimeFailure);
        ::dup2(input, STDIN_FILENO);
        ::dup2(log, STDOUT_FILENO);
        ::dup2(log, STDERR_FILENO);
        ::close(input);
        ::close(log);
        try { _exit(run_impl(config, ready_pipe[1])); }
        catch (const std::exception& error) {
            std::cerr << "crate: " << error.what() << '\n';
            ::close(ready_pipe[1]);
            _exit(kRuntimeFailure);
        }
    }
    ::close(ready_pipe[1]);
    char ready = 0;
    ssize_t count;
    do { count = ::read(ready_pipe[0], &ready, 1); } while (count < 0 && errno == EINTR);
    ::close(ready_pipe[0]);
    if (count != 1) {
        int status = 0;
        ::waitpid(supervisor, &status, 0);
        throw std::runtime_error("detached container failed to start; see " + config.log_path.string());
    }
    std::cout << config.name << '\n';
    return 0;
}

int list_containers(bool include_stopped) {
    std::cout << std::left << std::setw(22) << "NAME" << std::setw(10) << "PID"
              << std::setw(12) << "STATUS" << "COMMAND\n";
    for (auto state : read_all_states()) {
        const bool running = state.status == "running" && process_matches(state);
        if (!include_stopped && !running) continue;
        if (!running && state.status == "running") state.status = "stale";
        std::cout << std::left << std::setw(22) << state.name
                  << std::setw(10) << (running ? std::to_string(state.supervisor_pid) : "-")
                  << std::setw(12) << state.status << state.command_line << '\n';
    }
    return 0;
}

int stop_container(const std::string& name, unsigned int timeout_seconds) {
    const auto state = read_state(validate_name(name));
    if (state.status != "running" || !process_matches(state)) {
        throw std::runtime_error("container is not running: " + name);
    }
    if (::kill(static_cast<pid_t>(state.supervisor_pid), SIGTERM) != 0) fail("stop " + name);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!process_matches(state)) return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (process_matches(state) && state.container_pid > 0) {
        if (::kill(static_cast<pid_t>(state.container_pid), SIGKILL) != 0 && errno != ESRCH) {
            fail("kill container init for " + name);
        }
        const auto kill_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (process_matches(state) && std::chrono::steady_clock::now() < kill_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (process_matches(state) && ::kill(static_cast<pid_t>(state.supervisor_pid), SIGKILL) != 0) {
        fail("kill supervisor for " + name);
    }
    return 0;
}

int show_logs(const std::string& name, bool follow) {
    auto state = read_state(validate_name(name));
    if (state.log_path.empty()) throw std::runtime_error("container has no managed log: " + name);
    std::uintmax_t offset = 0;
    while (true) {
        std::ifstream input(state.log_path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open log: " + state.log_path.string());
        input.seekg(static_cast<std::streamoff>(offset));
        char buffer[8192];
        while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
            const auto count = input.gcount();
            std::cout.write(buffer, count);
            offset += static_cast<std::uintmax_t>(count);
        }
        std::cout.flush();
        if (!follow || state.status != "running" || !process_matches(state)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        state = read_state(name);
    }
    return 0;
}

int check_host() {
    bool okay = true;
    std::cout << "Crate host compatibility\n";
    std::cout << "  Linux namespaces: available\n";
    if (std::filesystem::exists("/sys/fs/cgroup/cgroup.controllers")) {
        std::cout << "  cgroup v2:        available\n";
    } else {
        std::cout << "  cgroup v2:        unavailable\n";
        okay = false;
    }
    std::cout << "  effective UID:    " << ::geteuid()
              << (::geteuid() == 0 ? " (root)\n" : " (use --userns or sudo)\n");
    return okay ? 0 : 1;
}

#else

int run(const Config&) {
    throw std::runtime_error("Crate requires Linux namespaces and cgroup v2; this host is not Linux");
}

int run_detached(const Config&) {
    throw std::runtime_error("Crate requires Linux namespaces and cgroup v2; this host is not Linux");
}

int list_containers(bool) {
    throw std::runtime_error("Crate process management requires Linux");
}

int stop_container(const std::string&, unsigned int) {
    throw std::runtime_error("Crate process management requires Linux");
}

int show_logs(const std::string&, bool) {
    throw std::runtime_error("Crate process management requires Linux");
}

int check_host() {
    std::cout << "Crate host compatibility\n  Linux namespaces: unavailable (Crate requires Linux)\n";
    return 1;
}

#endif

}  // namespace crate
