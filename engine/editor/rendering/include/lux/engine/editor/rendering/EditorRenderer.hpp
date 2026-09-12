#pragma once
#include <lux/engine/editor/rendering/RenderView.hpp>
#include <lux/engine/editor/rendering/EditorFramePacket.hpp>
namespace lux::window
{
    class LuxWindow;
}
namespace lux::ui
{
    class UISession;
    class UiFrameSnapshot;
} // namespace lux::ui
namespace lux::editor::rendering
{
    class LUX_EDITOR_RENDERING_PUBLIC EditorRenderer final : public lux::scene::RenderRuntime
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<EditorRenderer>> create(lux::window::LuxWindow &,
                                                                                  lux::ui::UISession &,
                                                                                  const RendererConfig &) noexcept;
        // Window + UI initialization/font source outlive renderer close. No Pane invocation here.
        ~EditorRenderer() noexcept override;
        EditorRenderer(const EditorRenderer &) = delete;
        EditorRenderer &operator=(const EditorRenderer &) = delete;
        EditorRenderer(EditorRenderer &&) = delete;
        EditorRenderer &operator=(EditorRenderer &&) = delete;
        [[nodiscard]] ERendererState state() const noexcept;
        [[nodiscard]] RendererStatistics statistics() const noexcept;
        // Owning, allocation-free records; overflow is counted in statistics().dropped_events.
        [[nodiscard]] RenderResult<std::optional<RendererDiagnostic>> takeDiagnostic() noexcept;
        // Observes this immutable record's actual submission, then the real device completion watermark.
        // It does not infer physical screen display, and leaves the captured image descriptor unchanged.
        [[nodiscard]] RenderResult<ImageContentStamp> imageEvidence(const ViewImage &) const noexcept;
        // SPSC producer admission: the owner is the sole producer; no callback occurs before publication.
        [[nodiscard]] bool controlAvailable(std::size_t packets = 1) const noexcept;
        // Counts response-ring envelopes, including failures/unmatched packets. All three lanes share
        // the budget and rotate after each attempt. An envelope may contain multiple existing records.
        // Upload admission (at most reply_budget), one frame submission, each bounded View slot and one
        // maintenance frame are separate work, excluded from the returned reply count. Zero does no work.
        [[nodiscard]] RenderResult<std::size_t> poll(std::size_t reply_budget) noexcept;
        [[nodiscard]] RenderResult<std::unique_ptr<RenderView>> openView(lux::render::RenderSceneId,
                                                                         ViewConfig) noexcept;
        // Failure retains the snapshot. Success moves it into the returned packet.
        // The input image span is borrowed only until return; valid image leases are retained by packet.
        [[nodiscard]] RenderResult<EditorFramePacket> sealFrame(lux::ui::UiFrameSnapshot &,
                                                                std::span<const ViewImage>) noexcept;
        // SUBMITTED consumes packet into renderer-owned CPU queue; not proof of GPU submission.
        // BACKPRESSURED or failure leaves packet unchanged and caller-owned.
        [[nodiscard]] RenderResult<EFrameSubmit> trySubmitFrame(EditorFramePacket &) noexcept;
        [[nodiscard]] RenderResult<void> beginClose() noexcept;
        [[nodiscard]] RenderResult<ERenderClose> advanceClose() noexcept;
        [[nodiscard]] RenderResult<void> joinStopped() noexcept;
        [[nodiscard]] lux::cxx::expected<lux::scene::RenderRuntimeLease, lux::scene::RenderRuntimeFailure>
        acquire() noexcept override;

    private:
        friend class RenderView;
        void release() noexcept override;
        lux::render::RenderControlSession &control() noexcept override;
        lux::render::RenderProgramSession &programs() noexcept override;
        lux::render::RenderUploadClient upload() noexcept override;
        const lux::render::FeatureCatalog &features() const noexcept override;
        struct Impl;
        explicit EditorRenderer(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::rendering
