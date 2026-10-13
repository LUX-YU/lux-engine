#include <lux/engine/render/transport/State.hpp>

#include <utility>

#include <algorithm>
#include <lux/cxx/core/CheckedArithmetic.hpp>

namespace lux::render
{
    namespace
    {
        bool validDelivery(EDelivery delivery) noexcept
        {
            return delivery == EDelivery::COMMAND ||
                delivery == EDelivery::PRIORITY_COMMAND || delivery == EDelivery::BULK;
        }

        struct Admission final
        {
            cxx::AdmissionTicket<> ticket;
            detail::TransportState& state;
            ~Admission() { ticket.release(); state.notify(); }
        };

        struct Reader final
        {
            detail::TransportState& state;
            explicit Reader(detail::TransportState& value) noexcept : state(value)
            {
                state.readers.fetch_add(1, std::memory_order_acq_rel);
            }

            ~Reader()
            {
                state.readers.fetch_sub(1, std::memory_order_release);
                if (state.admission.closed()) state.notify();
            }
        };
    }

    CommandBatch::CommandBatch(TransportCapacity capacity) noexcept
        : records_(capacity.records), byte_limit_((std::max)(capacity.command_bytes, capacity.bulk_bytes)),
          attachment_limit_(capacity.attachments)
    {
        for (auto& record : records_) record.attachments.reserve(capacity.attachments);
    }

    void CommandBatch::clear() noexcept
    {
        for (std::size_t i = 0; i < size_; ++i)
        {
            records_[i].bytes = {};
            records_[i].attachments.clear();
            records_[i].binding = {};
            records_[i].reserved = {};
        }
        size_ = 0;
    }

    CommandBatch::CommandBatch(CommandBatch&& other) noexcept
        : records_(std::move(other.records_)), size_(std::exchange(other.size_, 0)),
          byte_limit_(other.byte_limit_), attachment_limit_(other.attachment_limit_) {}

    CommandBatch& CommandBatch::operator=(CommandBatch&& other) noexcept
    {
        if (this == &other) return *this;
        clear();
        records_ = std::move(other.records_);
        size_ = std::exchange(other.size_, 0);
        byte_limit_ = other.byte_limit_;
        attachment_limit_ = other.attachment_limit_;
        return *this;
    }

    RenderResult<void> CommandBatch::appendErased(
        detail::Binding binding, OwnedWireBytes bytes, std::span<const OwnedWireBytes> attachments
    ) noexcept
    {
        const bool is_full = size_ == records_.size() || attachments.size() > attachment_limit_;
        if (is_full) return cxx::unexpected(RenderError{kTransportCapacity});
        auto& record = records_[size_++];
        record.binding = std::move(binding);
        record.bytes = std::move(bytes);
        record.attachments.assign(attachments.begin(), attachments.end());
        return {};
    }

    CommandBatch TransportSender::makeBatch() const noexcept { return CommandBatch(state_->capacity); }

    RenderResult<detail::Binding> TransportSender::bindErased(
        const OperationDescriptor& descriptor, const OperationDescriptor& typed
    ) const noexcept
    {
        if (stopping()) return cxx::unexpected(RenderError{kTransportStopped});
        return detail::bindOperation(*state_->routes, descriptor, typed);
    }

    BatchRequest TransportSender::makeRequests() const noexcept { return BatchRequest(state_->capacity.records); }

