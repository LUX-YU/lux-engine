#pragma once
#include <filesystem>
#include <string>

namespace lux::editor
{
    // Runtime summary. Persistent selections and scene records live only in ProjectManifest.
    struct ProjectDescription final
    {
        std::string name;
        std::filesystem::path root;
    };
} // namespace lux::editor
