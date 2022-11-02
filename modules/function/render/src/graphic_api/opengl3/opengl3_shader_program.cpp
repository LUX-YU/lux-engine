#include "lux-engine/function/graphic_api/opengl3/ShaderProgram.hpp"
#include <filesystem>

namespace lux::engine::function
{
    bool is_file_exists(const std::string& file_path)
    {
        return std::filesystem::exists(file_path) && std::filesystem::is_regular_file(file_path);
    }
}