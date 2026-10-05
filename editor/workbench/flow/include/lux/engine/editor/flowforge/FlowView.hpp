#pragma once
#include <lux/engine/editor/flowforge/FlowCompilationService.hpp>
#include <lux/engine/editor/flowforge/FlowInteraction.hpp>
#include <lux/engine/editor/persistence/DerivedArtifact.hpp>
#include <lux/engine/editor/views/IViewHost.hpp>

namespace lux::editor::persistence
{
    class WriteCoordinator;
    class IArtifactStore;
} // namespace lux::editor::persistence

namespace lux::editor::desktop
{
    struct UiDescriptor;
}

namespace lux::editor::flowforge
{
    extern const desktop::UiDescriptor kFlowView;
    struct FlowViewBinding final
    {
        sessions::TSessionKey<FlowSession> session;
        FlowInteraction* interaction{};
        friend bool operator==(FlowViewBinding, FlowViewBinding) = default;
    };
    struct FlowViewServices final
    {
        sessions::TSessionAccess<FlowSession> sessions;
        std::shared_ptr<FlowCompilationService> compilation;
        FlowEnvironment metadata;
        std::shared_ptr<persistence::IArtifactSubmission> publication;
    };
    struct FlowViewState final
    {
        LinkSettings linker;
    };
    using VFlowViewFailure = std::variant<
        FlowEditError,
        VFlowCompilationFailure,
        persistence::PersistenceFailure,
        views::EViewError,
        lux::ui::EAttachmentError>;
    template <class T> using FlowViewResult = cxx::expected<T, VFlowViewFailure>;
    class FlowView final : public lux::ui::Pane
    {
    public:
        [[nodiscard]] static FlowViewResult<std::unique_ptr<FlowView>> create(
            object::ObjectDispatcherRef,
            lux::ui::PaneId,
            FlowViewServices,
            std::optional<FlowViewBinding> = {},
            FlowViewState = {}
        );
        ~FlowView() noexcept override;
        FlowView(const FlowView&) = delete;
        FlowView& operator=(const FlowView&) = delete;
        FlowView(FlowView&&) = delete;
        FlowView& operator=(FlowView&&) = delete;
        [[nodiscard]] FlowViewResult<void> rebind(std::optional<FlowViewBinding>);
        [[nodiscard]] FlowViewResult<void> rebindContent(const views::ViewContent&);
        [[nodiscard]] FlowViewResult<void> beginEdit(std::string);
        [[nodiscard]] FlowViewResult<void> previewEdit(std::vector<VFlowEdit>&);
        [[nodiscard]] FlowViewResult<void> commitEdit();
        [[nodiscard]] FlowViewResult<void> cancelEdit();
        [[nodiscard]] FlowViewResult<void> undo();
        [[nodiscard]] FlowViewResult<void> redo();
        [[nodiscard]] FlowViewResult<FlowCompileId> compile();
        [[nodiscard]] FlowViewResult<void> retryLink(LinkSettings);
        [[nodiscard]] FlowViewResult<void> acknowledgeCompilation();
        // Returns the original publication owner's accepted request identity, not signal delivery success.
        [[nodiscard]] FlowViewResult<std::uint64_t> requestPublication();
        [[nodiscard]] const std::optional<FlowViewBinding>& binding() const noexcept;
        [[nodiscard]] const FlowViewResult<void>& status() const noexcept;
        [[nodiscard]] FlowCompileId compilation() const noexcept;
        [[nodiscard]] views::ViewCaptureResult captureState() const;
        [[nodiscard]] views::ViewStateResult prepareState(std::uint32_t, std::span<const std::byte>);

    private:
        FlowView(object::ObjectDispatcherRef, lux::ui::PaneId, FlowViewServices, FlowViewState);
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    [[nodiscard]] FlowViewResult<views::DetachedView> makeFlowView(
        object::ObjectDispatcherRef,
        lux::ui::PaneId,
        FlowViewServices,
        std::optional<FlowViewBinding> = {},
        FlowViewState = {}
    );
} // namespace lux::editor::flowforge
