#pragma once
#include <lux/engine/editor/ui/scene/visibility.h>
#include <lux/engine/editor/ui/shell/WindowSpec.hpp>
#include <lux/engine/editor/sessions/scene/SceneSession.hpp>
#include <lux/engine/ui/Frame.hpp>
#include <string>
#include <vector>
namespace lux::editor::ui
{
    struct ComponentReadBinding final
    {
        lux::cxx::TypeToken type;
        std::string canonical_schema;
        std::uint32_t version{};
        std::string display_name;
        void (*draw)(sessions::SceneSession &, sessions::SceneEntityRef, lux::ui::Frame &){};
        std::shared_ptr<const void> code_lifetime;
    };
    [[nodiscard]] LUX_EDITOR_SCENE_UI_PUBLIC std::vector<ComponentReadBinding> firstPartySceneReaders();
} // namespace lux::editor::ui
