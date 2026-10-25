#pragma once
#include <lux/engine/context/render/RenderRequester.hpp>
#include <lux/engine/scene/render/CameraRenderResourceComponent.hpp>
#include <lux/engine/scene/render/CameraViewBinding.hpp>
#include <lux/engine/scene/render/RenderCapabilityOperation.hpp>

namespace lux::scene
{
    class CameraInputOperation final
    {
    public:
        using SourceComponents = std::tuple<
            Camera,
            CameraTargetsComponent,
            simulation::ecs::WorldTransform3D,
            detail::CameraRenderResourceComponent>;
        using ResultComponents = std::tuple<>;
        using Dependencies = std::tuple<CameraViewBinding>;
        static constexpr std::string_view name = "lux.scene.operation.camera_inputs";
        static constexpr bool effect_output = true;
        static constexpr auto policy = ERenderUpdatePolicy::CHANGES;
        static constexpr auto scope = ERenderOperationScope::VIEW;
        static std::span<const render::RenderPayloadContract> inputs() noexcept;
        static render::RenderResult<std::unique_ptr<CameraInputOperation>> create(RenderBindingContext&) noexcept;
        SceneStageResult maintain(SceneStageContext&) noexcept;
        SceneStageResult prepare(SceneStageContext&) noexcept;
        SceneStageResult publish(SceneStageContext&, engine::render::FrameInput&) noexcept;

        template <class C>
        void sourceChanged(
            simulation::ecs::Registry& registry,
            simulation::ecs::Entity entity,
            ERenderComponentChange
        ) noexcept
        {
            if (entity == camera_)
            {
                changed_ = true;
            }
        }

    private:
        CameraInputOperation(
            RenderBindingContext&,
            engine::render::BoundCommand<render::UpdateAttachmentInputs>,
            engine::render::BoundCommand<render::SetTargetActive>
        ) noexcept;
        simulation::ecs::Registry& registry_;
        engine::render::RenderRequester& client_;
        engine::render::BoundCommand<render::UpdateAttachmentInputs> update_;
        engine::render::BoundCommand<render::SetTargetActive> active_;
        bool camera_input_, clipping_;
        simulation::ecs::Entity camera_;
        render::ViewId view_;
        bool changed_ = true, prepared_ = false;
        detail::CameraRenderTarget* current() const noexcept;
        std::uint64_t resynchronization_ = 0;
    };
} // namespace lux::scene
