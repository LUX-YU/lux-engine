#pragma once
#include <lux/engine/editor/sessions/SessionFactory.hpp>
#include <lux/engine/editor/scene/SceneCodec.hpp>

namespace lux::editor::scene
{
    [[nodiscard]] std::shared_ptr<sessions::SessionFactoryEntry> makeSceneSessionFactory(
        simulation::ecs::ComponentSchemaSet schemas,
        contracts::CodeLease code = contracts::CodeLease::builtin()
    );
}
