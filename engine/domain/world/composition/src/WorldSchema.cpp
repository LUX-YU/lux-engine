#include <lux/engine/world/World.hpp>
#include <lux/engine/world/ObserverComponent.hpp>
#include <lux/engine/world/LifecyclePolicyComponent.hpp>
#include <lux/engine/world/WorldObjectId.type_static_info.hpp>

#include <lux/engine/world/ObserverComponent.ecs_schema.hpp>
#include <lux/engine/world/LifecyclePolicyComponent.ecs_schema.hpp>

namespace lux::world
{
    std::span<const ecs::ComponentSchema> worldComponentSchemas() noexcept
    {
        static const auto schemas = []
        {
            std::vector<ecs::ComponentSchema> values;
            // Identity is restored from SceneObjectData.id, never from a duplicate component payload.
            values.push_back(ecs::makeComponentSchema<WorldObjectId>(
                ecs::componentSchemaId("lux.world.WorldObjectId"), 1, ecs::EComponentSnapshotPolicy::REBUILD,
                {}, nullptr, ecs::EComponentSemanticKind::FOUNDATION
            ));
            for (const auto source : {ecs::generated::observerComponentSchemas(),
                                     ecs::generated::lifecyclePolicyComponentSchemas()})
            {
                values.insert(values.end(), source.begin(), source.end());
            }
            return values;
        }();
        return schemas;
    }
}
