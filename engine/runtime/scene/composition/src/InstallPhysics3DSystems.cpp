#include <lux/engine/runtime/scene/composition/InstallPhysics3DSystems.hpp>

#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/physics3d/Physics3DConfig.hpp>
#include <lux/engine/ecs/physics3d/streaming/StaticCollider3DSystem.hpp>
#include <lux/engine/ecs/physics3d/systems/Physics3DScene.hpp>
#include <lux/engine/ecs/physics3d/systems/Physics3DSystem.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>

#include <memory>
#include <string_view>

namespace lux::runtime
{
    bool installPhysics3DSystems(
        lux::ecs::ScheduleBuilder& builder,
        const lux::ecs::ComponentTypeCatalog& components,
        lux::ecs::physics3d::streaming::StaticCollider3DPrepareClient
            preparation)
    {
        using namespace lux::ecs;
        constexpr std::string_view required_components[]{
            "lux.ecs.staticcolliderbatch3dcomponent",
            "lux.ecs.rigidbody3dcomponent",
            "lux.ecs.collider3dcomponent",
            "lux.ecs.charactercontroller3dcomponent",
            "lux.ecs.collisionfilter3dcomponent"};
        if (auto validated = validateComponentSchemas(
                components, required_components); !validated)
        {
            return false;
        }

        const auto checkpoint = builder.checkpoint();
        auto* const blobs = builder.services().borrow<
            lux::ecs::entity_scene::ContentBlobClient>();
        const auto* configured = builder.services().get<Physics3DConfig>();
        auto scene = Physics3DScene::create(
            configured ? *configured : Physics3DConfig{});
        if (!blobs || !preparation || !scene)
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        auto shared_scene = std::move(*scene);
        if (!builder.services().emplace<physics3d::streaming::
                Physics3DSceneService>(
                physics3d::streaming::Physics3DSceneService{shared_scene}) ||
            !builder.add(
                std::make_unique<Physics3DSystem>(shared_scene),
                kPhaseSimulation) ||
            !builder.add(
                std::make_unique<physics3d::streaming::
                    StaticCollider3DSystem>(
                    preparation,
                    std::move(shared_scene),
                    *blobs),
                kPhaseSimulation))
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        return true;
    }
}
