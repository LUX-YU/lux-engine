#pragma once
#include <string>
#include <vector>

namespace lux::editor
{
    struct PaneDescription final
    {
        std::string type;
        std::string name;
        std::string title;
    };
    using EditorLayout = std::vector<PaneDescription>;
} // namespace lux::editor
