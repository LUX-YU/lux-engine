#pragma once
#include <lux/engine/scene/ScenePackage.hpp>
#include <lux/engine/simulation/ecs/ComponentSchema.hpp>
#include <lux/engine/simulation/ecs/WorldEntityMap.hpp>

namespace lux::scene
{
    struct CapturedComponent final
    {
        lux::world::WorldObjectId object;
        std::uint32_t schema;
        std::uint32_t version;
        lux::simulation::ecs::ComponentCapture value;
    };

    struct SceneCapture final
    {
        struct Object final
        {
            lux::world::WorldObjectId id;
            lux::partition::PartitionOrdinal partition;
        };
        std::shared_ptr<const ScenePackage> source;
        lux::simulation::ecs::WorldEntityMap identities;
        std::vector<CapturedComponent> components;
        std::vector<Object> objects;
        bool structure_changed{};
        // Complete sorted schema directory for this capture; existing unknown schemas remain present.
        std::vector<lux::world::WorldDataSchemaId> schemas;
    };

    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC lux::cxx::expected<ScenePackage, ScenePackageFailure> buildScenePackage(
        const SceneCapture&,
        std::size_t max_bytes,
        std::stop_token = {}
    );
}
