#include <lux/engine/render/transport/State.hpp>

#include <utility>

#include <lux/cxx/core/CheckedArithmetic.hpp>

namespace lux::render::detail
{
    TransportState::TransportState(std::shared_ptr<Routes> route_values, TransportCapacity limits) noexcept
        : routes(std::move(route_values)), capacity(limits), budget(limits.replies, limits.bulk_bytes),
          commands(limits.pending_commands), priority(limits.pending_priority), bulk(limits.pending_bulk)
    {
        cells.reserve(capacity.replies);
        const auto physical = cells.capacity();
        cells.reserve(physical);
        std::vector<RequestId> prepared;
        prepared.reserve(physical);
        for (std::size_t i = 0; i < physical; ++i)
        {
            auto id = cells.tryEmplacePrepared(OperationId{});
            if (!id) std::terminate();
            cells.aux(*id)->attachments.reserve(capacity.attachments);
            prepared.push_back(*id);
        }
        for (auto id : prepared) cells.erase(id);
        for (Ring* ring : {&commands, &priority})
        {
            for (unsigned i = 0; i < 4; ++i)
            {
                auto* slot = ring->tryBeginWrite();
                if (!slot) std::terminate();
                slot->reserve(capacity.records);
                if (!ring->publishWrite() || !ring->tryAcquireRead()) std::terminate();
            }
        }
    }

    const RouteEntry* TransportState::route(OperationId id) const noexcept
    {
        auto* found = routes->entries.find(id);
        return found ? found->get() : nullptr;
    }

    RenderResult<RequestId> TransportState::reserve(
        OperationId id, const OwnedWireBytes& bytes, std::span<const OwnedWireBytes> attachments, bool is_bulk
    ) noexcept
    {
        std::size_t accounted = bytes.size();
        for (const auto& attachment : attachments)
        {
            auto sum = cxx::checkedAdd(accounted, attachment.size());
            if (!sum) return cxx::unexpected(RenderError{kTransportBudget});
            accounted = *sum;
        }
        cxx::BudgetReservation<> lease;
        if (is_bulk)
        {
            auto acquired = budget.tryAcquire(1, accounted);
            if (!acquired) return cxx::unexpected(RenderError{kTransportBudget});
            lease = std::move(*acquired);
        }
        std::lock_guard lock(reply_mutex);
        if (admission.closed()) return cxx::unexpected(RenderError{kTransportStopped});
        if (cells.size() >= capacity.replies) return cxx::unexpected(RenderError{kTransportCapacity});
        auto key = cells.tryEmplacePrepared(id);
        if (!key) return cxx::unexpected(RenderError{kTransportCapacity});
        auto& backing = *cells.aux(*key);
        backing.input = bytes;
        backing.attachments.assign(attachments.begin(), attachments.end());
        backing.budget = std::move(lease);
        work_count.fetch_add(1, std::memory_order_relaxed);
        return *key;
    }

    void TransportState::rollback(RequestId id) noexcept
    {
        static_cast<void>(abandon(id));
        releaseWork(id);
    }

    void TransportState::releaseWork(RequestId id) noexcept
    {
        RequestBacking* backing = nullptr;
        {
            std::lock_guard lock(reply_mutex);
            auto* cell = cells.find(id);
            if (!cell || cell->work_done) std::terminate();
            backing = cells.aux(id);
        }
        // Exactly one queue consumer/promise owns input use. The cell cannot be
        // erased until work_done, so arbitrary source destructors run unlocked.
        backing->input = {};
        backing->attachments.clear();
        backing->budget.release();
        OwnedWireBytes discarded;
        {
            std::lock_guard lock(reply_mutex);
            auto& cell = *cells.find(id);
            cell.work_done = true;
            if (!cell.client)
            {
                discarded = std::exchange(backing->output, {});
                cells.erase(id);
            }
        }
        work_count.fetch_sub(1, std::memory_order_release);
        notify();
    }

