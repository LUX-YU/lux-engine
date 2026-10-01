#pragma once
#include "../SceneSessionData.hpp"

namespace lux::editor::scene::detail
{
    struct SceneEditMemento final
    {
        SceneSessionAccess::Data& owner;
        sessions::ContentStamp expected;
        std::string label;
        ScenePreparedInput input;
        SceneConfiguration before_configuration;
        mutable std::vector<SceneObjectData> initial_objects;
    };

    [[nodiscard]] SceneEditResult<void> decodeInto(
        simulation::ecs::Registry& destination,
        simulation::ecs::Entity entity,
        const SceneComponentData& value,
        const simulation::ecs::ComponentSchema& schema,
        const simulation::ecs::WorldEntityMap& identities
    );
    [[nodiscard]] SceneEditResult<SceneComponentData> encodeComponent(
        const simulation::ecs::Registry& registry,
        simulation::ecs::Entity entity,
        const simulation::ecs::ComponentSchema& schema,
        const simulation::ecs::WorldEntityMap& identities,
        SceneBudget& budget
    );
    [[nodiscard]] editing::EditResult<editing::PreparedEditPtr> prepareSceneFields(
        const SceneEditMemento& edit,
        const editing::ApplyContext& context,
        editing::EditPreparationBudget& budget
    );
    [[nodiscard]] editing::EditResult<editing::PreparedEditPtr> prepareSceneConfiguration(
        const SceneEditMemento& edit,
        const editing::ApplyContext& context,
        editing::EditPreparationBudget& budget
    );
}
