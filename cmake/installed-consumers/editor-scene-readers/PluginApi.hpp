#pragma once
#include <lux/engine/editor/ui/scene/ComponentReadBinding.hpp>
struct PluginApi final
{
    lux::simulation::ecs::ComponentSchema schema;
    lux::editor::ui::ComponentReadBinding reader;
    lux::simulation::ecs::Entity (*populate)(lux::simulation::ecs::Registry &);
    bool (*verify)(lux::editor::sessions::SceneSession &, lux::editor::sessions::SceneEntityRef);
    unsigned (*drawCalls)();
};
using OpenPlugin = void (*)(PluginApi *);
