#pragma once

#include <filesystem>
#include <string>

namespace crate::detail {

void write_text(const std::filesystem::path& path, const std::string& value);

}  // namespace crate::detail
