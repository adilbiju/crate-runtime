#pragma once

#include "crate/config.hpp"

namespace crate {

int run(const Config& config);
int run_detached(const Config& config);
int list_containers(bool include_stopped);
int stop_container(const std::string& name, unsigned int timeout_seconds);
int show_logs(const std::string& name, bool follow);
int check_host();

}  // namespace crate
