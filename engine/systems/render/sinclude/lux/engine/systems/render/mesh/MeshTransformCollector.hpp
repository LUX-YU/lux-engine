#pragma once

#include <entt/container/dense_set.hpp>
#include <entt/signal/sigh.hpp>
#include <array>
#include <lux/engine/render/capabilities/Mesh3dCapability.hpp>
#include <lux/engine/systems/render/RenderDataCollector.hpp>
#include <lux/engine/systems/render/RenderSystem.hpp>
#include <lux/engine/systems/render/visibility.h>

namespace lux::engine::assets { class AssetRepository; }

namespace lux::engine::render
{
    class LUX_ENGINE_SYSTEM_RENDER_PUBLIC MeshTransformCollector final : public RenderDataCollector
    {
    public:
        static constexpr auto capability = cxx::make_basic_type_info<lux::render::Mesh3dCapability>();
        using CreateResult = lux::render::RenderResult<std::unique_ptr<MeshTransformCollector>>;

        [[nodiscard]] static CreateResult
        create(RenderSystem& system, lux::render::FrameDataDestination destination) noexcept;
        [[nodiscard]] lux::render::RenderResult<void> collect(lux::render::FrameWriter& frame) noexcept override;
        void invalidate() noexcept override;

    private:
        MeshTransformCollector(
            RenderSystem& system, lux::render::FrameDataDestination destination
        ) noexcept;

        void transformChanged(ecs::Registry& registry, ecs::Entity entity) noexcept;
        void propertiesChanged(ecs::Registry& registry, ecs::Entity entity) noexcept;
        void resourcesRemoved(ecs::Registry& registry, ecs::Entity entity) noexcept;

        ecs::Registry& registry_;
        assets::AssetRepository& assets_;
        lux::render::FrameDataBinding<lux::render::TransformData> output_;
        lux::render::FrameDataBinding<lux::render::MeshInstanceUpdates> properties_output_;
        entt::dense_set<ecs::Entity> pending_;
        entt::dense_set<ecs::Entity> pending_properties_;
        bool resynchronize_ = false;
        std::array<entt::scoped_connection, 18> connections_;
    };
}
