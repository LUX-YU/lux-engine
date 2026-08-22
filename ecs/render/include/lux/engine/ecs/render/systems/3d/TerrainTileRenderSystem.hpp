#pragma once
/**
 * @file TerrainTileRenderSystem.hpp
 * @brief ECS-first terrain tile presentation system.
 */

#include <lux/engine/function/visibility.h>
#include <lux/engine/ecs/render/SceneContentRenderStatus.hpp>
#include <lux/engine/ecs/entity_scene/ContentBlobStorage.hpp>
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/ecs/render/SceneGeometryPreparation.hpp>

#include <memory>

namespace lux::ecs
{
    struct TerrainTileObservedCommand;
    class LUX_FUNCTION_PUBLIC
    TerrainTileRenderSystem final : public ISystem
    {
    public:
        explicit TerrainTileRenderSystem(
            lux::ecs::SceneRenderBinding& render,
            lux::ecs::entity_scene::ContentBlobClient blobs,
            lux::ecs::TerrainPreparePort preparation) noexcept;
        ~TerrainTileRenderSystem() override;

        TerrainTileRenderSystem(const TerrainTileRenderSystem&) = delete;
        TerrainTileRenderSystem& operator=(
            const TerrainTileRenderSystem&) = delete;

        void onAdded(const lux::ecs::SystemSetupContext& setup) override;
        void onRemoved(const lux::ecs::SystemRemovalContext& removal) override;
        [[nodiscard]] static std::span<const std::string_view>
        requiredRenderFeatures() noexcept;
        void update(const lux::ecs::SystemUpdateContext& context) override;
        void requestClose() noexcept override;
        [[nodiscard]] std::span<const Type> runsAfter() const noexcept override;
        [[nodiscard]] SceneContentRenderEntrySnapshot status(
            lux::ecs::Entity entity) const noexcept;
        [[nodiscard]] SceneContentRenderSubsystemSnapshot snapshot()
            const noexcept;

    private:
        friend struct TerrainTileObservedCommand;
        void applyObservedChange(
            lux::ecs::Entity entity,
            bool topology) noexcept;
        struct Impl;
        lux::ecs::SceneRenderBinding* render_{nullptr};
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::ecs
