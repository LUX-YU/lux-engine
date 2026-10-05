#pragma once
#include <lux/engine/editor/views/ViewError.hpp>
#include <lux/engine/object/CodeLease.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <utility>

namespace lux::editor::views
{
    // Owner order is intentional, including move assignment. The host completes detach and transfers
    // resource retirement before destroying a mounted result. Factories use detached Pane constructors.
    class DetachedView final
    {
    public:
        using PrepareClose = ViewCloseResult (*)(lux::ui::Pane&);
        using CaptureContent = ViewContent (*)(const lux::ui::Pane&) noexcept;
        using RebindContent = ViewCloseResult (*)(lux::ui::Pane&, const ViewContent&);
        DetachedView(
            lux::object::CodeLease code,
            std::unique_ptr<lux::ui::Pane> pane,
            PrepareClose prepare_close = nullptr,
            PrepareClose cancel_preview = nullptr,
            CaptureContent capture_content = nullptr,
            RebindContent rebind_content = nullptr
        ) noexcept
            : code_(std::move(code)), pane_(std::move(pane)), prepare_close_(prepare_close),
              cancel_preview_(cancel_preview), capture_content_(capture_content), rebind_content_(rebind_content)
        {
            const bool is_invalid_owner = !code_.valid() || !pane_;
            const bool is_attached = pane_ && (pane_->attachedRoot() || pane_->parent());
            const bool is_invalid_candidate = is_invalid_owner || is_attached;
            if (is_invalid_candidate)
            {
                std::terminate();
            }
        }
        ~DetachedView() noexcept
        {
            if (pane_ && pane_->attachedRoot())
            {
                std::terminate();
            }
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
        [[nodiscard]] bool usesCode(const lux::object::CodeLease& code) const noexcept
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
            {
                return cxx::unexpected(ViewPreparationFailure{"view.content", 0, "Invalid content association", false});
            }
            if (rebind_content_)
            {
                return rebind_content_(*pane_, content);
            }
            if (content == this->content())
            {
                return {};
            }
            return cxx::unexpected(ViewPreparationFailure{"view.content", 0, "This view has no content binding", false}
            );
        }

    private:
        lux::object::CodeLease code_;
        std::unique_ptr<lux::ui::Pane> pane_;
        PrepareClose prepare_close_{};
        PrepareClose cancel_preview_{};
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
} // namespace lux::editor::views
