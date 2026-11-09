#pragma once
#include <lux/engine/editor/ProjectManifest.hpp>
#include <string>
#include <vector>

namespace lux::editor
{
    enum class EPaneRegion : unsigned char
    {
        CENTER,
        LEFT,
        RIGHT,
        BOTTOM
    };

    struct PaneDescription final
    {
        std::string type;
        // Project-layout input identity, checked for duplicates during open.
        // No persistent name-to-Pane binding is provided by the framework yet.
        std::string name;
        std::string title;
        EPaneRegion region{EPaneRegion::CENTER};
    };

    using EditorLayout = std::vector<PaneDescription>;

    enum class ELayoutSplit : std::uint8_t
    {
        LEAF,
        HORIZONTAL,
        VERTICAL
    };

    struct ProjectLayoutNode final
    {
        ELayoutSplit split{ELayoutSplit::LEAF};
        std::uint32_t first{UINT32_MAX}, second{UINT32_MAX};
        float ratio{0.5F};
        std::vector<std::string> panes;
    };

    struct ProjectLayoutSurface final
    {
        std::uint32_t node{};
        float x{}, y{}, width{1280}, height{800};
        bool floating{};
    };

    // Persistent Pane keys, never runtime PaneHandle or Context addresses.
    struct ProjectLayout final
    {
        std::uint32_t version{1};
        EditorLayout panes;
        std::vector<ProjectLayoutNode> nodes;
        std::vector<ProjectLayoutSurface> surfaces;
    };

    [[nodiscard]] ProjectLayout defaultProjectLayout(EditorLayout);
    [[nodiscard]] ProjectResult<void> validateProjectLayout(const ProjectLayout&) noexcept;
    [[nodiscard]] ProjectResult<ProjectLayout> decodeProjectLayout(std::string_view) noexcept;
    [[nodiscard]] ProjectResult<std::string> encodeProjectLayout(const ProjectLayout&) noexcept;
    [[nodiscard]] ProjectResult<ProjectLayout>
    readProjectLayout(const std::filesystem::path&, std::stop_token = {}) noexcept;
    [[nodiscard]] ProjectResult<void>
    writeProjectLayout(const std::filesystem::path&, const ProjectLayout&, std::stop_token = {}) noexcept;
} // namespace lux::editor
