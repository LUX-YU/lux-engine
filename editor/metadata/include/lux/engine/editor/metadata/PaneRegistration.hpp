#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/metadata/PaneState.hpp>
#include <lux/engine/ui/Ids.hpp>
#include <functional>
#include <memory>
#include <string>

namespace lux::ui
{
    class Pane;
}
namespace lux::editor
{
    class PaneManager;

    struct PaneRegistration final
    {
        using CreateResult = EditorResult<std::reference_wrapper<lux::ui::Pane>>;
        lux::ui::PaneTypeId type;
        std::string name;
        CreateResult (*create)(PaneManager&) noexcept {};
        std::shared_ptr<const void> code_lifetime;
        EditorResult<std::string> (*capture)(PaneManager&, const lux::ui::Pane&) noexcept {};
        CreateResult (*restore)(PaneManager&, const PaneState&) noexcept {};

        [[nodiscard]] bool valid() const noexcept
        {
            return type.isValid() && !name.empty() && create;
        }
    };
}
