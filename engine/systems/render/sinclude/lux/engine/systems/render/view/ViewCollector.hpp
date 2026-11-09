#pragma once

#include <entt/entity/storage.hpp>
#include <entt/signal/sigh.hpp>
#include <lux/engine/world/World.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/scene/ViewComponent.hpp>
#include <lux/engine/render/inputs/CameraInputs.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/systems/render/visibility.h>
#include <lux/engine/systems/render/RenderDataCollector.hpp>
#include <lux/engine/systems/render/RenderSystem.hpp>
#include <lux/engine/systems/render/view/RenderViewComponent.hpp>
#include <array>
#include <memory>
#include <tuple>

namespace lux::engine::render
{
    [[nodiscard]] LUX_ENGINE_SYSTEM_RENDER_PUBLIC lux::render::RenderResult<lux::render::CameraInput>
    makeViewParameters(const scene::ViewComponent& view, const ecs::Transform3D& transform,
        const lux::render::TargetDesc& target) noexcept;

    [[nodiscard]] LUX_ENGINE_SYSTEM_RENDER_PUBLIC lux::render::RenderResult<void> writeViewParameters(
        ecs::Registry& registry, ecs::Entity entity, lux::render::FrameDataBinding<lux::render::CameraInput> binding,
        lux::render::FrameWriter& frame
    ) noexcept;

    // Each selected capability writes its own declared camera entry. No implicit broadcast or resource creation.
    template <class Capability> class ViewCollector final : public RenderDataCollector
    {
    public:
        static constexpr auto capability = cxx::make_basic_type_info<Capability>();
        using CreateResult = lux::render::RenderResult<std::unique_ptr<ViewCollector>>;

        [[nodiscard]] static CreateResult
        create(RenderSystem& system, lux::render::FrameDataDestination destination) noexcept
        {
            return std::unique_ptr<ViewCollector>{new ViewCollector{
                system.world().registry(), destination
            }};
        }

        void invalidate() noexcept override { resynchronize_ = true; }

        lux::render::RenderResult<void> collect(lux::render::FrameWriter& frame) noexcept override
        {
            if (resynchronize_)
            {
                for (auto entity : registry_.view<scene::ViewComponent, ecs::Transform3D, RenderViewComponent>())
                {
                    onViewChanged(registry_, entity);
                }
            }
            // Engine never dispatches completions or mutates Registry during Collector traversal.
            // Keep the batch on a write failure; successful frame rejection uses invalidate().
            for (const auto entity : dirty_views_)
            {
                auto written = writeViewParameters(registry_, entity, output_, frame);
                if (!written)
                {
                    return written;
                }
            }
            dirty_views_.clear();
            resynchronize_ = false;
            return {};
        }

    private:
        ViewCollector(ecs::Registry& registry, const lux::render::FrameDataDestination& destination) noexcept
            : registry_(registry),
              output_(lux::render::frameDataBinding<Capability, lux::render::CameraInput>(
                  destination.effect, destination.capability, destination.scope
              ))
        {
            connections_[0] = registry.on_construct<scene::ViewComponent>()
                .template connect<&ViewCollector::onViewChanged>(*this);
            connections_[1] = registry.on_update<scene::ViewComponent>()
                .template connect<&ViewCollector::onViewChanged>(*this);
            connections_[2] = registry.on_destroy<scene::ViewComponent>()
                .template connect<&ViewCollector::onViewRemoved>(*this);
            connections_[3] = registry.on_construct<ecs::Transform3D>()
                .template connect<&ViewCollector::onViewChanged>(*this);
            connections_[4] = registry.on_update<ecs::Transform3D>()
                .template connect<&ViewCollector::onViewChanged>(*this);
            connections_[5] = registry.on_destroy<ecs::Transform3D>()
                .template connect<&ViewCollector::onViewRemoved>(*this);
            connections_[6] = registry.on_construct<RenderViewComponent>()
                .template connect<&ViewCollector::onViewChanged>(*this);
            connections_[7] = registry.on_update<RenderViewComponent>()
                .template connect<&ViewCollector::onViewChanged>(*this);
            connections_[8] = registry.on_destroy<RenderViewComponent>()
                .template connect<&ViewCollector::onViewRemoved>(*this);
            for (auto entity : registry.view<scene::ViewComponent, ecs::Transform3D, RenderViewComponent>())
            {
                onViewChanged(registry, entity);
            }
        }

        void onViewChanged(ecs::Registry& registry, ecs::Entity entity) noexcept
        {
            const bool is_view = registry.all_of<scene::ViewComponent, ecs::Transform3D, RenderViewComponent>(entity);
            if (is_view && !dirty_views_.contains(entity)) dirty_views_.emplace(entity);
        }

        void onViewRemoved(ecs::Registry&, ecs::Entity entity) noexcept
        {
            dirty_views_.remove(entity);
        }

        using DirtyViews = entt::basic_storage<void, ecs::Entity>;

        ecs::Registry& registry_;
        lux::render::FrameDataBinding<lux::render::CameraInput> output_;
        DirtyViews dirty_views_;
        bool resynchronize_ = false;
        // Disconnect before the dirty storage and Registry borrow are destroyed.
        std::array<entt::scoped_connection, 9> connections_;
    };
}
