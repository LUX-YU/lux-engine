#include "thumbnail/PreviewWorldCommon.hpp"

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/SystemPhase.hpp>
#include <lux/engine/ecs/transform/systems/Transform3DSystem.hpp>
#include <lux/engine/ecs/render/systems/3d/Camera3DSystem.hpp>
#include <lux/engine/ecs/animation/systems/AnimationSystem.hpp>
#include <lux/engine/ecs/render/subsystems/ResidencySubsystem.hpp>
#include <lux/engine/ecs/render/RenderStage.hpp>
#include <lux/engine/ecs/render/subsystems/3d/MeshSubsystems.hpp>          // MeshSubsystem
#include <lux/engine/ecs/render/subsystems/3d/Camera3DUploadSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/3d/LightSubsystems.hpp>         // DirectionalLightSubsystem
#include <lux/engine/ecs/render/components/3d/MeshComponent.hpp>
#include <lux/engine/ecs/render/components/3d/Camera3DComponent.hpp>
#include <lux/engine/ecs/render/components/3d/DirectionalLightComponent.hpp>
#include <lux/engine/ecs/render/components/ViewPresentComponent.hpp>
#include <lux/engine/ecs/components/Transform3DComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/scene/SceneAsset.hpp>

#include <Eigen/Geometry>
#include <uuid.h>

namespace lux::editor
{
    bool installPreviewWorldSystems(
        lux::ecs::ScheduleBuilder& builder,
        const lux::scene::SceneDescription& description)
    {
        (void)description;
        const auto checkpoint = builder.checkpoint();
        if (!builder.add(
                std::make_unique<lux::ecs::Transform3DSystem>()) ||
            !builder.add(
                std::make_unique<lux::ecs::Camera3DSystem>()) ||
            !builder.add(
                std::make_unique<lux::ecs::AnimationSystem>()))
        {
            (void)builder.rollbackTo(checkpoint);
            return false;
        }
        return true;
    }

    bool installPreviewRendering(
        lux::ecs::ScheduleBuilder& builder,
        const lux::scene::SceneDescription&,
        std::vector<std::unique_ptr<lux::ecs::RenderStage>>& stages,
        std::vector<std::string_view>& feature_roots,
        lux::ecs::ResidencySubsystem& residency)
    {
        auto* const render = builder.services().borrow<
            lux::ecs::SceneRenderBinding>();
        auto* const active_view = builder.services().borrow<
            lux::ecs::ActiveRenderView>();
        if (!render || !active_view)
            return false;
        residency.resolveMeshOf<
            lux::ecs::MeshComponent,
            &lux::ecs::MeshComponent::mesh_asset_id,
            &lux::ecs::MeshComponent::material_asset_id>();
        const auto mesh_requirements =
            lux::ecs::MeshSubsystem::requiredRenderFeatures();
        feature_roots.insert(
            feature_roots.end(),
            mesh_requirements.begin(),
            mesh_requirements.end());
        if (!builder.add(
                std::make_unique<lux::ecs::MeshSubsystem>(
                    *render, *active_view),
                lux::ecs::kPhaseRender))
        {
            return false;
        }
        const auto light_requirements =
            lux::ecs::DirectionalLightSubsystem::requiredRenderFeatures();
        feature_roots.insert(
            feature_roots.end(),
            light_requirements.begin(),
            light_requirements.end());
        if (!builder.add(
                std::make_unique<lux::ecs::DirectionalLightSubsystem>(
                    *render, *active_view),
                lux::ecs::kPhaseRender))
        {
            return false;
        }
        stages.push_back(
            std::make_unique<lux::ecs::Camera3DUploadSubsystem>());
        return true;
    }

    lux::scene::SceneDescription
    makePreviewSceneDescription(std::string_view scene_name)
    {
        static const auto preview_namespace = uuids::uuid::from_string(
            "34bb613e-67ec-5f7d-ad21-232394516043"
        ).value();
        uuids::uuid_name_generator ids{preview_namespace};

        lux::scene::SceneDescription package;
        package.id = lux::asset::asset_id_t{ids(scene_name)};
        return package;
    }

    lux::asset::asset_id_t registerPreviewSceneAsset(
        lux::asset::AssetManager& assets,
        std::string_view scene_name)
    {
        auto description = makePreviewSceneDescription(scene_name);
        const auto id = description.id;
        if (assets.fetchAssetAs<lux::scene::SceneAsset>(id) != nullptr)
            return id;
        auto info = std::make_unique<lux::asset::AssetInfo>();
        info->id = id;
        info->type = lux::scene::kSceneAssetType;
        if (!assets.registerAsset(std::make_unique<lux::scene::SceneAsset>(
                std::move(info),
                std::make_unique<lux::scene::SceneDescription>(
                    std::move(description)))))
        {
            return {};
        }
        return id;
    }

    lux::ecs::Entity createPreviewKeyLight(lux::ecs::World& world)
    {
        const auto e = world.createEntity();
        auto& dl = world.emplace<lux::ecs::DirectionalLightComponent>(e);
        dl.direction   = Eigen::Vector3f(-0.4f, -0.8f, -0.45f).normalized();
        dl.color       = Eigen::Vector3f(1.f, 0.97f, 0.92f);
        dl.intensity   = 3.0f;
        dl.cast_shadow = true;
        return e;
    }

    lux::ecs::Entity createPreviewCamera(lux::ecs::World&            world,
                                             lux::render::RenderTargetId target,
                                             lux::math::Extent2u         extent,
                                             bool                        auto_aspect)
    {
        const auto e = world.createEntity();
        world.emplace<lux::ecs::Transform3DComponent>(e);
        world.emplace<lux::ecs::ResolvedTransform3DComponent>(e);
        {
            auto& cc = world.emplace<lux::ecs::Camera3DComponent>(e);
            cc.fov_rad     = 45.f * (3.14159265f / 180.f);
            cc.aspect      = 1.f;
            cc.auto_aspect = auto_aspect;
        }
        // 相机拥有 view:挂 ViewPresentComponent,CameraViewSystem 建 view 并合成
        // 到宿主的 target;摘掉/销毁则还回去(与编辑器视口相机同一条缝)。
        world.emplace<lux::ecs::ViewPresentComponent>(e,
            lux::ecs::ViewPresentComponent{target, 0u, extent});
        return e;
    }

    void aimPreviewCamera(lux::ecs::World&       world,
                          lux::ecs::Entity   camera,
                          const Eigen::Vector3f& eye,
                          const Eigen::Vector3f& center)
    {
        const Eigen::Vector3f f = (center - eye).normalized();
        const Eigen::Vector3f z = -f;
        const Eigen::Vector3f x = Eigen::Vector3f::UnitY().cross(z).normalized();
        const Eigen::Vector3f y = z.cross(x);
        Eigen::Matrix3f R;
        R.col(0) = x; R.col(1) = y; R.col(2) = z;
        const auto rotation = Eigen::Quaternionf(R);
        const lux::math::Position3d position{
            static_cast<double>(eye.x()),
            static_cast<double>(eye.y()),
            static_cast<double>(eye.z())};
        world.registry().patch<lux::ecs::Transform3DComponent>(
            camera,
            [&position, &rotation](auto& transform)
            {
                transform.position = position;
                transform.rotation = rotation;
            });
    }

    bool parseBuiltinId(const char* s, lux::asset::asset_id_t& out) noexcept
    {
        auto parsed = uuids::uuid::from_string(s);
        if (!parsed) return false;
        out = *parsed;
        return true;
    }

} // namespace lux::editor
