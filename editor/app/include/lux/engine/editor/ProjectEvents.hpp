#pragma once
#include <lux/engine/editor/ProjectManifest.hpp>
#include <lux/engine/error/Error.hpp>

namespace lux::editor
{
    struct OpenProjectRequest final
    {
        std::filesystem::path manifest_file;
        // Synchronous admission failure. An admitted request completes through project facts.
        error::Error rejection;
    };
    struct CreateProjectRequest final
    {
        std::filesystem::path root;
        ProjectManifest manifest;
        error::Error rejection;
    };
    struct CloseProjectRequest final
    {
    };
    struct ProjectOpenFailure final
    {
        std::filesystem::path manifest_file;
        error::Error error;
        bool manifest_published{};
    };
} // namespace lux::editor
