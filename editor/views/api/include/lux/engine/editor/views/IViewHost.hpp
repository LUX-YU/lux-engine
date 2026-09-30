#pragma once
#include <lux/engine/editor/views/ViewInfo.hpp>
#include <lux/engine/editor/contracts/CodeLease.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <utility>

namespace lux::editor::views
{
    // Owner order is intentional, including move assignment. The host completes detach and transfers
    // resource retirement before destroying a mounted result. Factories use detached Pane constructors.
    class DetachedView final
    {
    public:
        using PrepareClose = ViewResult<void> (*)(lux::ui::Pane&);
        DetachedView(contracts::CodeLease code, std::unique_ptr<lux::ui::Pane> pane, PrepareClose prepare_close = nullptr) noexcept
            : code_(std::move(code)), pane_(std::move(pane)), prepare_close_(prepare_close)
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
            return *this;
        }
        DetachedView(const DetachedView&) = delete;
        DetachedView& operator=(const DetachedView&) = delete;
        [[nodiscard]] lux::ui::Pane* pane() const noexcept
        {
            return pane_.get();
        }
        // Called at the host's outer safe point before focus/routing are revoked. BUSY retains the
        // entire mounted view and its pending close. It never releases a Session or task owner.
        [[nodiscard]] ViewResult<void> prepareClose()
        {
            return prepare_close_ ? prepare_close_(*pane_) : ViewResult<void>{};
        }

    private:
        contracts::CodeLease code_;
        std::unique_ptr<lux::ui::Pane> pane_;
        PrepareClose prepare_close_{};
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
