#pragma once

#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/systems/render/visibility.h>

namespace lux::engine { class EngineContext; }
namespace lux::world { class World; }

namespace lux::engine::render
{
    // Produces the VisibilityComponent range result, independent of RenderSystem/remote Views.
    // Assemble before RenderSystem in AFTER_SIMULATION. Construction never starts asset tasks.
    class LUX_ENGINE_SYSTEM_RENDER_PUBLIC MeshVisibilitySystem final : public scene::SceneSystem
    {
    public:
        using CreateResult = cxx::expected<std::unique_ptr<MeshVisibilitySystem>, error::Error>;

        [[nodiscard]] static CreateResult
        create(
            world::World& world,
            EngineContext& context,
            std::uint32_t geometry,
            double prepare_distance,
            double retain_distance,
            double cache_margin,
            double coverage_margin
        ) noexcept;
        ~MeshVisibilitySystem() noexcept override;
        [[nodiscard]] Result execute() noexcept override;
        [[nodiscard]] scene::ESceneSystemStage stage() const noexcept override;

    private:
        struct Impl;
        explicit MeshVisibilitySystem(std::unique_ptr<Impl> impl) noexcept;
        [[nodiscard]] exec::task<Result> prepareBounds(ecs::Entity entity, asset::AssetId mesh) noexcept;
        void collectBoundsChanges() noexcept;
        [[nodiscard]] Result updateSpatialIndex() noexcept;
        [[nodiscard]] Result updateViewRanges() noexcept;
        void updateRelations() noexcept;
        void publishChanges() noexcept;
        void onAssetChanged(asset::AssetId asset) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}
