#pragma once
#include <lux/engine/render/capabilities/LitScene.hpp>
#include <lux/engine/scene/render/RenderComponentBinding.hpp>
#include <lux/engine/scene/render/visibility.h>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

namespace lux::scene
{
    namespace detail
    {
        struct MeshRenderResourceComponent;
        struct MaterialRenderResourceComponent;
        struct MeshInstanceRenderResourceComponent;
    } // namespace detail

    class MeshRenderBinding final
    {
    public:
        using SourceComponents = std::tuple<simulation::ecs::Mesh3D, simulation::ecs::WorldTransform3D>;
        using ResultComponents = std::tuple<
            detail::MeshRenderResourceComponent,
            detail::MaterialRenderResourceComponent,
            detail::MeshInstanceRenderResourceComponent>;
        static constexpr std::string_view name = "lux.scene.binding.raster_mesh";
        static constexpr system::SystemDataContract data_inputs[]{
            {"lux.ecs.Transform3D"},
            {"lux.transform.world3d.ready", 1, system::ESystemDataKind::PREPARED_DATA, system::ESystemDataPhase::STABLE}
        };
        static std::span<const render::RenderPayloadContract> inputs() noexcept;
        static exec::task<render::RenderResult<void>> prepareResources(engine::render::RenderAssets&) noexcept;
        static render::RenderResult<std::unique_ptr<MeshRenderBinding>> create(RenderBindingContext&) noexcept;
        ~MeshRenderBinding() noexcept;
        SceneStageResult maintain(SceneStageContext&) noexcept;
        [[nodiscard]] render::RenderError assetError(simulation::ecs::Entity) const noexcept;
        [[nodiscard]] bool visualReady(simulation::ecs::Entity) const noexcept;
        // Cold recipe input. Includes qualified replacements until all views have adopted their union.
        [[nodiscard]] std::vector<render::RasterGeometryInput> geometry() const noexcept;
        [[nodiscard]] std::uint32_t instanceCapacity() const noexcept;

        template <class C>
        void sourceChanged(
            simulation::ecs::Registry& registry,
            simulation::ecs::Entity entity,
            ERenderComponentChange change
        ) noexcept
        {
            if constexpr (std::same_as<C, simulation::ecs::WorldTransform3D>)
            {
                if (change == ERenderComponentChange::UPDATE || !registry.any_of<simulation::ecs::Mesh3D>(entity))
                {
                    return;
                }
            }
            sourceChanged(entity);
        }

        template <class C> void resultDestroyed(simulation::ecs::Registry&, simulation::ecs::Entity entity) noexcept
        {
            if constexpr (std::same_as<C, detail::MeshRenderResourceComponent>)
            {
                meshDestroyed(entity);
            }
            else if constexpr (std::same_as<C, detail::MaterialRenderResourceComponent>)
            {
                materialDestroyed(entity);
            }
            else
            {
                instanceDestroyed(entity);
            }
        }

    private:
        struct Impl;
        explicit MeshRenderBinding(std::unique_ptr<Impl>) noexcept;
        void sourceChanged(simulation::ecs::Entity) noexcept;
        void meshDestroyed(simulation::ecs::Entity) noexcept;
        void materialDestroyed(simulation::ecs::Entity) noexcept;
        void instanceDestroyed(simulation::ecs::Entity) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::scene
