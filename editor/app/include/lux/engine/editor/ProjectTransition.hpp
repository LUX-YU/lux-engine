#pragma once
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/editor/ProjectManifest.hpp>

namespace lux::editor
{
    // Product configuration locates plugin catalogs; manifests contain only portable selections.
    struct ProjectPluginLocation final
    {
        std::filesystem::path catalog;
        std::filesystem::path root;
    };
    struct ProjectCreateRequest final
    {
        std::filesystem::path root;
        ProjectManifest manifest;
    };
    enum class EProjectTransition : std::uint8_t
    {
        IDLE,
        PREPARING,
        CLOSING_CURRENT,
        SUCCEEDED,
        FAILED,
        CANCELLED
    };
    // One host transition at a time. Terminal facts remain until the next admitted request.
    struct ProjectTransitionStatus final
    {
        EProjectTransition state{EProjectTransition::IDLE};
        error::Error failure;
        std::filesystem::path manifest_file;
        // CREATE may publish a file even if preparation/adoption is subsequently cancelled or fails.
        bool manifest_published{};
    };
} // namespace lux::editor
