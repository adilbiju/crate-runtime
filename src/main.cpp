#include "crate/config.hpp"
#include "crate/runtime.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
            std::cout << crate::usage();
            return argc < 2 ? 2 : 0;
        }
        const std::string command = argv[1];
        if (command == "run" && argc == 3 &&
            (std::string(argv[2]) == "--help" || std::string(argv[2]) == "-h")) {
            std::cout << crate::usage();
            return 0;
        }
        if (command == "check") return crate::check_host();
        if (command == "ps") {
            if (argc > 3 || (argc == 3 && std::string(argv[2]) != "--all")) {
                throw std::runtime_error("usage: crate ps [--all]");
            }
            return crate::list_containers(argc == 3);
        }
        if (command == "stop") {
            if (argc < 3) throw std::runtime_error("usage: crate stop NAME [--timeout SECONDS]");
            unsigned int timeout = 10;
            if (argc == 5 && std::string(argv[3]) == "--timeout") {
                timeout = static_cast<unsigned int>(std::stoul(argv[4]));
            } else if (argc != 3) {
                throw std::runtime_error("usage: crate stop NAME [--timeout SECONDS]");
            }
            return crate::stop_container(argv[2], timeout);
        }
        if (command == "logs") {
            if (argc < 3 || argc > 4) throw std::runtime_error("usage: crate logs NAME [--follow]");
            const bool follow = argc == 4 && std::string(argv[3]) == "--follow";
            if (argc == 4 && !follow) throw std::runtime_error("usage: crate logs NAME [--follow]");
            return crate::show_logs(argv[2], follow);
        }
        if (command != "run") {
            std::cerr << "crate: unknown command: " << command << "\n\n" << crate::usage();
            return 2;
        }
        const auto config = crate::parse_run_arguments(argc, argv);
        return config.detach ? crate::run_detached(config) : crate::run(config);
    } catch (const std::exception& error) {
        std::cerr << "crate: " << error.what() << '\n';
        return 125;
    }
}
