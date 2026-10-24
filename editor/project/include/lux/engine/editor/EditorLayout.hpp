#pragma once
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
} // namespace lux::editor
