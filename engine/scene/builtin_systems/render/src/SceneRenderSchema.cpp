#include <lux/engine/scene/SceneRenderSchema.hpp>

#include <lux/engine/scene/CameraSchema.hpp>
#include <lux/engine/scene/ResolvedMeshResources.hpp>
#include <lux/engine/serialization/Serialization.hpp>

#include <array>
#include <tuple>

namespace lux::scene
{
    std::span<const simulation::ecs::ComponentSchema> sceneRenderComponentSchemas() noexcept
    {
        using namespace simulation::ecs;
        static const std::array schemas{
            makeComponentSchema<ResolvedMeshResources>(
                componentSchemaId("lux.scene.ResolvedMeshResources"),
                1U,
                EComponentSnapshotPolicy::REBUILD,
                {},
                nullptr,
                EComponentSemanticKind::RUNTIME_DERIVED
            ),
            cameraComponentSchemas().front()
        };
        return schemas;
    }
} // namespace lux::scene
