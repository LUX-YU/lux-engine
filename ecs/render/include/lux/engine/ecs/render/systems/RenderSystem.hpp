#pragma once

#include <lux/engine/ecs/render/RenderSystemBuilder.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/function/render/client/RenderLease.hpp>

#include <memory>
#include <span>
#include <string_view>

namespace lux::render
{
    class RenderFrameSession;
    class RenderControlSession;
    class RenderUploadClient;
    class FeatureCatalog;
}

namespace lux::ecs
{
    /// The schedule's sole rendering node. FrameCoordinator owns the lexical
    /// frame; this system only extracts ECS state into an already-open frame.
    class LUX_FUNCTION_PUBLIC RenderSystem final : public ISystem
    {
    public:
        RenderSystem(
            lux::render::RenderFrameSession&    session,
            lux::render::RenderControlSession&  control,
            lux::render::RenderUploadClient     upload,
            lux::render::RenderSceneLease       scene,
            RenderSystemPlan                    plan
        );
        ~RenderSystem() override;

        RenderSystem(const RenderSystem&) = delete;
        RenderSystem& operator=(const RenderSystem&) = delete;

        void onAdded(const SystemSetupContext& setup) override;
        void onRemoved(const SystemRemovalContext& removal) override;
        void update(const SystemUpdateContext& context) override;

        [[nodiscard]] std::span<const std::string_view>
        renderFeatures() const noexcept;

        void setFeatures(const lux::render::FeatureCatalog& catalog) noexcept;
        void bindFeature(
            std::string_view          name,
            lux::render::FeatureHandle handle
        );
        void unbindFeature(
            std::string_view name,
            lux::render::FeatureHandle expected = {}) noexcept;

        void settle();
        [[nodiscard]] lux::cxx::expected<
            InstalledRenderSubsystemBatch,
            RenderAssemblyFailure>
        installSubsystemBatch(RenderSubsystemMutationBatch&& batch);
        [[nodiscard]] lux::cxx::expected<void, RenderAssemblyFailure>
        removeSubsystemBatch(InstalledRenderSubsystemBatch&& batch);
        [[nodiscard]] lux::render::ERenderLeaseCloseStatus close() noexcept;

        [[nodiscard]] SceneRenderBinding& binding() noexcept;
        [[nodiscard]] ActiveRenderView& activeView() noexcept;
        [[nodiscard]] std::uint64_t droppedStaleCommands() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::ecs
