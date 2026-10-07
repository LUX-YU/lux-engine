#pragma once
#include <string>
#include <vector>

namespace lux::editor
{
    struct PaneDescription final
    {
        std::string type;
        // Project-layout input identity, checked for duplicates during open.
        // No persistent name-to-Pane binding is provided by the framework yet.
        std::string name;
        std::string title;
    };
    using EditorLayout = std::vector<PaneDescription>;
} // namespace lux::editor
