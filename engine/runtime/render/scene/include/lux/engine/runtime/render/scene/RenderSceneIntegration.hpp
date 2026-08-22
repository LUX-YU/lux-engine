#pragma once
/**
 * @file RenderSceneIntegration.hpp
 * @brief Optional render-domain activation for a render-blind SceneRuntime.
 */

#include <lux/engine/runtime/render/scene/visibility.h>
#include <lux/engine/runtime/render/scene/StandardFeaturePlan.hpp>
#include <lux/engine/ecs/render/presentation/PrimaryViewPresentation.hpp>
#include <lux/engine/runtime/scene/SceneRuntime.hpp>
#include <lux/engine/function/render/client/FeatureCatalog.hpp>
#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>
#include <lux/engine/math/Extent.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace lux::runtime { class ResidencyAssembly; }
namespace lux::render
{
    class RenderControlSession;
    class RenderFrameSession;
}

namespace lux::runtime
{
    struct RenderSceneServices
    {
        lux::render::RenderFrameSession&               frame;
        lux::render::RenderControlSession&             control;
        lux::render::RenderUploadClient                upload;
        lux::render::FeatureCatalog&                   feature_catalog;
        const std::vector<lux::render::FeatureAttach>& feature_plan;
        ResidencyAssembly&                             residency;
        const RenderProfile&                           profile;
    };

    struct RenderSceneConfig
    {
        lux::render::RenderTargetId target{};
        lux::math::Extent2u extent{};
        /// Game/Play presentation opts in. Editor and thumbnail scaffolding
        /// keep their explicitly authored runtime-only ViewPresentComponent.
        bool present_primary_camera{false};
    };

    class LUX_RUNTIME_RENDER_SCENE_PUBLIC RenderSceneIntegration final
        : public ISceneRuntimeIntegration
    {
    public:
        RenderSceneIntegration(
            RenderSceneServices& services,
            RenderSceneConfig config) noexcept;
        ~RenderSceneIntegration() override;

        RenderSceneIntegration(const RenderSceneIntegration&) = delete;
        RenderSceneIntegration& operator=(const RenderSceneIntegration&) =
            delete;

        [[nodiscard]] lux::cxx::TypeToken type() const noexcept override
        {
            return lux::cxx::typeToken<RenderSceneIntegration>();
        }
        [[nodiscard]] lux::cxx::expected<void, ESceneIntegrationError>
        prepare(SceneRuntimeAssemblyContext& context) noexcept override;
        [[nodiscard]] lux::cxx::expected<void, ESceneIntegrationError>
        finalize(SceneRuntimeAssemblyContext& context) noexcept override;
        [[nodiscard]] lux::cxx::expected<void, ESceneIntegrationError>
        onPublished(SceneRuntimePublishedContext& context) noexcept override;
        void processSafePoint() noexcept override;
        [[nodiscard]] ESceneIntegrationCloseStatus close() noexcept override;

        void settleViewCreation() noexcept;
        void reattachTarget(
            lux::render::RenderTargetId target,
            lux::math::Extent2u extent) noexcept;
        [[nodiscard]] lux::render::RenderSceneId sceneId() const noexcept;
        [[nodiscard]] const lux::ecs::render::presentation::
            PrimaryViewPresentationSnapshot*
        primaryViewPresentation() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    [[nodiscard]] inline RenderSceneIntegration* renderScene(
        SceneRuntime& runtime) noexcept
    {
        return runtime.integration<RenderSceneIntegration>();
    }

    [[nodiscard]] inline const RenderSceneIntegration* renderScene(
        const SceneRuntime& runtime) noexcept
    {
        return runtime.integration<RenderSceneIntegration>();
    }
}
