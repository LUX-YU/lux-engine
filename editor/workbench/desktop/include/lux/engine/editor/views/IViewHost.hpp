#pragma once
#include <lux/engine/editor/views/ViewError.hpp>
#include <lux/engine/editor/contracts/CodeLease.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <utility>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/workspace/WorkspaceValues.hpp>

namespace lux::editor::views
{
    using ViewStateResult = cxx::expected<cxx::move_only_function<void()>, ViewPreparationFailure>;
    using ViewCaptureResult = cxx::expected<workspace::VersionedViewState, ViewPreparationFailure>;
    class PreparedViewState final
    {
    public:
        PreparedViewState(PreparedViewState&&) noexcept = default;
        PreparedViewState& operator=(PreparedViewState&& other) noexcept
        {
            PreparedViewState previous(std::move(other));
            using std::swap;
            swap(code_, previous.code_);
            swap(apply_, previous.apply_);
            return *this;
        }
        PreparedViewState(const PreparedViewState&) = delete;
        PreparedViewState& operator=(const PreparedViewState&) = delete;
        // Called once after preparation/preview cancellation, at the host commit boundary. No callbacks or IO.
        void apply() noexcept
        {
            auto adopted = std::move(apply_);
            if (adopted)
                adopted();
        }

    private:
        friend class DetachedView;
        PreparedViewState(contracts::CodeLease code, cxx::move_only_function<void()> apply)
            : code_(std::move(code)), apply_(std::move(apply))
        {}
        contracts::CodeLease code_;
        cxx::move_only_function<void()> apply_;
    };
    // Owner order is intentional, including move assignment. The host completes detach and transfers
    // resource retirement before destroying a mounted result. Factories use detached Pane constructors.
    class DetachedView final
    {
    public:
        using PrepareClose = ViewCloseResult (*)(lux::ui::Pane&);
        using PrepareState = ViewStateResult (*)(lux::ui::Pane&, std::uint32_t, std::span<const std::byte>);
        using CaptureState = ViewCaptureResult (*)(const lux::ui::Pane&);
        using CaptureContent = ViewContent (*)(const lux::ui::Pane&) noexcept;
        using RebindContent = ViewCloseResult (*)(lux::ui::Pane&, const ViewContent&);
        DetachedView(
            contracts::CodeLease code,
            std::unique_ptr<lux::ui::Pane> pane,
            PrepareClose prepare_close = nullptr,
            PrepareClose cancel_preview = nullptr,
            PrepareState prepare_state = nullptr,
            CaptureState capture_state = nullptr,
            CaptureContent capture_content = nullptr,
            RebindContent rebind_content = nullptr
        ) noexcept
            : code_(std::move(code)), pane_(std::move(pane)), prepare_close_(prepare_close),
              cancel_preview_(cancel_preview), prepare_state_(prepare_state), capture_state_(capture_state),
              capture_content_(capture_content), rebind_content_(rebind_content)
        {
            if (!code_.valid() || !pane_ || pane_->attachedRoot() || pane_->parent())
                std::terminate();
        }
        ~DetachedView() noexcept
        {
            if (pane_ && pane_->attachedRoot())
                std::terminate();
        }
        DetachedView(DetachedView&&) noexcept = default;
        DetachedView& operator=(DetachedView&& other) noexcept
        {
            DetachedView previous(std::move(other));
            using std::swap;
            swap(code_, previous.code_);
            swap(pane_, previous.pane_);
            swap(prepare_close_, previous.prepare_close_);
            swap(cancel_preview_, previous.cancel_preview_);
            swap(prepare_state_, previous.prepare_state_);
            swap(capture_state_, previous.capture_state_);
            swap(capture_content_, previous.capture_content_);
            swap(rebind_content_, previous.rebind_content_);
            swap(connections_, previous.connections_);
            return *this;
        }
        DetachedView(const DetachedView&) = delete;
        DetachedView& operator=(const DetachedView&) = delete;
        // A complete factory result owns its intent connections. Disconnect before destroying the Pane.
        void addConnection(object::Connection connection)
        {
            connections_.push_back(std::move(connection));
        }
        [[nodiscard]] lux::ui::Pane* pane() const noexcept
        {
            return pane_.get();
        }
        [[nodiscard]] bool usesCode(const contracts::CodeLease& code) const noexcept
        {
            return code_.sameOwner(code);
        }
        // Called at the host's outer safe point before focus/routing are revoked. BUSY retains the
        // entire mounted view and its pending close. It never releases a Session or task owner.
        [[nodiscard]] ViewCloseResult prepareClose()
        {
            return prepare_close_ ? prepare_close_(*pane_) : ViewCloseResult{};
        }
        [[nodiscard]] ViewCloseResult cancelPreview()
        {
            return cancel_preview_ ? cancel_preview_(*pane_) : ViewCloseResult{};
        }
        [[nodiscard]] ViewContent content() const noexcept
        {
            return capture_content_ ? capture_content_(*pane_) : ViewContent{};
        }
        [[nodiscard]] ViewCloseResult rebindContent(const ViewContent& content)
        {
            if (!content.valid())
                return cxx::unexpected(ViewPreparationFailure{"view.content", 0, "Invalid content association", false});
            if (rebind_content_)
                return rebind_content_(*pane_, content);
            if (content == this->content())
                return {};
            return cxx::unexpected(ViewPreparationFailure{"view.content", 0, "This view has no content binding", false});
        }
        [[nodiscard]] ViewCaptureResult captureState() const
        {
            return capture_state_ ? capture_state_(*pane_) : ViewCaptureResult{workspace::VersionedViewState{}};
        }
        [[nodiscard]] cxx::expected<PreparedViewState, ViewPreparationFailure> prepareState(
            std::uint32_t schema,
            std::span<const std::byte> bytes
        )
        {
            if (!prepare_state_)
            {
                if (schema != 1 || !bytes.empty())
                    return cxx::unexpected(
                        ViewPreparationFailure{"view.state", schema, "Unsupported view configuration", false}
                    );
                return PreparedViewState{code_, {}};
            }
            auto prepared = prepare_state_(*pane_, schema, bytes);
            if (!prepared)
                return cxx::unexpected(std::move(prepared.error()));
            return PreparedViewState{code_, std::move(*prepared)};
        }

    private:
        contracts::CodeLease code_;
        std::unique_ptr<lux::ui::Pane> pane_;
        PrepareClose prepare_close_{};
        PrepareClose cancel_preview_{};
        PrepareState prepare_state_{};
        CaptureState capture_state_{};
        CaptureContent capture_content_{};
        RebindContent rebind_content_{};
        std::vector<object::Connection> connections_;
    };
    // Accept only identities. Implementations queue intents; no callback may erase its own UI owner.
    class ViewRequests
    {
    public:
        virtual ~ViewRequests() = default;
        [[nodiscard]] virtual ViewResult<void> close(ViewId) noexcept = 0;
        [[nodiscard]] virtual ViewResult<void> show(ViewId) noexcept = 0;
        [[nodiscard]] virtual ViewResult<void> focus(ViewId) noexcept = 0;
    };
    class IViewHost : public ViewRequests
    {
    public:
        [[nodiscard]] virtual ViewResult<ViewInfo> describe(ViewId) const = 0;
    };
}
