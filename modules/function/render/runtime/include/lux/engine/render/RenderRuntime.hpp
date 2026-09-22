#pragma once

#include <lux/engine/render/RenderView.hpp>
#include <lux/engine/render/RendererConfig.hpp>

namespace lux::render
{
enum class ERenderRuntimeState : std::uint8_t
{
    ACTIVE,
    STOPPING,
    RETIRED
};
struct RenderRuntimeStatus final
{
    ERenderRuntimeState state{ERenderRuntimeState::ACTIVE};
    RenderError error;
};

// Main owns admission and results; the dedicated backend owns Vulkan.
// Feature factories are cold inputs, not a built-in UI/Scene implementation.
class LUX_RENDER_RUNTIME_PUBLIC RenderRuntime final
{
  public:
    [[nodiscard]] static RenderResult<std::unique_ptr<RenderRuntime>> create(RendererConfig config);
    ~RenderRuntime();
    RenderRuntime(const RenderRuntime &) = delete;
    RenderRuntime &operator=(const RenderRuntime &) = delete;

    [[nodiscard]] RenderResult<RenderSceneLease> createScene(const RenderControlSession::CreateSceneConfig &,
                                                             const std::vector<SceneFeatureAttachment> &features);
    [[nodiscard]] RenderResult<std::unique_ptr<RenderView>> openView(const RenderSceneLease &, ViewConfig);
    [[nodiscard]] RenderResult<ViewObservation> observeView(RenderViewId) const noexcept;
    [[nodiscard]] RenderResult<ImageContentStamp> imageEvidence(const ViewImage &) const noexcept;
    [[nodiscard]] bool controlAvailable(std::size_t packets = 1) const noexcept;
    [[nodiscard]] RenderResult<std::reference_wrapper<RenderControlSession>> control() noexcept;
    [[nodiscard]] RenderResult<RenderUploadClient> upload() noexcept;
    [[nodiscard]] const FeatureCatalog &features() const noexcept;
    [[nodiscard]] RenderResult<EFrameSubmit> submit(RenderProgram<> &input) noexcept;
    [[nodiscard]] RenderResult<std::size_t> poll(std::size_t reply_budget, std::size_t &control_budget,
                                                 std::size_t &program_budget);
    [[nodiscard]] RenderRuntimeStatus status() const noexcept;
    [[nodiscard]] RendererStatistics statistics() const noexcept;
    [[nodiscard]] RenderResult<std::optional<RendererDiagnostic>> takeDiagnostic() noexcept;
    [[nodiscard]] RenderResult<void> beginClose() noexcept;
    [[nodiscard]] RenderResult<ERenderClose> advanceClose(std::size_t &replies, std::size_t &controls,
                                                          std::size_t &programs);
    [[nodiscard]] RenderResult<void> joinStopped() noexcept;

  private:
    struct Impl;
    explicit RenderRuntime(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};
} // namespace lux::render
