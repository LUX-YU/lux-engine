#pragma once
/**
 * @file ClassicMeshBatchRenderSystem.hpp
 * @brief ECS-first Classic Mesh batch presentation system.
 */

#include <lux/engine/function/visibility.h>
#include <lux/engine/ecs/render/SceneContentRenderStatus.hpp>
#include <lux/engine/ecs/entity_scene/ContentBlobStorage.hpp>
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/ecs/render/SceneGeometryPreparation.hpp>
#include <lux/engine/ecs/render/ResidencyCallbacks.hpp>

#include <memory>

namespace lux::asset { class AssetManager; }

namespace lux::ecs
{
    struct ClassicMeshBatchObservedCommand;
    /// One ECS entity owns one immutable Classic Mesh batch. Individual rows
    /// are translated directly into RenderCluster instances and never become
    /// ECS entities.
    class LUX_FUNCTION_PUBLIC
    ClassicMeshBatchRenderSystem final : public ISystem
    {
    public:
        ClassicMeshBatchRenderSystem(
            lux::ecs::SceneRenderBinding& render,
            lux::ecs::entity_scene::ContentBlobClient blobs,
            lux::ecs::ResidencyCallbacks residency,
            lux::asset::AssetManager& assets,
            lux::ecs::ClassicMeshPreparePort preparation) noexcept;
        ~ClassicMeshBatchRenderSystem() override;

        ClassicMeshBatchRenderSystem(
            const ClassicMeshBatchRenderSystem&) = delete;
        ClassicMeshBatchRenderSystem& operator=(
            const ClassicMeshBatchRenderSystem&) = delete;

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
        friend struct ClassicMeshBatchObservedCommand;
        void applyObservedChange(
            lux::ecs::Entity entity,
            bool topology) noexcept;
        struct Impl;
        lux::ecs::SceneRenderBinding* render_{nullptr};
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::ecs
