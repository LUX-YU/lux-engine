#pragma once
#include <optional>
#include <lux/engine/editor/views/IViewHost.hpp>
#include <lux/engine/ui/Docking.hpp>
#include <lux/cxx/core/function_ref.hpp>

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
    struct ViewCandidate final
    {
        views::ViewRestoreKey restore_key;
        views::DetachedView owner;
        std::optional<bool> visible;
    };
    struct ViewVisibility final
    {
        views::ViewId id;
        bool visible{};
    };
    class PreparedViewBatch final
    {
    public:
        ~PreparedViewBatch();
        PreparedViewBatch(PreparedViewBatch&&) noexcept;
        PreparedViewBatch& operator=(PreparedViewBatch&&) noexcept;
        PreparedViewBatch(const PreparedViewBatch&) = delete;
        PreparedViewBatch& operator=(const PreparedViewBatch&) = delete;
        [[nodiscard]] std::span<const views::ViewId> viewIds() const noexcept;

    private:
        friend class ViewHost;
        struct Data;
        explicit PreparedViewBatch(std::unique_ptr<Data>) noexcept;
        std::unique_ptr<Data> data_;
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
        // All candidates stay detached until commit. Preparation failure leaves input owners unchanged.
        // A successful preparation owns them; abandoning it releases nodes before their code pins.
        [[nodiscard]] views::ViewResult<PreparedViewBatch> prepareBatch(
            std::span<ViewCandidate>,
            std::span<const ViewVisibility> existing = {},
            std::optional<lux::ui::PreparedDockTree> docking = {}
        );
        [[nodiscard]] views::ViewResult<object::SignalDelivery> commit(PreparedViewBatch&);
        // Close-only handoff: after successful detach and notification, before detached owners are
        // destroyed. The application can commit already prepared content permits here. This callback
        // must not fail or retain the Host borrow; it is never called if structural commit is refused.
        [[nodiscard]] views::ViewResult<object::SignalDelivery> commitClose(
            PreparedViewBatch&,
            cxx::function_ref<void()> handoff
        );
        [[nodiscard]] cxx::
            expected<PreparedViewBatch, views::ViewPreparationFailure> prepareClose(std::span<const views::ViewId>);
        // Native close is an unapproved user intent. Only the application decides last-view/content policy.
        [[nodiscard]] views::ViewResult<std::vector<views::ViewId>> closeIntents() const;
        [[nodiscard]] views::ViewResult<void> dismissCloseIntent(views::ViewId) noexcept;
        [[nodiscard]] views::ViewResult<views::ViewInfo> describe(views::ViewId) const override;
        // Synchronous owner-thread borrow only. Structural requests are deferred until it returns.
        // A caller checks pane.type() before a concrete cast and never retains the reference.
        [[nodiscard]] views::ViewResult<void> withView(views::ViewId, cxx::function_ref<void(lux::ui::Pane&)> visit);
        // Owner-thread safe point. Binding failure keeps the previous association and view state.
        [[nodiscard]] views::ViewCloseResult rebindContent(views::ViewId, const views::ViewContent&);
        [[nodiscard]] views::ViewCloseResult cancelPreview(views::ViewId);
        [[nodiscard]] views::ViewResult<std::vector<views::ViewInfo>> describeAll() const;
        [[nodiscard]] views::ViewResult<std::optional<views::ViewPreparationFailure>> closeFailure(views::ViewId) const;
        [[nodiscard]] views::ViewResult<void> close(views::ViewId) noexcept override;
        [[nodiscard]] views::ViewResult<void> show(views::ViewId) noexcept override;
        [[nodiscard]] views::ViewResult<void> hide(views::ViewId) noexcept;
        [[nodiscard]] views::ViewResult<void> focus(views::ViewId) noexcept override;
        [[nodiscard]] views::ViewResult<ViewDrain> drain();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
