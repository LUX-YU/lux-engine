#pragma once

#include <lux/engine/scene/MeshQuery.hpp>
#include <lux/engine/scene/RenderResources.hpp>
#include <lux/engine/scene/ResolvedMeshResources.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/simulation/ecs/ComponentChangeSet.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <unordered_map>

namespace lux::scene
{
    class RenderSystem;
    class LUX_ENGINE_SCENE_RENDER_PUBLIC RenderAssets final
    {
    public:
        ~RenderAssets();
        [[nodiscard]] static RenderAssets* find(simulation::ecs::Registry&, system::SystemInstanceId) noexcept;
        [[nodiscard]] static const RenderAssets* find(
            const simulation::ecs::Registry&,
            system::SystemInstanceId
        ) noexcept;
        [[nodiscard]] render::RenderResult<void> replaceInput(RenderAssetInput) noexcept;
        [[nodiscard]] std::span<const RenderAssetStatus> statuses() const noexcept;
        [[nodiscard]] std::uint64_t revision() const noexcept
        {
            static_cast<void>(statuses());
            return revision_;
        }
        [[nodiscard]] render::RenderResult<void> retry(const RenderAssetKey&) noexcept;

    private:
        friend class RenderSystem;
        static RenderAssets& install(
            simulation::ecs::Registry&,
            system::SystemInstanceId,
            SceneInstanceId,
            RenderResources&,
            RenderAssetInput
        );
        static void uninstall(simulation::ecs::Registry&, system::SystemInstanceId) noexcept;
        RenderAssets(simulation::ecs::Registry&, SceneInstanceId, RenderResources&, RenderAssetInput);
        void maintain(SceneStageContext&);
        void synchronizeQuery(MeshQuery&);
        [[nodiscard]] bool pending() const noexcept;
        struct Association final
        {
            RenderAssetKey key;
            RenderResourceId mesh, material;
            RenderAssetStatus row;
            render::RenderSubmissionState submission;
            bool fresh{};
            bool geometry_observed{};
        };
        simulation::ecs::Registry& registry_;
        SceneInstanceId instance_;
        RenderResources& resources_;
        RenderAssetInput input_;
        std::optional<RenderAssetInput> replacement_;
        std::unordered_map<simulation::ecs::Entity, Association> current_;
        mutable std::vector<RenderAssetStatus> snapshot_;
        std::unordered_map<asset::AssetId, const void*> query_sources_;
        std::uint64_t sequence_{};
        mutable bool changed_{true};
        mutable std::uint64_t revision_{};
        bool first_{true}, query_dirty_{true};
        simulation::ecs::TExtractionChangeSet<
            simulation::ecs::Mesh3D,
            simulation::ecs::TComponentList<>,
            simulation::ecs::TComponentList<>>
            changes_;
        entt::scoped_connection destroyed_;
        std::vector<simulation::ecs::Entity> pending_, departures_;
        void departed(simulation::ecs::Registry&, simulation::ecs::Entity);
        void release(Association&) noexcept;
        void associate(simulation::ecs::Entity, bool fresh = false);
    };
} // namespace lux::scene
