#pragma once

#include <exec/task.hpp>
#include <lux/engine/context/render/RenderRequester.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/scene/render/DirectLightValues.hpp>

#include <lux/engine/scene/render/LightRenderResourceComponent.hpp>
#include <lux/engine/scene/render/RenderComponentBinding.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <unordered_map>

namespace lux::scene
{
    class DirectLightBinding final
    {
    public:
        using SourceComponents = std::tuple<simulation::ecs::Light3D, simulation::ecs::WorldTransform3D>;
        using ResultComponents = std::tuple<detail::LightRenderResourceComponent>;
        static constexpr std::string_view name = "lux.scene.binding.direct_lights";
        static constexpr system::SystemDataContract data_inputs[]{
            {"lux.ecs.Transform3D"},
            {"lux.transform.world3d.ready", 1, system::ESystemDataKind::PREPARED_DATA, system::ESystemDataPhase::STABLE}
        };

        [[nodiscard]] static std::span<const render::RenderPayloadContract> inputs() noexcept;
        [[nodiscard]] static render::RenderResult<std::unique_ptr<DirectLightBinding>>
        create(RenderBindingContext&) noexcept;
        ~DirectLightBinding() noexcept;
        static std::span<const RenderInputSelection> resourceInputs() noexcept;

        [[nodiscard]] render::CapabilityInput lightInput() const noexcept
        {
            return input_;
        }
        [[nodiscard]] SceneStageResult maintain(SceneStageContext&) noexcept;

        template <class C>
        void sourceChanged(
            simulation::ecs::Registry& registry,
            simulation::ecs::Entity entity,
            ERenderComponentChange change
        ) noexcept
        {
            using namespace simulation::ecs;
            if constexpr (std::same_as<C, WorldTransform3D>)
            {
                if (change == ERenderComponentChange::UPDATE ||
                    !registry.any_of<Light3D, detail::LightRenderResourceComponent>(entity))
                {
                    return;
                }
            }
            if (change == ERenderComponentChange::UPDATE)
            {
                if (demands_.contains(entity))
                {
                    return; // A pending creation adopts the latest parameters without starving on animation.
                }
                const auto* resource = registry.try_get<detail::LightRenderResourceComponent>(entity);
                const auto& source = registry.get<Light3D>(entity).value;
                if (resource && resource->type == source.type && resource->shadows == detail::lightShadowInputs(source))
                {
                    return; // Runtime parameter edits do not rebuild the cold recipe.
                }
            }
            if (next_demand_ == UINT64_MAX)
            {
                std::terminate();
            }
            auto& demand = demands_[entity];
            demand.serial = ++next_demand_;
            demand.revoked |= change == ERenderComponentChange::DESTROY;
        }

        template <class C>
        void resultDestroyed(simulation::ecs::Registry& registry, simulation::ecs::Entity entity) noexcept
        {
            static_assert(std::same_as<C, detail::LightRenderResourceComponent>);
            const auto& resource = registry.get<C>(entity);
            ++revision_;
            retired_.push_back({resource.light, revision_});
        }

    private:
        struct Demand final
        {
            std::uint64_t serial = 0;
            bool revoked = false;
        };

        struct Creation final
        {
            simulation::ecs::Entity entity;
            std::uint64_t demand;
            rdesc::ELightType type;
        };

        struct Retirement final
        {
            render::LightId light;
            std::uint64_t after_revision;
        };

        struct Work final
        {
            std::vector<Creation> creations;
            std::vector<render::VDirectLight> values;
            std::vector<render::LightId> retired;
            std::vector<render::LightId> created;
            std::size_t released = 0;
            render::RenderError error;
        };

        DirectLightBinding(RenderBindingContext&, render::CapabilityInput, engine::render::BoundCommand<render::CreateDirectLights>, engine::render::BoundCommand<render::RemoveDirectLight>) noexcept;
        exec::task<render::RenderResult<void>> sendWork() noexcept;
        void startWork() noexcept;
        void adoptWork(bool closing) noexcept;

        simulation::ecs::Registry& registry_;
        engine::render::RenderRequester& client_;
        execution::ExecutionRuntime& execution_;
        render::CapabilityInput input_;
        engine::render::BoundCommand<render::CreateDirectLights> create_;
        engine::render::BoundCommand<render::RemoveDirectLight> remove_;
        std::unordered_map<simulation::ecs::Entity, Demand> demands_;
        std::vector<Retirement> retired_;
        std::optional<Work> work_;
        std::uint64_t next_demand_ = 0;
        std::uint64_t& revision_;
        std::uint64_t& adopted_revision_;
        render::RenderError failure_;
        bool pending_ = false;
    };
} // namespace lux::scene
