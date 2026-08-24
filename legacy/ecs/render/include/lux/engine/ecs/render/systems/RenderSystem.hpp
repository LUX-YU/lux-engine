#pragma once

#include <lux/engine/ecs/render/RenderStage.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/function/render/client/RenderLease.hpp>

#include <memory>
#include <vector>

namespace lux::render
{
    class RenderFrameSession;
    class RenderControlSession;
    class RenderUploadClient;
    class FeatureCatalog;
}

namespace lux::ecs
{
    /// Render-scene provider and owner of lightweight extraction stages.
    /// FrameCoordinator owns the lexical frame; ordinary render-facing
    /// Systems may follow this node in the same Schedule.
    class LUX_FUNCTION_PUBLIC RenderSystem final : public ISystem
    {
    public:
        RenderSystem(
            SceneRenderBinding&                 binding,
            ActiveRenderView&                   active_view,
            lux::render::RenderSceneLease       scene,
            std::vector<std::unique_ptr<RenderStage>> stages
        );
        ~RenderSystem() override;

        RenderSystem(const RenderSystem&) = delete;
        RenderSystem& operator=(const RenderSystem&) = delete;

        void onAdded(const SystemSetupContext& setup) override;
        void onRemoved(const SystemRemovalContext& removal) override;
        void update(const SystemUpdateContext& context) override;
        void requestClose() noexcept override;
        void requestClose(SystemCloseProgressSink progress) noexcept override;
        [[nodiscard]] bool closeComplete() const noexcept override;
        [[nodiscard]] bool closeNeedsOwnerTick() const noexcept override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::ecs
