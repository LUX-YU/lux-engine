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
        // Canonical source binding for an opened project. Empty only for explicit in-memory bootstrap.
        std::filesystem::path manifest_file;
    };
} // namespace lux::editor