    RenderResult<void>
    TransportSender::submit(CommandBatch& batch, BatchRequest& output, EDelivery delivery) const noexcept
    {
        if (state_->admission.closed()) return cxx::unexpected(RenderError{kTransportStopped});
        if (!validDelivery(delivery)) return cxx::unexpected(RenderError{kTransportContract});
        const bool is_invalid_batch = batch.size_ == 0 || batch.size_ > state_->capacity.records ||
            !output.requests_.empty() || batch.size_ > output.requests_.capacity();
        const bool is_invalid_bulk = delivery == EDelivery::BULK && batch.size_ != 1;
        if (is_invalid_batch || is_invalid_bulk) return cxx::unexpected(RenderError{kTransportContract});
        const bool is_wrong_thread = delivery != EDelivery::BULK && std::this_thread::get_id() != state_->producer;
        if (is_wrong_thread) return cxx::unexpected(RenderError{kTransportThread});

        const auto limit = delivery == EDelivery::BULK ?
                state_->capacity.bulk_bytes : state_->capacity.command_bytes;
        std::size_t total = 0;
        for (std::size_t i = 0; i < batch.size_; ++i)
        {
            auto& record = batch.records_[i];
            if (record.binding.owner != state_->routes->identity) return cxx::unexpected(RenderError{kTransportOwner});
            const auto* entry = state_->route(record.binding.operation);
            if (!entry) return cxx::unexpected(RenderError{kTransportStale});
            if (record.attachments.size() > state_->capacity.attachments)
            {
                return cxx::unexpected(RenderError{kTransportCapacity});
            }
            auto checked = validateWire({entry->descriptor.payload, record.bytes, record.attachments}, {limit, limit});
            if (!checked) return checked;
            auto add = [&](std::size_t bytes) noexcept
            {
                auto sum = cxx::checkedAdd(total, bytes);
                if (!sum || *sum > limit) return false;
                total = *sum;
                return true;
            };
            if (!add(record.bytes.size())) return cxx::unexpected(RenderError{kTransportBudget});
            for (const auto& owner : record.attachments)
            {
                const bool is_missing_owner = owner.size() != 0 && owner.data() == nullptr;
                if (is_missing_owner) return cxx::unexpected(RenderError{kInvalidDataReference});
                if (!add(owner.size())) return cxx::unexpected(RenderError{kTransportBudget});
            }
        }

        auto acquired = state_->admission.tryAcquire();
        if (!acquired) return cxx::unexpected(RenderError{kTransportStopped});
        Admission admission{std::move(*acquired), *state_};
        std::size_t reserved = 0;
        auto rollback = [&]() noexcept
        {
            for (std::size_t i = 0; i < reserved; ++i)
            {
                state_->rollback(batch.records_[i].reserved);
                batch.records_[i].reserved = {};
            }
        };
        for (; reserved < batch.size_; ++reserved)
        {
            auto& record = batch.records_[reserved];
            auto id = state_->reserve(
                record.binding.operation, record.bytes, record.attachments, delivery == EDelivery::BULK
            );
            if (!id) { rollback(); return cxx::unexpected(id.error()); }
            record.reserved = *id;
        }

        RenderError publication_error{};
        if (delivery == EDelivery::BULK)
        {
            std::unique_lock lock(state_->bulk_mutex, std::try_to_lock);
            if (!lock.owns_lock()) publication_error = RenderError{kTransportBusy};
            else
            {
                const auto count = state_->bulk_count.load(std::memory_order_relaxed);
                if (count == state_->bulk.size()) publication_error = RenderError{kTransportCapacity};
                else
                {
                    state_->bulk[(state_->bulk_head + count) % state_->bulk.size()] = batch.records_[0].reserved;
                    state_->bulk_count.store(count + 1, std::memory_order_release);
                }
            }
        }
        else
        {
            auto& ring = delivery == EDelivery::COMMAND ? state_->commands : state_->priority;
            auto* slot = ring.tryBeginWrite();
            if (!slot) publication_error = RenderError{kTransportCapacity};
            else
            {
                for (std::size_t i = 0; i < batch.size_; ++i) slot->push_back(batch.records_[i].reserved);
                if (!ring.publishWrite())
                {
                    slot->clear();
                    publication_error = RenderError{kTransportCapacity};
                }
            }
        }
        if (publication_error.type) { rollback(); return cxx::unexpected(publication_error); }
        for (std::size_t i = 0; i < batch.size_; ++i)
        {
            output.requests_.push_back(RenderRequest(state_, batch.records_[i].reserved));
        }
        batch.clear();
        return {};
    }

    bool TransportSender::stopping() const noexcept { return state_->admission.closed(); }

    std::size_t TransportSender::bulkBytes() const noexcept { return state_->budget.currentBytes(); }

    std::uint64_t TransportSender::wakeEpoch() const noexcept
    {
        return state_->wake_epoch.load(std::memory_order_acquire);
    }

    bool TransportSender::waitUntil(
        std::uint64_t observed, std::chrono::steady_clock::time_point deadline, std::stop_token stop
    ) const noexcept
    {
        std::unique_lock lock(state_->wake_mutex);
        return state_->wake.wait_until(lock, stop, deadline, [&]() noexcept
        {
            return state_->admission.closed() || state_->wake_epoch.load(std::memory_order_acquire) != observed;
        });
    }

