#include "lux/opengl3/ShaderProgram.hpp"
#include <filesystem>

namespace lux::gapiwrap::opengl
{
    bool is_file_exists(const std::string& file_path)
    {
        return std::filesystem::exists(file_path) && std::filesystem::is_regular_file(file_path);
    }
}