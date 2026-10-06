#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/ui/Ids.hpp>
#include <memory>

namespace lux::ui
{
    class Element;
}
namespace lux::editor
{
    class ConfigurationValue;
}
namespace lux::editor::settings
{
    class SettingsEntry;
    // Optional presentation for one immutable settings declaration. The page owner destroys
    // its controls before releasing the entry and its defining code. No Scene dependency.
    struct SettingsPage final
    {
        using CreateResult = EditorResult<std::unique_ptr<lux::ui::Element>>;
        using Create = CreateResult (*)(lux::ui::Element&, lux::ui::ElementId, ConfigurationValue&) noexcept;
        std::shared_ptr<SettingsEntry> entry;
        Create create{};
    };
} // namespace lux::editor::settings
