#pragma once
/**
 * @file ClassicMeshBatchRenderSubsystem.hpp
 * @brief ECS-first Classic Mesh batch presentation leaf.
 */

#include <lux/engine/runtime/render/scene/visibility.h>
#include <lux/engine/runtime/render/scene/SceneContentRenderStatus.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/ecs/render/RenderStage.hpp>
#include <lux/engine/ecs/render/ResidencyCallbacks.hpp>

#include <memory>

namespace lux::asset { class AssetManager; }

namespace lux::runtime
{
    class SceneAsyncContext;
    class ClassicMeshPrepareClient;
    struct ClassicMeshBatchObservedCommand;
    /// One ECS entity owns one immutable Classic Mesh batch. Individual rows
    /// are translated directly into RenderCluster instances and never become
    /// ECS entities.
    class LUX_RUNTIME_RENDER_SCENE_PUBLIC
    ClassicMeshBatchRenderSubsystem final : public lux::ecs::RenderStage
    {
    public:
        ClassicMeshBatchRenderSubsystem(
            entity_scene::ContentBlobClient blobs,
            lux::ecs::ResidencyCallbacks residency,
            lux::asset::AssetManager& assets,
            ClassicMeshPrepareClient preparation,
            SceneAsyncContext& async) noexcept;
        ~ClassicMeshBatchRenderSubsystem() override;

        ClassicMeshBatchRenderSubsystem(
            const ClassicMeshBatchRenderSubsystem&) = delete;
        ClassicMeshBatchRenderSubsystem& operator=(
            const ClassicMeshBatchRenderSubsystem&) = delete;

        void onAdded(const lux::ecs::SystemSetupContext& setup) override;
        void onRemoved(const lux::ecs::SystemRemovalContext& removal) override;
        [[nodiscard]] std::span<const std::string_view>
        requiredFeatures() const noexcept override;
        void prepare(lux::ecs::RenderSubsystemContext& context) noexcept
            override;
        void extract(lux::ecs::RenderSubsystemContext& context) override;
        void close(lux::ecs::RenderSubsystemContext& context) noexcept
            override;
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
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::runtime
