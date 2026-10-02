#pragma once
#include <filesystem>
namespace lux::editor::application
{
    [[nodiscard]] int runLauncher(const std::filesystem::path& installation, unsigned smoke_frames = 0);
}
