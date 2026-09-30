#pragma once
#include <lux/engine/editor/flowforge/FlowInteraction.hpp>
#include <lux/engine/editor/flowforge/FlowCompilationService.hpp>
#include <lux/engine/editor/persistence/ArtifactStore.hpp>
#include <lux/engine/editor/views/IViewHost.hpp>

namespace lux::editor::flowforge
{
    struct FlowViewBinding final
    {
        sessions::TSessionKey<FlowSession> session;
        FlowInteraction* interaction{};
        friend bool operator==(FlowViewBinding, FlowViewBinding) = default;
    };
    struct FlowViewServices final
    {
        sessions::TSessionAccess<FlowSession> sessions;
        FlowCompilationService& compilation;
        lux::flowforge::FlowSourceEnvironment metadata;
        void* request_owner{};
        FlowCompilationResult<FlowCompileId> (*compile)(void*, sessions::TSessionKey<FlowSession>){};
        persistence::PersistenceResult<void> (*publish)(void*, FlowCompileId){};
    };
    struct FlowViewState final
    {
        LinkSettings linker;
    };
    using VFlowViewFailure =
        std::variant<FlowEditError, VFlowCompilationFailure, persistence::PersistenceFailure, views::EViewError>;
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
        [[nodiscard]] FlowViewResult<void> beginEdit(std::string);
        [[nodiscard]] FlowViewResult<void> previewEdit(std::vector<VFlowEdit>&);
        [[nodiscard]] FlowViewResult<void> commitEdit();
        [[nodiscard]] FlowViewResult<void> cancelEdit();
        [[nodiscard]] FlowViewResult<void> undo();
        [[nodiscard]] FlowViewResult<void> redo();
        [[nodiscard]] FlowViewResult<FlowCompileId> compile();
        [[nodiscard]] FlowViewResult<void> retryLink(LinkSettings);
        [[nodiscard]] FlowViewResult<void> publish();
        [[nodiscard]] const std::optional<FlowViewBinding>& binding() const noexcept;
        [[nodiscard]] const FlowViewResult<void>& status() const noexcept;
        [[nodiscard]] FlowCompileId compilation() const noexcept;

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
}
