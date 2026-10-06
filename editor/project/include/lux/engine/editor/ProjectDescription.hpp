#pragma once
#include <filesystem>
#include <lux/engine/editor/FrameworkResult.hpp>
#include <string>

namespace lux::editor
{
    // In-memory bootstrap only. This is not a new project file format.
    struct ProjectDescription final
    {
        std::string name;
        std::filesystem::path root;
    };
    [[nodiscard]] FrameworkResult<void> validateProject(const ProjectDescription&) noexcept;
} // namespace lux::editor
