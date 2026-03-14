#include "crate/io.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace crate::detail {

void write_text(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("cannot open " + path.string() + ": " + std::strerror(errno));
    }
    output << value;
    output.flush();
    if (!output) throw std::runtime_error("cannot write " + path.string());
    output.close();
    if (output.fail()) throw std::runtime_error("cannot close " + path.string());
}

}  // namespace crate::detail