    RenderResult<std::unique_ptr<RenderTransport>>
    RenderTransport::create(RenderRouteTable&& table, TransportCapacity capacity) noexcept
    {
        const bool is_invalid_spsc = capacity.pending_commands == 0 || capacity.pending_commands > 3 ||
            capacity.pending_priority == 0 || capacity.pending_priority > 3;
        const bool is_invalid_capacity = capacity.records == 0 || capacity.pending_bulk == 0 || capacity.replies == 0 ||
            capacity.command_bytes == 0 || capacity.bulk_bytes == 0 || capacity.reply_bytes == 0 ||
            capacity.replies > UINT32_MAX - 256;
        if (is_invalid_spsc || is_invalid_capacity) return cxx::unexpected(RenderError{kTransportCapacity});
        if (!table.routes_ || table.routes_->frozen) return cxx::unexpected(RenderError{kTransportContract});
        for (const auto& entry : table.routes_->entries)
        {
            if (entry->descriptor.reply && entry->descriptor.reply->fixed_size > capacity.reply_bytes)
            {
                return cxx::unexpected(RenderError{kTransportCapacity});
            }
        }
        table.routes_->frozen = true;
        auto state = std::make_shared<detail::TransportState>(std::move(table.routes_), capacity);
        return std::unique_ptr<RenderTransport>(new RenderTransport(std::move(state)));
    }

    RenderTransport::~RenderTransport()
    {
        state_->stop();
        const bool has_callers = state_->admission.inFlight() != 0 ||
            state_->readers.load(std::memory_order_acquire) != 0;
        if (has_callers) std::terminate();
        // Quiescent destruction may run on the host thread after consumers joined.
        for (auto* ring : {&state_->commands, &state_->priority})
        {
            while (ring->tryAcquireRead())
            {
                for (auto id : ring->currentRead()) state_->releaseWork(id);
                ring->currentRead().clear();
            }
        }
        while (state_->bulk_count.load(std::memory_order_relaxed) != 0)
        {
            auto id = state_->bulk[state_->bulk_head];
            state_->bulk_head = (state_->bulk_head + 1) % state_->bulk.size();
            state_->bulk_count.fetch_sub(1, std::memory_order_release);
            state_->releaseWork(id);
        }
    }

    RenderResult<bool> RenderTransport::poll(EDelivery delivery) noexcept
    {
        if (!validDelivery(delivery)) return cxx::unexpected(RenderError{kTransportContract});
        Reader reader(*state_);
        auto dispatch = [&](RequestId id) noexcept
        {
            ReplyPromise promise(state_, id);
            if (state_->admission.closed()) return;
            OperationId operation;
            {
                std::lock_guard lock(state_->reply_mutex);
                operation = state_->cells.find(id)->operation;
            }
            const auto* entry = state_->route(operation);
            auto input = promise.input();
            if (!input) { static_cast<void>(promise.fail(input.error())); return; }
            const auto limit = delivery == EDelivery::BULK ?
                state_->capacity.bulk_bytes : state_->capacity.command_bytes;
            auto checked = validateWire(*input, {limit, limit});
            if (!checked) { static_cast<void>(promise.fail(checked.error())); return; }
            entry->handler(entry->context, std::move(promise));
        };
        if (delivery == EDelivery::BULK)
        {
            RequestId id;
            {
                std::unique_lock lock(state_->bulk_mutex, std::try_to_lock);
                if (!lock.owns_lock()) return cxx::unexpected(RenderError{kTransportBusy});
                const auto count = state_->bulk_count.load(std::memory_order_relaxed);
                if (count == 0) return false;
                id = state_->bulk[state_->bulk_head];
                state_->bulk_head = (state_->bulk_head + 1) % state_->bulk.size();
                state_->bulk_count.store(count - 1, std::memory_order_release);
            }
            dispatch(id);
            return true;
        }
        {
            std::lock_guard lock(state_->consumer_mutex);
            const auto current = std::this_thread::get_id();
            if (state_->consumer == std::thread::id{}) state_->consumer = current;
            if (state_->consumer != current) return cxx::unexpected(RenderError{kTransportThread});
            if (state_->spsc_reading) return cxx::unexpected(RenderError{kTransportBusy});
            state_->spsc_reading = true;
        }
        auto& ring = delivery == EDelivery::COMMAND ? state_->commands : state_->priority;
        const bool consumed = ring.tryAcquireRead();
        if (consumed)
        {
            for (auto id : ring.currentRead()) dispatch(id);
            ring.currentRead().clear();
        }
        {
            std::lock_guard lock(state_->consumer_mutex);
            state_->spsc_reading = false;
        }
        return consumed;
    }

    void RenderTransport::requestStop() noexcept { state_->stop(); }

    bool RenderTransport::drained() const noexcept
    {
        return state_->admission.closed() && state_->admission.inFlight() == 0 &&
            state_->commands.pendingFrames() == 0 && state_->priority.pendingFrames() == 0 &&
            state_->bulk_count.load(std::memory_order_acquire) == 0 &&
            state_->readers.load(std::memory_order_acquire) == 0 &&
            state_->work_count.load(std::memory_order_acquire) == 0;
    }
}
