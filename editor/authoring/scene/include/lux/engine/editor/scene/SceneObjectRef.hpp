#pragma once

#include <lux/engine/editor/sessions/ContentStamp.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaId.hpp>
#include <lux/engine/world/WorldObjectId.hpp>

namespace lux::editor::scene
{
    struct SceneObjectRef final
    {
        sessions::SessionId session;
        editing::HistoryId history;
        world::WorldObjectId object;
        friend bool operator==(const SceneObjectRef&, const SceneObjectRef&) noexcept = default;
    };

    struct SceneObjectLocator final
    {
        SceneObjectRef target;
        simulation::ecs::ComponentSchemaId component;
        std::string field;
    };
}
