#pragma once

#include <lux/engine/render/capabilities/Mesh3dCapability.hpp>
#include <lux/engine/systems/render/RenderReaction.hpp>
#include <lux/engine/systems/render/material/MaterialUploadDefinition.hpp>
#include <lux/engine/systems/render/mesh/MeshUploadDefinition.hpp>
#include <unordered_map>
#include <unordered_set>

namespace lux::engine::render
{
    struct MeshRenderResourceComponent;

    class LUX_ENGINE_SYSTEM_RENDER_PUBLIC MeshInstanceReaction final : public RenderReaction
    {
    public:
        using PreparationFailure = std::pair<ecs::Entity, error::Error>;
        object::TSignal<PreparationFailure> preparationFailed{*this};
        static constexpr auto capability = cxx::make_basic_type_info<lux::render::Mesh3dCapability>();
        using CreateResult = lux::render::RenderResult<std::unique_ptr<MeshInstanceReaction>>;
        [[nodiscard]] static CreateResult create(
            RenderSystem& system,
            lux::render::CapabilityImplementationId implementation,
            MeshUpload& meshes,
            MaterialUpload& materials,
            lux::render::CommandId create_instances,
            lux::render::CommandId release_instance
        ) noexcept;
        ~MeshInstanceReaction() noexcept override;

    private:
        MeshInstanceReaction(
            RenderSystem& system,
            lux::render::CapabilityImplementationId implementation,
            MeshUpload& meshes,
            MaterialUpload& materials,
            lux::render::CommandId create_instances,
            lux::render::CommandId release_instance
        ) noexcept;
        [[nodiscard]] Result refreshAssets(ecs::Entity entity) noexcept;
        void forgetAssets(ecs::Entity entity) noexcept;
        void onAssetChanged(asset::AssetId asset) noexcept;
        void inputChanged(ecs::Registry&, ecs::Entity entity) noexcept;
        void removeResources(ecs::Registry&, ecs::Entity entity) noexcept;
        void reportFailure(ecs::Entity entity, error::Error error) noexcept override;
        [[nodiscard]] MeshRenderResourceComponent takeResources(ecs::Entity entity) noexcept;
        [[nodiscard]] exec::task<Result> reconcile(ecs::Entity entity) noexcept override;
        [[nodiscard]] exec::task<Result> drainRemoved(ecs::Entity entity) noexcept;
        [[nodiscard]] exec::task<Result> discard(ecs::Entity entity, MeshRenderResourceComponent resources) noexcept;
        [[nodiscard]] exec::task<Result> releaseMeshResources(MeshRenderResourceComponent& resources) noexcept;
        [[nodiscard]] exec::task<Result> releaseResources() noexcept override;

        using AssetObserver = std::pair<
            assets::AssetRef<asset::Asset>, std::pair<entt::scoped_connection, std::unordered_set<ecs::Entity>>
        >;
        // Subscription edges, not a second asset cache. Disconnect before releasing their AssetRef.
        std::unordered_map<asset::AssetId, AssetObserver> asset_observers_;
        std::unordered_map<ecs::Entity, std::vector<asset::AssetId>> asset_inputs_;
        MeshUpload& meshes_;
        MaterialUpload& materials_;
        lux::render::CapabilityImplementationId implementation_;
        lux::render::CommandId create_instances_id_;
        lux::render::CommandId release_instance_id_;
        std::unordered_multimap<ecs::Entity, MeshRenderResourceComponent> removed_resources_;
    };
} // namespace lux::engine::render
