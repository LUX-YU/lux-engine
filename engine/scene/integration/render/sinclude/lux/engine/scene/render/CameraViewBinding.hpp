#pragma once
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/render/CameraTargetsComponent.hpp>
#include <lux/engine/scene/render/RenderComponentBinding.hpp>
#include <lux/engine/scene/render/visibility.h>
#include <lux/engine/simulation/ecs/Transform.hpp>

namespace lux::scene
{
    namespace detail
    {
        struct CameraRenderResourceComponent;
    }

    class CameraViewBinding final
    {
    public:
        using SourceComponents = std::tuple<Camera, CameraTargetsComponent, simulation::ecs::WorldTransform3D>;
        using ResultComponents = std::tuple<detail::CameraRenderResourceComponent>;
        static constexpr std::string_view name = "lux.scene.binding.camera_views";
        static constexpr bool effect_output = true;
        static constexpr system::SystemDataContract data_inputs[]{
            {"lux.scene.Camera"},
            {"lux.ecs.Transform3D"},
            {"lux.transform.world3d.ready", 1, system::ESystemDataKind::PREPARED_DATA, system::ESystemDataPhase::STABLE}
        };

        static std::span<const render::RenderPayloadContract> inputs() noexcept
        {
            return {};
        }

        static render::RenderResult<std::unique_ptr<CameraViewBinding>> create(RenderBindingContext&) noexcept;
        ~CameraViewBinding() noexcept;
        SceneStageResult maintain(SceneStageContext&) noexcept;

        // Main Scene edit safe point. These only change intent via EnTT patch; maintenance adopts results.
        [[nodiscard]] render::RenderResult<CameraTargetId>
        addCameraTarget(simulation::ecs::Entity, CameraTarget) noexcept;

        [[nodiscard]] bool updateCameraTarget(simulation::ecs::Entity, CameraTargetId, CameraTarget) noexcept;
        [[nodiscard]] bool removeCameraTarget(simulation::ecs::Entity, CameraTargetId) noexcept;
        [[nodiscard]] render::AttachmentId attachment(simulation::ecs::Entity, CameraTargetId) const noexcept;
        [[nodiscard]] render::RenderError cameraTargetError(simulation::ecs::Entity, CameraTargetId) const noexcept;
        [[nodiscard]] bool cameraTargetRetired(simulation::ecs::Entity, CameraTargetId) const noexcept;
        [[nodiscard]] exec::task<render::RenderResult<void>>
        whenCameraTargetReady(simulation::ecs::Entity, CameraTargetId) noexcept;

        [[nodiscard]] exec::task<render::RenderResult<void>>
        whenCameraTargetRetired(simulation::ecs::Entity, CameraTargetId) noexcept;

        template <class C>
        void sourceChanged(
            simulation::ecs::Registry& registry,
            simulation::ecs::Entity entity,
            ERenderComponentChange change
        ) noexcept
        {
            if constexpr (std::same_as<C, simulation::ecs::WorldTransform3D>)
            {
                if (change == ERenderComponentChange::UPDATE || !registry.any_of<Camera>(entity))
                {
                    return;
                }
            }
            cameraChanged(entity);
        }

        template <class C> void resultDestroyed(simulation::ecs::Registry&, simulation::ecs::Entity entity) noexcept
        {
            static_assert(std::same_as<C, detail::CameraRenderResourceComponent>);
            bindingsDestroyed(entity);
        }

    private:
        struct Impl;
        explicit CameraViewBinding(std::unique_ptr<Impl>) noexcept;
        void cameraChanged(simulation::ecs::Entity) noexcept;
        void bindingsDestroyed(simulation::ecs::Entity) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::scene
