#pragma once
#include <lux/engine/editor/views/IViewHost.hpp>
#include <lux/engine/ui/Root.hpp>

namespace lux::editor::desktop
{
    struct ViewHostLimits final
    {
        std::size_t views{64}, requests{256};
    };
    struct ViewAdoption final
    {
        views::ViewId id;
        object::SignalDelivery notifications;
    };
    struct ViewDrain final
    {
        std::size_t completed{}, stale{}, focus_refused{}, pending{};
        object::SignalDelivery notifications;
    };

    // Single owner-thread controller. Root borrows the tree; only these slots own its top-level Panes.
    // Root and dispatcher outlive the host. Adoption/drain/destruction require an outer UI safe point.
    class ViewHost final : public views::IViewHost
    {
    public:
        explicit ViewHost(lux::ui::Root&, ViewHostLimits = {});
        ~ViewHost() noexcept override;
        ViewHost(const ViewHost&) = delete;
        ViewHost& operator=(const ViewHost&) = delete;
        ViewHost(ViewHost&&) = delete;
        ViewHost& operator=(ViewHost&&) = delete;

        // Preparation failure leaves candidate untouched. During Root notifications describe/adopt/drain
        // report BUSY; identity requests are queued and cannot destroy a callback's current object.
        [[nodiscard]] views::ViewResult<ViewAdoption> adopt(views::DetachedView&, views::ViewRestoreKey);
        [[nodiscard]] views::ViewResult<views::ViewInfo> describe(views::ViewId) const override;
        [[nodiscard]] views::ViewResult<std::vector<views::ViewInfo>> describeAll() const;
        [[nodiscard]] views::ViewResult<void> close(views::ViewId) noexcept override;
        [[nodiscard]] views::ViewResult<void> show(views::ViewId) noexcept override;
        [[nodiscard]] views::ViewResult<void> focus(views::ViewId) noexcept override;
        [[nodiscard]] views::ViewResult<ViewDrain> drain();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
