#pragma once
/**
 * @file TerrainTileRenderSubsystem.hpp
 * @brief ECS-first terrain tile presentation leaf.
 */

#include <lux/engine/runtime/render/scene/visibility.h>
#include <lux/engine/runtime/render/scene/SceneContentRenderStatus.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/ecs/render/IRenderSubsystem.hpp>

#include <memory>

namespace lux::runtime
{
    class SceneAsyncContext;
    class TerrainPrepareClient;
    struct TerrainTileObservedCommand;
    class LUX_RUNTIME_RENDER_SCENE_PUBLIC
    TerrainTileRenderSubsystem final : public lux::ecs::IRenderSubsystem
    {
    public:
        explicit TerrainTileRenderSubsystem(
            entity_scene::ContentBlobClient blobs,
            TerrainPrepareClient preparation,
            SceneAsyncContext& async) noexcept;
        ~TerrainTileRenderSubsystem() override;

        TerrainTileRenderSubsystem(const TerrainTileRenderSubsystem&) = delete;
        TerrainTileRenderSubsystem& operator=(
            const TerrainTileRenderSubsystem&) = delete;

        void onAdded(const lux::ecs::SystemSetupContext& setup) override;
        void onRemoved(const lux::ecs::SystemRemovalContext& removal) override;
        [[nodiscard]] std::span<const std::string_view>
        renderFeatures() const noexcept override;
        void prepare(lux::ecs::RenderSubsystemContext& context) noexcept
            override;
        void update(lux::ecs::RenderSubsystemContext& context) override;
        void close(lux::ecs::RenderSubsystemContext& context) noexcept
            override;
        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }

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
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::runtime
