#pragma once
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/ui/Frame.hpp>

namespace lux::editor::gui
{
    struct ComponentReadBinding final
    {
        lux::cxx::TypeToken type;
        std::string name;
        void (*draw)(scene::SceneEditor &, scene::SceneEntityRef, lux::ui::Frame &);
    };

    [[nodiscard]] std::vector<ComponentReadBinding> firstPartySceneReaders();
} // namespace lux::editor::gui
