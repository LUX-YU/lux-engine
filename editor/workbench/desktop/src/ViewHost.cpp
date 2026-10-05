#include <lux/engine/editor/desktop/ViewHost.hpp>
#include <lux/engine/ui/Root.hpp>
#include <algorithm>
#include <atomic>
#include <optional>

namespace lux::editor::desktop
{
    namespace
    {
        void append(object::SignalDelivery& to, object::SignalDelivery from) noexcept
        {
            to.direct += from.direct;
            to.queued += from.queued;
            to.full += from.full;
            to.closed += from.closed;
        }
        views::EViewError attachmentError(lux::ui::EAttachmentError error) noexcept
        {
            using enum lux::ui::EAttachmentError;
            if (error == BUSY || error == WRONG_THREAD)
                return views::EViewError::BUSY;
            if (error == CAPACITY)
                return views::EViewError::CAPACITY;
            return views::EViewError::NOT_ATTACHED;
        }
        class Dispatch final
        {
        public:
            explicit Dispatch(bool& active) noexcept : active_(active)
            {
                active_ = true;
            }
            ~Dispatch() noexcept
            {
                active_ = false;
            }
            Dispatch(const Dispatch&) = delete;
            Dispatch& operator=(const Dispatch&) = delete;

        private:
            bool& active_;
        };
    } // namespace
    struct PreparedViewBatch::Data final
    {
        std::uint64_t domain{}, revision{};
        std::vector<views::ViewId> ids;
        std::vector<ViewCandidate> candidates;
        std::vector<views::DetachedView> retiring;
        bool detach{};
        std::optional<lux::ui::PreparedDockTree> docking;
        std::optional<lux::ui::PreparedAttachment> attachment;
    };
    PreparedViewBatch::PreparedViewBatch(std::unique_ptr<Data> data) noexcept : data_(std::move(data)) {}
    PreparedViewBatch::~PreparedViewBatch() = default;
    PreparedViewBatch::PreparedViewBatch(PreparedViewBatch&&) noexcept = default;
    PreparedViewBatch& PreparedViewBatch::operator=(PreparedViewBatch&& other) noexcept
    {
        PreparedViewBatch previous(std::move(other));
        data_.swap(previous.data_);
        return *this;
    }
    std::span<const views::ViewId> PreparedViewBatch::viewIds() const noexcept
    {
        return data_ ? std::span<const views::ViewId>{data_->ids} : std::span<const views::ViewId>{};
    }
    struct ViewHost::Impl final
    {
        enum class EAction : std::uint8_t
        {
            CLOSE,
            SHOW,
            HIDE,
            FOCUS
        };
        struct Slot final
        {
            std::uint64_t generation{};
            views::ViewRestoreKey restore_key;
            std::optional<views::DetachedView> owner;
            bool close_requested{};
            std::optional<views::ViewPreparationFailure> close_failure;
        };
        struct Request final
        {
            views::ViewId id;
            EAction action;
        };
        inline static std::atomic_uint64_t next_domain{1};
        lux::ui::Root& root_;
        const std::uint64_t domain_{next_domain.fetch_add(1, std::memory_order_relaxed)};
        ViewHostLimits limits_;
        std::vector<Slot> slots_;
        std::vector<Request> requests_, batch_;
        std::vector<views::ViewId> close_batch_;
        std::size_t in_flight_{};
        std::uint64_t revision_{};
        bool dispatching_{}, closing_{};

