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

    enum class EApplicationState : std::uint8_t
    {
        STARTING,
        SELECTING_PROJECT,
        LOADING_PROJECT,
        RUNNING,
        PROJECT_CLEANUP_FAILED,
        CLOSING,
        CLOSE_FAILED,
        CLOSED
    };

    struct ProjectLoadOutcome final
    {
        std::uint64_t request{};
        std::filesystem::path manifest_file;
        error::Error error;
        EManifestPublication manifest_publication{EManifestPublication::UNCHANGED};
        bool project_published = false;
    };

    struct CancelProjectLoadRequest final
    {
        error::Error rejection;
    };

    struct RetryProjectCleanupRequest final
    {
        error::Error rejection;
    };

    struct RetryApplicationCloseRequest final
    {
        error::Error rejection;
    };
} // namespace lux::editor
