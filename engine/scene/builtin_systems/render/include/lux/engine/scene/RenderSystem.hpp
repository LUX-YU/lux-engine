#pragma once

#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/render/RenderRuntime.hpp>
#include <lux/engine/scene/RenderAssets.hpp>
#include <lux/engine/scene/RenderFeatureSceneBinding.hpp>
#include <lux/engine/scene/RenderSyncStage.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <lux/engine/scene/render/visibility.h>

#include <array>
#include <span>
#include <string_view>

namespace lux::scene
{
class SceneBuilder;
class MeshQuerySystem;
namespace detail
{
class RenderAssets;
}

class LUX_ENGINE_SCENE_RENDER_PUBLIC RenderSystem : public object::LuxObject
{
  public:
    inline static constexpr std::array Capabilities{std::string_view{"lux.scene.render"}};
    inline static constexpr system::SystemTypeDescription Description{
        .canonical_name = "lux.builtin.system.render",
        .version = 1U,
        .configuration_schema_name = "lux.render.system.Configuration",
        .configuration_schema_version = 1U,
        .capabilities = Capabilities,
        .multiplicity = system::ESystemMultiplicity::SINGLE_PER_OWNER};

    ~RenderSystem() noexcept override;
    [[nodiscard]] render::RenderSceneId renderSceneId() const noexcept;
    [[nodiscard]] system::SystemInstanceId instanceId() const noexcept
    {
        return instance_;
    }
    [[nodiscard]] double coordinatePageSize() const noexcept
    {
        return coordinate_page_size_;
    }
    [[nodiscard]] render::SceneResourceStatus resourceStatus() const noexcept;
    [[nodiscard]] render::RenderSceneReceipt resourceReceipt() const noexcept;
    [[nodiscard]] render::RenderSceneLease retainScene() const noexcept
    {
        return lease_.retain();
    }
    [[nodiscard]] render::FeatureHandle feature(render::FeatureTypeId) const noexcept;
    [[nodiscard]] render::RenderResult<std::unique_ptr<render::RenderView>> openView(render::ViewConfig);
    [[nodiscard]] render::RenderResult<void> associateView(render::RenderViewId, SceneInstanceId,
                                                           simulation::ecs::Entity);
    [[nodiscard]] render::RenderResult<void> dissociateView(render::RenderViewId, SceneInstanceId,
                                                            simulation::ecs::Entity) noexcept;
    [[nodiscard]] SceneStageResult maintain() noexcept;
    [[nodiscard]] SceneStageResult maintain(SceneStageContext &) noexcept;
    [[nodiscard]] SceneStageResult publish(SceneStageContext &) noexcept;
    [[nodiscard]] std::span<const RenderAssetStatus> assetStatus() const;
    [[nodiscard]] std::uint64_t assetRevision() const;
    [[nodiscard]] render::RenderResult<void> retryAsset(const RenderAssetKey &);
    [[nodiscard]] render::RenderResult<void> replaceAssetSource(std::shared_ptr<RenderAssetSource>);
    [[nodiscard]] RenderSyncStatistics transportStatistics() const noexcept;

  protected:
    struct Extraction final
    {
        render::FeatureTypeId feature{};
        CreateRenderSyncStageFn create{};
    };

    RenderSystem(system::SystemInstanceId, SceneInstanceId, render::RenderRuntime &, simulation::ecs::Registry &,
                 render::RenderSceneLease, double coordinate_page_size, std::vector<Extraction>,
                 std::shared_ptr<RenderAssetSource> = {}, MeshQuerySystem * = nullptr,
                 object::ObjectDispatcherRef = {});
    [[nodiscard]] const render::RenderSceneLease &sceneLease() const noexcept
    {
        return lease_;
    }
    [[nodiscard]] render::RenderRuntime &runtime() noexcept
    {
        return runtime_;
    }
    [[nodiscard]] SceneStageResult submitProgram(render::RenderProgram<> &, SceneStageContext &) noexcept;

  private:
    friend class SceneBuilder;
    friend lux::cxx::expected<void, SceneSystemBuildFailure> installBuiltinRenderSystem(SceneBuilder &,
                                                                                        SceneSystemDescription) noexcept;

    system::SystemInstanceId instance_;
    struct ViewBinding final
    {
        render::RenderViewId id;
        simulation::ecs::Entity camera;
    };
    std::vector<ViewBinding> view_bindings_;
    RenderViewAssociations view_associations_;
    void refreshViews();
    render::RenderRuntime &runtime_;
    simulation::ecs::Registry &registry_;
    const double coordinate_page_size_;
    // The lease retains the extraction code through stage destruction.
    render::RenderSceneLease lease_;
    std::unique_ptr<detail::RenderAssets> assets_;
    MeshQuerySystem *query_{};
    std::vector<Extraction> factories_;
    std::vector<std::unique_ptr<RenderSyncStage>> stages_;
    render::RenderProgram<> pending_;
    RenderSyncStatistics statistics_;
    std::chrono::nanoseconds captured_elapsed_{}, captured_delta_{};
    std::uint64_t captured_step_{};
    SceneStageResult result_{ESceneProgress::COMPLETE};
    bool initialized_{}, prepared_{}, full_sync_{true};
};

[[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC SceneSystemRegistration builtinRenderSystemRegistration() noexcept;
[[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC std::span<const SceneSystemRegistration>
builtinRenderSystemRegistrations() noexcept;
LUX_ENGINE_SCENE_RENDER_PUBLIC void initializeBuiltinRenderSystemMeta() noexcept;
} // namespace lux::scene
