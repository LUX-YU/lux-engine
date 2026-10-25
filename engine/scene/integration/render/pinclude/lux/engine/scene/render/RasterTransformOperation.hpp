#pragma once
#include <entt/container/dense_set.hpp>
#include <lux/engine/context/render/RenderRequester.hpp>
#include <lux/engine/scene/render/MeshInstanceRenderResourceComponent.hpp>
#include <lux/engine/scene/render/MeshRenderBinding.hpp>
#include <lux/engine/scene/render/RenderCapabilityOperation.hpp>

namespace lux::scene
{
    class RasterTransformOperation final
    {
    public:
        using SourceComponents =
            std::tuple<simulation::ecs::WorldTransform3D, detail::MeshInstanceRenderResourceComponent>;
        using ResultComponents = std::tuple<>;
        using Dependencies = std::tuple<MeshRenderBinding>;
        static constexpr std::string_view name = "lux.scene.operation.raster_transforms";
        static constexpr auto policy = ERenderUpdatePolicy::CHANGES;
        static constexpr auto scope = ERenderOperationScope::SCENE;
        static std::span<const render::RenderPayloadContract> inputs() noexcept;
        static render::RenderResult<std::unique_ptr<RasterTransformOperation>> create(RenderBindingContext&) noexcept;
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
            if constexpr (std::same_as<C, simulation::ecs::WorldTransform3D>)
            {
                if (!registry.any_of<detail::MeshInstanceRenderResourceComponent>(entity))
                {
                    return;
                }
            }
            changed_.insert(entity);
        }

    private:
        RasterTransformOperation(
            RenderBindingContext& context,
            render::CapabilityInput input,
            engine::render::BoundCommand<render::UpdateTransforms> update
        ) noexcept
            : registry_(context.registry), client_(context.client), input_(input), update_(update)
        {
        }

        simulation::ecs::Registry& registry_;
        engine::render::RenderRequester& client_;
        render::CapabilityInput input_;
        engine::render::BoundCommand<render::UpdateTransforms> update_;
        entt::dense_set<simulation::ecs::Entity> changed_;
        std::vector<simulation::ecs::Entity> prepared_;
        std::uint64_t sequence_ = 0, resynchronization_ = 0;
    };
} // namespace lux::scene