    RenderResult<void> TransportState::finish(
        RequestId id, EOperationOutcome outcome, RenderError error, OwnedWireBytes bytes
    ) noexcept
    {
        {
            std::lock_guard lock(reply_mutex);
            auto* cell = cells.find(id);
            if (!cell || cell->terminal) return cxx::unexpected(RenderError{kTransportStale});
            cell->outcome = outcome;
            cell->error = error;
            if (cell->client) cells.aux(id)->output = std::move(bytes);
            cell->terminal = true;
        }
        notify();
        return {};
    }

    RenderResult<std::optional<TerminalReply>> TransportState::take(RequestId id) noexcept
    {
        TerminalReply result;
        {
            std::lock_guard lock(reply_mutex);
            auto* cell = cells.find(id);
            if (!cell || !cell->client) return cxx::unexpected(RenderError{kTransportStale});
            if (!cell->terminal) return std::optional<TerminalReply>{};
            result.state_ = shared_from_this();
            result.operation_ = cell->operation;
            result.outcome = cell->outcome;
            result.error = cell->error;
            result.bytes_ = std::exchange(cells.aux(id)->output, {});
            cell->client = false;
            if (cell->work_done) cells.erase(id);
        }
        notify();
        return std::optional<TerminalReply>{std::move(result)};
    }

    RenderResult<void> TransportState::abandon(RequestId id) noexcept
    {
        OwnedWireBytes discarded;
        {
            std::lock_guard lock(reply_mutex);
            auto* cell = cells.find(id);
            if (!cell || !cell->client) return cxx::unexpected(RenderError{kTransportStale});
            cell->client = false;
            discarded = std::exchange(cells.aux(id)->output, {});
            if (cell->work_done) cells.erase(id);
        }
        notify();
        return {};
    }

    RenderResult<WireView> TransportState::input(RequestId id) noexcept
    {
        std::lock_guard lock(reply_mutex);
        auto* cell = cells.find(id);
        if (!cell || cell->work_done) return cxx::unexpected(RenderError{kTransportStale});
        auto* backing = cells.aux(id);
        return WireView{route(cell->operation)->descriptor.payload, backing->input, backing->attachments};
    }

    void TransportState::stop() noexcept
    {
        admission.close();
        {
            std::lock_guard lock(reply_mutex);
            for (auto& cell : cells)
            {
                if (cell.terminal) continue;
                cell.terminal = true;
                cell.outcome = EOperationOutcome::CANCELLED;
                cell.error = RenderError{kTransportStopped};
            }
        }
        notify();
    }

    void TransportState::notify() noexcept
    {
        {
            std::lock_guard lock(wake_mutex);
            wake_epoch.fetch_add(1, std::memory_order_release);
        }
        wake.notify_all();
    }
}

namespace lux::render
{
    RenderResult<WireView> TerminalReply::wireView() const noexcept
    {
        if (!state_) return cxx::unexpected(RenderError{kTransportStale});
        if (outcome != EOperationOutcome::SUCCESS) return cxx::unexpected(error);
        const auto* entry = state_->route(operation_);
        if (!entry || !entry->descriptor.reply) return cxx::unexpected(RenderError{kTransportContract});
        return WireView{*entry->descriptor.reply, bytes_, {}};
    }

    RenderResult<std::optional<TerminalReply>> ReplyInbox::poll(ReplyTicket ticket) const noexcept
    {
        if (ticket.owner_ != state_->routes->identity) return cxx::unexpected(RenderError{kTransportOwner});
        return state_->take(ticket.id_);
    }

    RenderResult<void> ReplyInbox::abandon(ReplyTicket ticket) const noexcept
    {
        if (ticket.owner_ != state_->routes->identity) return cxx::unexpected(RenderError{kTransportOwner});
        return state_->abandon(ticket.id_);
    }

    RenderRequest::RenderRequest(RenderRequest&& other) noexcept
        : state_(std::move(other.state_)), id_(other.id_) {}

