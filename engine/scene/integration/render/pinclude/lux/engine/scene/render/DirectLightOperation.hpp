#pragma once

#include <lux/engine/context/render/RenderRequester.hpp>

#include <entt/container/dense_set.hpp>
#include <lux/engine/scene/render/LightRenderResourceComponent.hpp>
#include <lux/engine/scene/render/RenderCapabilityOperation.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

namespace lux::scene
{
    class DirectLightOperation final
    {
    public:
        using SourceComponents = std::
            tuple<simulation::ecs::Light3D, simulation::ecs::WorldTransform3D, detail::LightRenderResourceComponent>;
        using ResultComponents = std::tuple<>;
        static constexpr std::string_view name = "lux.scene.operation.direct_light_parameters";
        static constexpr auto scope = ERenderOperationScope::SCENE;

        [[nodiscard]] static std::span<const render::RenderPayloadContract> inputs() noexcept;
        [[nodiscard]] static render::RenderResult<std::unique_ptr<DirectLightOperation>>
        create(RenderBindingContext&) noexcept;
        [[nodiscard]] SceneStageResult maintain(SceneStageContext&) noexcept;
        [[nodiscard]] SceneStageResult prepare(SceneStageContext&) noexcept;
        [[nodiscard]] SceneStageResult publish(SceneStageContext&, engine::render::FrameInput&) noexcept;

        template <class C>
        void sourceChanged(
            simulation::ecs::Registry& registry,
            simulation::ecs::Entity entity,
            ERenderComponentChange
        ) noexcept
        {
            if constexpr (std::same_as<C, simulation::ecs::WorldTransform3D>)
            {
                if (!registry.any_of<simulation::ecs::Light3D, detail::LightRenderResourceComponent>(entity))
                {
                    return;
                }
            }
            changed_.insert(entity);
        }

    private:
        DirectLightOperation(
            RenderBindingContext&,
            render::CapabilityInput,
            engine::render::BoundCommand<render::UpdateDirectLights>
        ) noexcept;

        simulation::ecs::Registry& registry_;
        engine::render::RenderRequester& client_;
        render::CapabilityInput input_;
        engine::render::BoundCommand<render::UpdateDirectLights> update_;
        entt::dense_set<simulation::ecs::Entity> changed_;
        std::vector<simulation::ecs::Entity> prepared_;
        std::uint64_t sequence_ = 0, resynchronization_ = 0;
    };
} // namespace lux::scene
