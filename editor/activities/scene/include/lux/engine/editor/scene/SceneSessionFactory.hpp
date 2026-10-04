#pragma once
#include <lux/engine/editor/sessions/SessionFactory.hpp>
#include <lux/engine/editor/scene/SceneCodec.hpp>

namespace lux::editor::scene
{
    [[nodiscard]] sessions::SessionPreparation prepareSceneSession(
        PreparedSceneData data,
        [[nodiscard]] sessions::SourceBinding binding,
        std::optional<persistence::WriteTarget> target,
        simulation::ecs::ComponentSchemaSet schemas,
        lux::object::CodeLease code = lux::object::CodeLease::builtin()
    );
    [[nodiscard]] std::shared_ptr<sessions::SessionFactoryEntry> makeSceneSessionFactory(
        simulation::ecs::ComponentSchemaSet schemas,
        lux::object::CodeLease code = lux::object::CodeLease::builtin()
    );
}
