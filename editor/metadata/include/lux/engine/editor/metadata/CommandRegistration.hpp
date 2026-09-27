#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/ui/Menu.hpp>
#include <functional>
#include <memory>

namespace lux::editor
{
    class EditorContext;

    struct CommandRegistration final
    {
        lux::ui::CommandId id;
        std::string label;
        std::string menu;
        std::string shortcut_label;
        lux::ui::Shortcut shortcut;
        // Declared before the callable: its destructor must return before code is released.
        std::shared_ptr<const void> code_lifetime;
        std::function<EditorResult<void>(EditorContext&, lux::ui::Command&)> invoke;

        [[nodiscard]] bool valid() const noexcept
        {
            return id.isValid() && !label.empty() && bool(invoke);
        }
    };
}
