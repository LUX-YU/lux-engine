#pragma once
#include <lux/engine/function/render/features/resources/ResourceHandles.hpp>
#include <atomic>
#include <lux/engine/editor/rendering/EditorRenderer.hpp>
#include <lux/engine/editor/scene/SceneResources.hpp>
#include <lux/engine/editor/scene/detail/SceneAssetTasks.hpp>
#include <lux/engine/function/render/features/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/features/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/scene/ResolvedMeshResources.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>
#include <lux/engine/simulation/ecs/WorldEntityMap.hpp>
#include <unordered_map>

namespace lux::editor::scene::detail
{
    struct ResourceRequest final
    {
        explicit ResourceRequest(ResourceRequestKey key)
            : row{std::move(key)}, mesh_read(std::make_shared<AssetResult<lux::asset::MeshAsset>>()),
              material_read(std::make_shared<AssetResult<lux::asset::MaterialAsset>>())
        {
        }

        SceneResourceRow row;
        std::shared_ptr<AssetResult<lux::asset::MeshAsset>> mesh_read;
        std::shared_ptr<AssetResult<lux::asset::MaterialAsset>> material_read;
        lux::render::RenderRequest<lux::render::MeshUploadedReply> mesh_request;
        lux::render::RenderRequest<lux::render::MaterialUploadedReply> material_request;
        lux::render::RenderRequest<lux::render::ShaderCompiledReply> forward_request, gbuffer_request;
        lux::render::RMeshHandle mesh;
        lux::render::RMaterialHandle material;
        lux::render::ShaderHandle forward, gbuffer;

        struct TextureDependency final
        {
            lux::asset::AssetId asset;
            std::shared_ptr<AssetResult<lux::asset::TextureAsset>> read;
            lux::render::RenderRequest<lux::render::Texture2DCreatedReply> upload;
            lux::render::RTextureHandle handle;
        };

        std::vector<TextureDependency> textures; // At most MaterialDescription::kMaxTextures unique dependencies.
        bool texture_reads_started{};
        std::uint64_t refresh_sequence{};
        bool adopted{};
        std::size_t run_pins{};
        std::shared_ptr<std::atomic<bool>> retired_program_consumed;
        void start(ResourceTasks &, lux::process::asset_loading::AssetReadPort) noexcept;
        void acceptReplies() noexcept;
        bool settled() const noexcept;
        std::size_t liveHandles() const noexcept;
        void prepareStep(rendering::EditorRenderer &, lux::scene::RenderRuntimeLease &, ResourceTasks &,
                         lux::process::asset_loading::AssetReadPort);
        void releaseStep(rendering::EditorRenderer &, lux::scene::RenderRuntimeLease &);
    };

    struct FrozenSceneResource final
    {
        lux::world::WorldObjectId object;
        lux::scene::ResolvedMeshResources value;
        lux::scene::QueryResult<std::shared_ptr<const lux::scene::MeshQueryGeometry>> geometry;
    };

    // Main owns the pins; the worker only receives copies of values(). Resource
    // retirement still obeys the existing packet/GPU lifetime after the last pin.
    class SceneResourcePins final
    {
      public:
        SceneResourcePins() = default;
        ~SceneResourcePins();
        SceneResourcePins(SceneResourcePins &&) noexcept;
        SceneResourcePins &operator=(SceneResourcePins &&) noexcept;
        SceneResourcePins(const SceneResourcePins &) = delete;
        SceneResourcePins &operator=(const SceneResourcePins &) = delete;
        [[nodiscard]] std::span<const FrozenSceneResource> values() const noexcept
        {
            return values_;
        }

      private:
        friend class SceneResources;
        void reset() noexcept;
        std::vector<ResourceRequest *> requests_;
        std::vector<FrozenSceneResource> values_;
    };

    class SceneResources final
    {
      public:
        SceneResources(editing::HistoryId, SceneInstanceId, lux::process::asset_loading::AssetReadPort, rendering::EditorRenderer *,
                       std::size_t);
        ~SceneResources() noexcept;
        SceneResult<void> activate() noexcept;
        SceneResult<bool> prepareUpdate(lux::simulation::ecs::Registry &) noexcept;
        void synchronizeQuery(lux::scene::MeshQuerySystem &);
        [[nodiscard]] SceneResult<SceneResourcePins> freeze(const lux::simulation::ecs::Registry &,
                                                            const lux::simulation::ecs::WorldEntityMap &);
        void acknowledgeSnapshot() noexcept;

        bool snapshotChanged() const noexcept
        {
            return pending_change_;
        }

        bool hasPendingWork() const noexcept;
        void afterPresentation(bool source_update_pending) noexcept;
        SceneResult<void> retry(const ResourceRequestKey &) noexcept;
        SceneResult<std::size_t> refreshAssets(std::span<const lux::asset::AssetId>) noexcept;
        SceneResult<std::shared_ptr<const SceneResourceSnapshot>> snapshot(std::uint64_t) const noexcept;
        SceneResult<std::shared_ptr<const SceneCloseSnapshot>> closeSnapshot(ECloseState, std::size_t views,
                                                                             bool scene_present) const noexcept;
        SceneResult<void> beginClose() noexcept;
        SceneResult<bool> advanceClose() noexcept;

      private:
        editing::HistoryId history_;
        SceneInstanceId instance_;
        lux::process::asset_loading::AssetReadPort port_;
        rendering::EditorRenderer *renderer_{};
        lux::scene::RenderRuntimeLease runtime_;
        std::vector<std::unique_ptr<ResourceRequest>> requests_;
        // Non-owning current association. Full Entity includes its generation; the pointed-to key owns sources.
        // Superseded requests remain exclusively owned by requests_ until their real retirement completes.
        std::unordered_map<lux::simulation::ecs::Entity, ResourceRequest *> current_requests_;
        std::unordered_map<lux::asset::AssetId, std::weak_ptr<AssetResult<lux::asset::MeshAsset>>> mesh_reads_;
        // Retained query associations, marked once per current source during adoption.
        std::unordered_map<lux::asset::AssetId, bool> query_sources_;
        void shareMeshRead(ResourceRequest &);
        std::size_t capacity_{};
        std::size_t refresh_reservations_{};
        std::uint64_t sequence_{};
        bool active_{}, closing_{}, closed_{};
        bool pending_change_{true};
        ResourceTasks tasks_;
        SceneResult<void> prepareRetirement(bool all) noexcept;
        lux::render::RenderProgram<> retirement_program_, retirement_progress_;
        bool retirement_pending_{};
    };
} // namespace lux::editor::scene::detail
