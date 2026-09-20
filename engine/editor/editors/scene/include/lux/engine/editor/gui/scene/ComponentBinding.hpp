#pragma once

#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/scene/ui/visibility.h>
#include <lux/engine/ui/Frame.hpp>

namespace lux::editor::gui
{
    class InspectorInteraction;
    struct ComponentBinding final
    {
        lux::cxx::TypeToken type;
        std::string name;
        void (*draw)(scene::SceneEditor &, scene::SceneEntityRef, lux::ui::Frame &, InspectorInteraction &);
        std::shared_ptr<const void> code_lifetime;
    };

    [[nodiscard]] LUX_EDITOR_SCENE_UI_PUBLIC std::vector<ComponentBinding> firstPartyComponentBindings();
} // namespace lux::editor::gui
