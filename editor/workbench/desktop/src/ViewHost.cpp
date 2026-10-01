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
    }
    struct PreparedViewBatch::Data final
    {
        std::uint64_t domain{}, revision{};
        std::vector<views::ViewId> ids;
        std::vector<ViewCandidate> candidates;
        std::vector<object::Connection> connections;
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
    std::span<const views::ViewId> PreparedViewBatch::created() const noexcept
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
            object::Connection close_connection;
            bool close_requested{};
            std::optional<views::ViewCloseFailure> close_failure;
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
                    slot.close_connection.disconnect();
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
                pane.focused()
            };
        }
        views::ViewResult<PreparedViewBatch> prepareBatch(
            std::span<ViewCandidate> candidates,
            std::span<const ViewVisibility> visibility
        )
        {
            if (closing_)
                return cxx::unexpected(views::EViewError::CLOSED);
            if (busy())
                return cxx::unexpected(views::EViewError::BUSY);
            Dispatch guard(dispatching_);
            auto data = std::make_unique<PreparedViewBatch::Data>();
            data->domain = domain_;
            data->revision = revision_;
            data->ids.reserve(candidates.size());
            data->candidates.reserve(candidates.size());
            data->connections.reserve(candidates.size());
            std::vector<lux::ui::Pane*> panes;
            std::vector<lux::ui::WindowVisibility> states;
            panes.reserve(candidates.size());
            states.reserve(visibility.size());
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
                const auto duplicate = [&](const views::ViewRestoreKey& key, const lux::ui::Pane& pane) {
                    return key == candidate.restore_key && pane.type() == candidate.owner.pane()->type();
                };
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
                auto& slot = slots_[next_slot];
                const views::ViewId id{domain_, static_cast<std::uint32_t>(next_slot++), slot.generation + 1};
                auto connected = object::LuxObject::connect(
                    candidate.owner.pane(),
                    &lux::ui::Pane::closeRequested,
                    [&slot, id]() noexcept {
                        if (slot.owner && slot.generation == id.generation)
                            slot.close_requested = true;
                    }
                );
                if (!connected)
                    return cxx::unexpected(views::EViewError::CAPACITY);
                data->connections.push_back(std::move(*connected));
                data->ids.push_back(id);
                panes.push_back(candidate.owner.pane());
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
            auto* data = prepared.data_.get();
            const bool stale = !data || data->domain != domain_ || data->revision != revision_ || !data->attachment;
            if (stale)
                return cxx::unexpected(views::EViewError::INVALID_ID);
            Dispatch guard(dispatching_);
            const auto adopt = [&]() noexcept {
                for (std::size_t i{}; i < data->ids.size(); ++i)
                {
                    auto& slot = slots_[data->ids[i].slot];
                    slot.generation = data->ids[i].generation;
                    slot.restore_key = std::move(data->candidates[i].restore_key);
                    slot.owner.emplace(std::move(data->candidates[i].owner));
                    slot.close_connection = std::move(data->connections[i]);
                    slot.close_requested = false;
                    slot.close_failure.reset();
                }
                ++revision_;
            };
            // Consume before dispatch. A notification may move or destroy the public preparation.
            auto active = std::move(prepared.data_);
            auto committed = root_.commit(*active->attachment, adopt);
            if (!committed)
            {
                prepared.data_ = std::move(active);
                return cxx::unexpected(attachmentError(committed.error()));
            }
            return committed->notifications;
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
            auto ready = slot.owner->prepareClose();
            if (!ready)
            {
                slot.close_failure = std::move(ready.error());
                return false;
            }
            slot.close_failure.reset();
            auto prepared = root_.prepareDetach(*slot.owner->pane());
            if (!prepared)
                return false;
            auto committed = root_.commit(*prepared);
            if (!committed)
                return false;
            append(report.notifications, committed->notifications);
            slot.close_connection.disconnect();
            slot.owner.reset();
            ++revision_;
            slot.close_requested = false;
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
            report.pending = requests_.size() + std::ranges::count_if(slots_, [](const Slot& slot) {
                                 return slot.owner && slot.close_requested;
                             });
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
        const auto id = prepared->created().front();
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
        std::span<const ViewVisibility> visibility
    )
    {
        return impl_->prepareBatch(candidates, visibility);
    }
    views::ViewResult<object::SignalDelivery> ViewHost::commit(PreparedViewBatch& prepared)
    {
        return impl_->commit(prepared);
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
        return impl_->info(*slot);
    }
    views::ViewResult<std::vector<views::ViewInfo>> ViewHost::describeAll() const
    {
        if (impl_->busy())
            return cxx::unexpected(views::EViewError::BUSY);
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
    views::ViewResult<std::optional<views::ViewCloseFailure>> ViewHost::closeFailure(views::ViewId id) const
    {
        if (impl_->busy())
            return cxx::unexpected(views::EViewError::BUSY);
        const auto* slot = impl_->find(id);
        if (!slot)
            return cxx::unexpected(views::EViewError::INVALID_ID);
        return slot->close_failure;
    }

}