        Impl(lux::ui::Root& root, ViewHostLimits limits) : root_(root), limits_(limits), slots_(limits.views)
        {
            if (!domain_ || limits.views > UINT32_MAX || !root.isOnAffinityThread())
                std::terminate();
            requests_.reserve(limits.requests);
            batch_.reserve(limits.requests);
            close_batch_.reserve(limits.views);
        }
        ~Impl() noexcept
        {
            if (dispatching_ || !root_.isOnAffinityThread())
                std::terminate();
            closing_ = true;
            Dispatch guard(dispatching_);
            for (auto& slot : slots_)
                if (slot.owner)
                {
                    if (!slot.owner->prepareClose())
                        std::terminate();
                    auto prepared = root_.prepareDetach(*slot.owner->pane());
                    if (!prepared || !root_.commit(*prepared))
                        std::terminate();
                    slot.owner.reset();
                }
        }
        Slot* find(views::ViewId id) noexcept
        {
            if (id.domain != domain_ || id.slot >= slots_.size())
                return nullptr;
            auto& slot = slots_[id.slot];
            return slot.owner && slot.generation == id.generation ? &slot : nullptr;
        }
        views::ViewId identity(const Slot& slot) const noexcept
        {
            return {domain_, static_cast<std::uint32_t>(&slot - slots_.data()), slot.generation};
        }
        bool busy() const noexcept
        {
            return dispatching_ || !root_.isOnAffinityThread();
        }
        views::ViewInfo info(const Slot& slot) const
        {
            const auto& pane = *slot.owner->pane();
            return {
                identity(slot),
                pane.type(),
                slot.restore_key,
                std::string(pane.title()),
                pane.visible(),
                pane.focused(),
                slot.owner->content()
            };
        }
        views::ViewResult<PreparedViewBatch> prepareBatch(
            std::span<ViewCandidate> candidates,
            std::span<const ViewVisibility> visibility,
            std::optional<lux::ui::PreparedDockTree> docking = {}
        )
        {
            if (closing_)
                return cxx::unexpected(views::EViewError::CLOSED);
            if (busy())
                return cxx::unexpected(views::EViewError::BUSY);
            Dispatch guard(dispatching_);
            return prepareAdmitted(candidates, visibility, std::move(docking));
        }
        views::ViewResult<PreparedViewBatch> prepareAdmitted(
            std::span<ViewCandidate> candidates,
            std::span<const ViewVisibility> visibility,
            std::optional<lux::ui::PreparedDockTree> docking
        )
        {
            auto data = std::make_unique<PreparedViewBatch::Data>();
            data->docking = std::move(docking);
            data->domain = domain_;
            data->revision = revision_;
            data->ids.reserve(candidates.size());
            data->candidates.reserve(candidates.size());
            std::vector<lux::ui::Pane*> panes;
            std::vector<lux::ui::WindowVisibility> states;
            panes.reserve(candidates.size());
            states.reserve(visibility.size() + candidates.size());
            for (const auto& state : visibility)
            {
                auto* slot = find(state.id);
                if (!slot)
                    return cxx::unexpected(views::EViewError::INVALID_ID);
                states.push_back({slot->owner->pane(), state.visible});
            }
            std::size_t next_slot{};
            for (const auto& candidate : candidates)
            {
                if (!candidate.owner.pane() || !candidate.restore_key.isValid())
                    return cxx::unexpected(views::EViewError::NOT_ATTACHED);
                const auto duplicate = [&](const views::ViewRestoreKey& key, const lux::ui::Pane& pane)
                { return key == candidate.restore_key && pane.type() == candidate.owner.pane()->type(); };
                for (const auto& slot : slots_)
                    if (slot.owner && duplicate(slot.restore_key, *slot.owner->pane()))
                        return cxx::unexpected(views::EViewError::INVALID_ID);
                for (const auto& prior : candidates.first(panes.size()))
                    if (duplicate(prior.restore_key, *prior.owner.pane()))
                        return cxx::unexpected(views::EViewError::INVALID_ID);
                while (next_slot < slots_.size() &&
                       (slots_[next_slot].owner || slots_[next_slot].generation == UINT64_MAX))
                    ++next_slot;
                if (next_slot == slots_.size())
                    return cxx::unexpected(views::EViewError::CAPACITY);
                const auto& slot = slots_[next_slot];
                const views::ViewId id{domain_, static_cast<std::uint32_t>(next_slot++), slot.generation + 1};
                data->ids.push_back(id);
                panes.push_back(candidate.owner.pane());
                if (candidate.visible)
                    states.push_back({candidate.owner.pane(), *candidate.visible});
            }
            auto prepared = root_.prepareMount(panes, states);
            if (!prepared)
                return cxx::unexpected(attachmentError(prepared.error()));
            data->attachment.emplace(std::move(*prepared));
            // No callbacks or allocation remain. Input ownership changes only after the entire batch prepares.
            for (auto& candidate : candidates)
                data->candidates.push_back(std::move(candidate));
            return PreparedViewBatch{std::move(data)};
        }
        views::ViewResult<object::SignalDelivery> commit(PreparedViewBatch& prepared)
        {
            if (closing_)
                return cxx::unexpected(views::EViewError::CLOSED);
            if (busy())
                return cxx::unexpected(views::EViewError::BUSY);
            Dispatch guard(dispatching_);
            return commitAdmitted(prepared);
        }
        views::ViewResult<object::SignalDelivery> commitAdmitted(
            PreparedViewBatch& prepared,
            cxx::function_ref<void()>* handoff = nullptr
        )
        {
            auto* data = prepared.data_.get();
            const bool stale = !data || data->domain != domain_ || data->revision != revision_ || !data->attachment;
            if (stale)
                return cxx::unexpected(views::EViewError::INVALID_ID);
            const auto adopt = [&]() noexcept
            {
                for (std::size_t i{}; i < data->ids.size(); ++i)
                {
                    auto& slot = slots_[data->ids[i].slot];
                    if (data->detach)
                    {
                        data->retiring.push_back(std::move(*slot.owner));
                        slot.owner.reset();
                    }
                    else
                    {
                        slot.generation = data->ids[i].generation;
                        slot.restore_key = std::move(data->candidates[i].restore_key);
                        slot.owner.emplace(std::move(data->candidates[i].owner));
                    }
                    slot.close_requested = false;
                    slot.close_failure.reset();
                }
                ++revision_;
                if (data->docking)
                    root_.commitDockTree(std::move(*data->docking));
            };
            // Consume before dispatch. A notification may move or destroy the public preparation.
            auto active = std::move(prepared.data_);
            auto committed = root_.commit(*active->attachment, adopt);
            if (!committed)
            {
                prepared.data_ = std::move(active);
                return cxx::unexpected(attachmentError(committed.error()));
            }
            if (handoff)
                (*handoff)();
            return committed->notifications;
        }
        cxx::expected<PreparedViewBatch, views::ViewPreparationFailure> prepareClose(std::span<const views::ViewId> ids)
        {
            if (closing_ || busy())
                return cxx::unexpected(views::ViewPreparationFailure{
                    "view.host",
                    static_cast<std::uint64_t>(closing_ ? views::EViewError::CLOSED : views::EViewError::BUSY),
                    "Host unavailable",
                    !closing_
                });
            Dispatch guard(dispatching_);
            return prepareCloseAdmitted(ids);
        }
        cxx::expected<PreparedViewBatch, views::ViewPreparationFailure> prepareCloseAdmitted(
            std::span<const views::ViewId> ids
        )
        {
            const auto failure = [](views::EViewError code, std::string message)
            {
                return cxx::unexpected(views::ViewPreparationFailure{
                    "view.host",
                    static_cast<std::uint64_t>(code),
                    std::move(message),
                    code == views::EViewError::BUSY
                });
            };
            auto data = std::make_unique<PreparedViewBatch::Data>();
            data->domain = domain_;
            data->revision = revision_;
            data->detach = true;
            data->ids.assign(ids.begin(), ids.end());
            data->retiring.reserve(ids.size());
            std::vector<lux::ui::Pane*> panes;
            panes.reserve(ids.size());
            const auto windows = root_.windowRevision();
            for (std::size_t i{}; i < ids.size(); ++i)
            {
                auto* slot = find(ids[i]);
                if (!slot || std::ranges::find(ids.first(i), ids[i]) != ids.first(i).end())
                    return failure(views::EViewError::INVALID_ID, "Close target is stale or repeated");
                panes.push_back(slot->owner->pane());
            }
            for (auto id : ids)
            {
                auto* slot = find(id);
                auto ready = slot->owner->prepareClose();
                if (!ready)
                    return cxx::unexpected(std::move(ready.error()));
            }
            if (revision_ != data->revision || root_.windowRevision() != windows)
                return failure(views::EViewError::INVALID_ID, "Workbench changed during close preparation");
            auto prepared = root_.prepareDetach(panes);
            if (!prepared)
                return failure(attachmentError(prepared.error()), "Detach preparation refused");
            data->attachment.emplace(std::move(*prepared));
            return PreparedViewBatch{std::move(data)};
        }
        views::ViewResult<void> request(views::ViewId id, EAction action) noexcept
        {
            if (!root_.isOnAffinityThread())
                return cxx::unexpected(views::EViewError::BUSY);
            if (closing_)
                return cxx::unexpected(views::EViewError::CLOSED);
            if (!find(id))
                return cxx::unexpected(views::EViewError::INVALID_ID);
            if (requests_.size() + in_flight_ >= limits_.requests)
                return cxx::unexpected(views::EViewError::CAPACITY);
            requests_.push_back({id, action});
            return {};
        }
        bool detach(Slot& slot, ViewDrain& report)
        {
            const auto id = identity(slot);
            auto prepared = prepareCloseAdmitted(std::span{&id, 1});
            if (!prepared)
            {
                slot.close_failure = std::move(prepared.error());
                return false;
            }
            auto committed = commitAdmitted(*prepared);
            if (!committed)
                return false;
            append(report.notifications, *committed);
            ++report.completed;
            return true;
        }
        views::ViewResult<ViewDrain> drain()
        {
            if (busy())
                return cxx::unexpected(views::EViewError::BUSY);
            Dispatch guard(dispatching_);
            ViewDrain report;
            batch_.swap(requests_);
            in_flight_ = batch_.size();
            close_batch_.clear();
            for (auto& slot : slots_)
                if (slot.owner && std::exchange(slot.close_requested, false))
                    close_batch_.push_back(identity(slot));
            for (const auto& request : batch_)
            {
                auto* slot = find(request.id);
                if (!slot)
                    ++report.stale;
                else if (request.action == EAction::CLOSE)
                {
                    slot->close_failure.reset(); // A new explicit close request retries a previous refusal.
                    if (!detach(*slot, report))
                        slot->close_requested = !slot->close_failure || slot->close_failure->retryable;
                }
                else if (request.action == EAction::SHOW || request.action == EAction::HIDE)
                {
                    slot->owner->pane()->setVisible(request.action == EAction::SHOW);
                    ++report.completed;
                }
                else if (root_.requestFocus(*slot->owner->pane()))
                    ++report.completed;
                else
                    ++report.focus_refused;
                --in_flight_;
            }
            batch_.clear();
            for (auto id : close_batch_)
                if (auto* slot = find(id); slot && (!slot->close_failure || slot->close_failure->retryable))
                    if (!detach(*slot, report))
                        slot->close_requested = !slot->close_failure || slot->close_failure->retryable;
            report.pending =
                requests_.size() +
                std::ranges::count_if(slots_, [](const Slot& slot) { return slot.owner && slot.close_requested; });
            return report;
        }
    };
    ViewHost::ViewHost(lux::ui::Root& root, ViewHostLimits limits) : impl_(std::make_unique<Impl>(root, limits)) {}
    ViewHost::~ViewHost() noexcept = default;
    views::ViewResult<ViewAdoption> ViewHost::adopt(views::DetachedView& candidate, views::ViewRestoreKey key)
    {
        ViewCandidate input{std::move(key), std::move(candidate)};
        auto prepared = impl_->prepareBatch(std::span{&input, 1}, {});
        if (!prepared)
        {
            candidate = std::move(input.owner);
            return cxx::unexpected(prepared.error());
        }
        const auto id = prepared->viewIds().front();
        auto committed = impl_->commit(*prepared);
        if (!committed)
        {
            candidate = std::move(prepared->data_->candidates.front().owner);
            return cxx::unexpected(committed.error());
        }
        return ViewAdoption{id, *committed};
    }
    views::ViewResult<PreparedViewBatch> ViewHost::prepareBatch(
        std::span<ViewCandidate> candidates,
        std::span<const ViewVisibility> visibility,
        std::optional<lux::ui::PreparedDockTree> docking
    )
    {
        return impl_->prepareBatch(candidates, visibility, std::move(docking));
    }
    views::ViewResult<object::SignalDelivery> ViewHost::commit(PreparedViewBatch& prepared)
    {
        return impl_->commit(prepared);
    }
    views::ViewResult<object::SignalDelivery> ViewHost::commitClose(
        PreparedViewBatch& prepared,
        cxx::function_ref<void()> handoff
    )
    {
        if (impl_->closing_)
            return cxx::unexpected(views::EViewError::CLOSED);
        if (impl_->busy())
            return cxx::unexpected(views::EViewError::BUSY);
        if (!prepared.data_ || !prepared.data_->detach)
            return cxx::unexpected(views::EViewError::INVALID_ID);
        Dispatch guard(impl_->dispatching_);
        return impl_->commitAdmitted(prepared, &handoff);
    }
    cxx::expected<PreparedViewBatch, views::ViewPreparationFailure> ViewHost::prepareClose(
        std::span<const views::ViewId> ids
    )
    {
        return impl_->prepareClose(ids);
    }
    views::ViewResult<std::vector<views::ViewId>> ViewHost::closeIntents() const
    {
        if (impl_->busy())
            return cxx::unexpected(views::EViewError::BUSY);
        std::vector<views::ViewId> result;
        for (const auto& slot : impl_->slots_)
            if (slot.owner && slot.owner->pane()->hasCloseRequest())
                result.push_back(impl_->identity(slot));
        return result;
    }
    views::ViewResult<void> ViewHost::dismissCloseIntent(views::ViewId id) noexcept
    {
        if (impl_->busy())
            return cxx::unexpected(views::EViewError::BUSY);
        auto* slot = impl_->find(id);
        if (!slot)
            return cxx::unexpected(views::EViewError::INVALID_ID);
        slot->owner->pane()->dismissCloseRequest();
        return {};
    }
    views::ViewResult<void> ViewHost::hide(views::ViewId id) noexcept
    {
        return impl_->request(id, Impl::EAction::HIDE);
    }
    views::ViewResult<views::ViewInfo> ViewHost::describe(views::ViewId id) const
    {
        if (impl_->busy())
            return cxx::unexpected(views::EViewError::BUSY);
        const auto* slot = impl_->find(id);
        if (!slot)
            return cxx::unexpected(views::EViewError::INVALID_ID);
        Dispatch guard(impl_->dispatching_);
        return impl_->info(*slot);
    }
    views::ViewResult<void> ViewHost::withView(views::ViewId id, cxx::function_ref<void(lux::ui::Pane&)> visit)
    {
        if (impl_->busy())
            return cxx::unexpected(views::EViewError::BUSY);
        auto* slot = impl_->find(id);
        if (!slot)
            return cxx::unexpected(views::EViewError::INVALID_ID);
        Dispatch guard(impl_->dispatching_);
        visit(*slot->owner->pane());
        return {};
    }
    views::ViewCloseResult ViewHost::rebindContent(views::ViewId id, const views::ViewContent& content)
    {
        if (impl_->busy())
            return cxx::unexpected(views::ViewPreparationFailure{"view.busy", 0, "Binding is busy", true});
        auto* slot = impl_->find(id);
        if (!slot)
            return cxx::unexpected(views::ViewPreparationFailure{"view.stale", 0, "View no longer exists", false});
        Dispatch guard(impl_->dispatching_);
        return slot->owner->rebindContent(content);
    }
    views::ViewCloseResult ViewHost::cancelPreview(views::ViewId id)
    {
        if (impl_->busy())
            return cxx::unexpected(views::ViewPreparationFailure{"view.busy", 0, "Preview is busy", true});
        auto* slot = impl_->find(id);
        if (!slot)
            return cxx::unexpected(views::ViewPreparationFailure{"view.stale", 0, "View no longer exists", false});
        Dispatch guard(impl_->dispatching_);
        return slot->owner->cancelPreview();
    }
    views::ViewResult<std::vector<views::ViewInfo>> ViewHost::describeAll() const
    {
        if (impl_->busy())
            return cxx::unexpected(views::EViewError::BUSY);
        Dispatch guard(impl_->dispatching_);
        std::vector<views::ViewInfo> result;
        result.reserve(impl_->slots_.size());
        for (const auto& slot : impl_->slots_)
            if (slot.owner)
                result.push_back(impl_->info(slot));
        return result;
    }
    views::ViewResult<void> ViewHost::close(views::ViewId id) noexcept
    {
        return impl_->request(id, Impl::EAction::CLOSE);
    }
    views::ViewResult<void> ViewHost::show(views::ViewId id) noexcept
    {
        return impl_->request(id, Impl::EAction::SHOW);
    }
    views::ViewResult<void> ViewHost::focus(views::ViewId id) noexcept
    {
        return impl_->request(id, Impl::EAction::FOCUS);
    }
    views::ViewResult<ViewDrain> ViewHost::drain()
    {
        return impl_->drain();
    }
    views::ViewResult<std::optional<views::ViewPreparationFailure>> ViewHost::closeFailure(views::ViewId id) const
    {
        if (impl_->busy())
            return cxx::unexpected(views::EViewError::BUSY);
        const auto* slot = impl_->find(id);
        if (!slot)
            return cxx::unexpected(views::EViewError::INVALID_ID);
        return slot->close_failure;
    }

} // namespace lux::editor::desktop