    RenderRequest& RenderRequest::operator=(RenderRequest&& other) noexcept
    {
        if (this == &other) return *this;
        static_cast<void>(abandon());
        state_ = std::move(other.state_);
        id_ = other.id_;
        return *this;
    }

    RenderRequest::~RenderRequest() { static_cast<void>(abandon()); }

    ReplyTicket RenderRequest::ticket() const noexcept
    {
        return ReplyTicket(state_ ? state_->routes->identity : nullptr, id_);
    }

    RenderResult<std::optional<TerminalReply>> RenderRequest::poll() noexcept
    {
        if (!state_) return cxx::unexpected(RenderError{kTransportStale});
        auto result = state_->take(id_);
        if (result && result->has_value()) state_.reset();
        return result;
    }

    RenderResult<void> RenderRequest::abandon() noexcept
    {
        if (!state_) return cxx::unexpected(RenderError{kTransportStale});
        auto state = std::move(state_);
        return state->abandon(id_);
    }

    ReplyPromise::ReplyPromise(ReplyPromise&& other) noexcept
        : state_(std::move(other.state_)), id_(other.id_) {}

    ReplyPromise& ReplyPromise::operator=(ReplyPromise&& other) noexcept
    {
        if (this == &other) return *this;
        release();
        state_ = std::move(other.state_);
        id_ = other.id_;
        return *this;
    }

    ReplyPromise::~ReplyPromise() { release(); }

    void ReplyPromise::release() noexcept
    {
        if (!state_) return;
        auto state = std::move(state_);
        static_cast<void>(state->finish(id_, EOperationOutcome::CANCELLED, RenderError{kTransportCancelled}));
        state->releaseWork(id_);
    }

    RenderResult<WireView> ReplyPromise::input() const noexcept
    {
        if (!state_) return cxx::unexpected(RenderError{kTransportStale});
        return state_->input(id_);
    }

    RenderResult<void> ReplyPromise::success() noexcept
    {
        if (!state_) return cxx::unexpected(RenderError{kTransportStale});
        auto wire = input();
        if (!wire) return cxx::unexpected(wire.error());
        OperationId operation;
        {
            std::lock_guard lock(state_->reply_mutex);
            operation = state_->cells.find(id_)->operation;
        }
        if (state_->route(operation)->descriptor.reply)
        {
            static_cast<void>(fail(RenderError{kTransportContract}));
            return cxx::unexpected(RenderError{kTransportContract});
        }
        return state_->finish(id_, EOperationOutcome::SUCCESS, {});
    }

    RenderResult<void> ReplyPromise::success(const DataSchemaDescriptor& schema, OwnedWireBytes bytes) noexcept
    {
        if (!state_) return cxx::unexpected(RenderError{kTransportStale});
        OperationId operation;
        {
            std::lock_guard lock(state_->reply_mutex);
            operation = state_->cells.find(id_)->operation;
        }
        const auto& expected = state_->route(operation)->descriptor.reply;
        auto checked = expected ? compatibleDataSchemas(*expected, schema) :
            RenderResult<void>(cxx::unexpected(RenderError{kTransportContract}));
        if (checked)
        {
            const auto limit = state_->capacity.reply_bytes;
            checked = validateWire({schema, bytes, {}}, {limit, limit});
        }
        if (!checked)
        {
            static_cast<void>(fail(checked.error()));
            return cxx::unexpected(checked.error());
        }
        return state_->finish(id_, EOperationOutcome::SUCCESS, {}, std::move(bytes));
    }

    RenderResult<void> ReplyPromise::fail(RenderError error) noexcept
    {
        if (!state_) return cxx::unexpected(RenderError{kTransportStale});
        if (!error.type) error = RenderError{kTransportContract};
        return state_->finish(id_, EOperationOutcome::FAILED, error);
    }

    RenderResult<void> ReplyPromise::cancel() noexcept
    {
        if (!state_) return cxx::unexpected(RenderError{kTransportStale});
        return state_->finish(id_, EOperationOutcome::CANCELLED, RenderError{kTransportCancelled});
    }
}
