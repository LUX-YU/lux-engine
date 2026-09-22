#pragma once

#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/scene/RenderAssets.hpp>
#include <lux/engine/scene/ResolvedMeshResources.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/simulation/ecs/ComponentChangeSet.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <unordered_map>

namespace lux::scene::detail
{
struct AssetRequest;
struct AssetUse;
struct SceneAssetLifetime;

class RenderAssets final
{
  public:
    RenderAssets(simulation::ecs::Registry &, SceneInstanceId, render::RenderSceneReceipt,
                 std::shared_ptr<RenderAssetSource>);
    ~RenderAssets();
    void maintain(SceneStageContext &);
    void replaceSource(std::shared_ptr<RenderAssetSource>);
    [[nodiscard]] std::span<const RenderAssetStatus> statuses() const;
    [[nodiscard]] std::uint64_t revision() const
    {
        static_cast<void>(statuses());
        return revision_;
    }
    [[nodiscard]] render::RenderResult<void> retry(const RenderAssetKey &);
    void synchronizeQuery(MeshQuerySystem &);
    [[nodiscard]] bool pending() const noexcept;

  private:
    struct Association final
    {
        RenderAssetKey key;
        std::shared_ptr<AssetUse> use;
        std::shared_ptr<const void> lifetime;
        bool fresh{};
        bool attempted{};
        bool adopted{};
        bool geometry_observed{};
    };
    simulation::ecs::Registry &registry_;
    SceneInstanceId instance_;
    std::shared_ptr<RenderAssetSource> source_;
    std::shared_ptr<RenderAssetSource> replacement_;
    std::shared_ptr<SceneAssetLifetime> lifetime_;
    std::unordered_map<simulation::ecs::Entity, Association> current_;
    mutable std::vector<RenderAssetStatus> snapshot_;
    std::unordered_map<asset::AssetId, const void *> query_sources_;
    std::uint64_t sequence_{};
    mutable bool changed_{true};
    mutable std::uint64_t revision_{};
    bool first_{true}, query_dirty_{true};
    simulation::ecs::ExtractionChangeSet<simulation::ecs::Mesh3D, simulation::ecs::ComponentList<>,
                                         simulation::ecs::ComponentList<>>
        changes_;
    entt::scoped_connection destroyed_;
    std::vector<simulation::ecs::Entity> pending_, departures_;
    void departed(simulation::ecs::Registry &, simulation::ecs::Entity);
    void associate(simulation::ecs::Entity, bool fresh = false);
    std::size_t cursor_{};
};
} // namespace lux::scene::detail
