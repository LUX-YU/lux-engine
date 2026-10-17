#include <lux/engine/platform/ipc/IpcCore.hpp>

#include <exception>
#include <cstring>
#include <string>
#ifdef LUX_IPC_TESTING
#include "TestHooks.hpp"
#include "Allocation.hpp"
#endif

namespace lux::platform::ipc
{
    namespace
    {
        static_assert(std::atomic<void*>::is_always_lock_free);
        static_assert(std::atomic<std::size_t>::is_always_lock_free);
        constexpr auto max_role_uses = std::size_t{1} << (sizeof(std::size_t) * 8 - 1);
        enum class EState { WAITING, OPEN, CLOSED };
        static_assert(std::atomic<EState>::is_always_lock_free);
        std::atomic<std::size_t> channel_count{0};

#ifdef LUX_IPC_TESTING
        std::atomic<std::size_t> link_count{0}, ring_count{0}, allocations{0};
        std::atomic<std::size_t> segment_count{0}, segment_reuses{0}, segment_allocation_failures{0};
        std::atomic<testing::Hook> test_hook{nullptr};
        void point(testing::EPoint value) noexcept
        {
            if (auto hook = test_hook.load()) hook(value);
        }
#endif

        void require(bool condition) noexcept
        {
            if (!condition) std::terminate();
        }
    }

    namespace
    {
        // Internal storage of one SPSC direction, uniquely owned by Link. No node is freed while roles live.
        class MessageQueue final
        {
            struct alignas(64) Segment final
            {
                alignas(64) std::atomic<std::size_t> read{0};
                alignas(64) std::atomic<std::size_t> write{0};
                std::atomic<Segment*> next{nullptr};
                Segment* owned_next = nullptr;
                Segment* recycled_next = nullptr;
            };

        public:
            explicit MessageQueue(std::size_t capacity) noexcept : capacity_(capacity)
            {
                tail_ = head_ = allocate();
            }

            ~MessageQueue() noexcept
            {
                while (owned_)
                {
                    auto* segment = owned_;
                    owned_ = segment->owned_next;
                    const auto end = segment->write.load(std::memory_order_relaxed);
                    for (auto read = segment->read.load(std::memory_order_relaxed); read != end; ++read)
                        std::destroy_at(std::launder(slot(segment, read)));
                    std::destroy_at(segment);
                    ::operator delete(segment, std::align_val_t{alignof(Segment)});
#ifdef LUX_IPC_TESTING
                    --segment_count;
#endif
                }
            }

            MessageQueue(const MessageQueue&) = delete;
            MessageQueue& operator=(const MessageQueue&) = delete;
            [[nodiscard]] static bool validCapacity(std::size_t value) noexcept
            {
                const bool is_representable = value != 0 && value <= (SIZE_MAX - sizeof(Segment)) / sizeof(IpcMessage);
                return is_representable && (value & (value - 1)) == 0;
            }

            [[nodiscard]] bool valid() const noexcept { return tail_ != nullptr; }

            [[nodiscard]] bool tryPush(IpcMessage& input) noexcept
            {
                const auto write = tail_->write.load(std::memory_order_relaxed);
                const auto read = tail_->read.load(std::memory_order_acquire);
                if (write - read < capacity_)
                {
                    std::construct_at(slot(tail_, write), std::move(input));
                    tail_->write.store(write + 1, std::memory_order_release);
                    return true;
                }
                auto* next = acquireSegment();
                if (!next) return false;
                std::construct_at(slot(next, 0), std::move(input));
                next->write.store(1, std::memory_order_relaxed);
                tail_->next.store(next, std::memory_order_release);
                tail_ = next;
                return true;
            }

            [[nodiscard]] bool tryPop(IpcMessage& output) noexcept
            {
                for (;;)
                {
                    const auto read = head_->read.load(std::memory_order_relaxed);
                    const auto write = head_->write.load(std::memory_order_acquire);
                    if (read != write)
                    {
                        auto* value = std::launder(slot(head_, read));
                        output = std::move(*value);
                        std::destroy_at(value);
                        head_->read.store(read + 1, std::memory_order_release);
                        return true;
                    }
                    auto* next = head_->next.load(std::memory_order_acquire);
                    if (!next) return false;
                    // Reading next may observe publication after the first write observation.
                    // All values of the old segment precede next's release publication.
                    if (head_->write.load(std::memory_order_acquire) != read) continue;
                    auto* exited = head_;
                    head_ = next;
#ifdef LUX_IPC_TESTING
                    point(testing::EPoint::SEGMENT_RECYCLE);
#endif
                    auto* recycled = recycled_.load(std::memory_order_relaxed);
                    do { exited->recycled_next = recycled; }
                    while (!recycled_.compare_exchange_weak(recycled, exited,
                        std::memory_order_release, std::memory_order_relaxed));
                    // No access to exited after release: producer may immediately reuse it.
                }
            }

        private:
            [[nodiscard]] IpcMessage* slot(Segment* segment, std::size_t position) const noexcept
            {
                auto* storage = reinterpret_cast<std::byte*>(segment) + sizeof(Segment);
                return reinterpret_cast<IpcMessage*>(storage + (position & (capacity_ - 1)) * sizeof(IpcMessage));
            }

            [[nodiscard]] Segment* allocate() noexcept
            {
#ifdef LUX_IPC_TESTING
                auto remaining = segment_allocation_failures.load(std::memory_order_relaxed);
                while (remaining)
                {
                    if (segment_allocation_failures.compare_exchange_weak(remaining, remaining - 1)) return nullptr;
                }
#endif
                if (capacity_ > (SIZE_MAX - sizeof(Segment)) / sizeof(IpcMessage)) return nullptr;
                void* storage = ::operator new(sizeof(Segment) + capacity_ * sizeof(IpcMessage),
                    std::align_val_t{alignof(Segment)}, std::nothrow);
                if (!storage) return nullptr;
                auto* segment = std::construct_at(static_cast<Segment*>(storage));
                segment->owned_next = owned_;
                owned_ = segment;
#ifdef LUX_IPC_TESTING
                ++segment_count;
                ++allocations;
#endif
                return segment;
            }

            [[nodiscard]] Segment* acquireSegment() noexcept
            {
                if (!available_) available_ = recycled_.exchange(nullptr, std::memory_order_acquire);
                if (!available_) return allocate();
                auto* segment = available_;
                available_ = segment->recycled_next;
                segment->next.store(nullptr, std::memory_order_relaxed);
                segment->read.store(0, std::memory_order_relaxed);
                segment->write.store(0, std::memory_order_relaxed);
#ifdef LUX_IPC_TESTING
                ++segment_reuses;
#endif
                return segment;
            }

            const std::size_t capacity_;
            Segment* owned_ = nullptr; // producer-only immutable ownership chain
            alignas(64) Segment* tail_ = nullptr;
            Segment* available_ = nullptr; // producer-only recycling batch
            alignas(64) Segment* head_ = nullptr; // consumer only
            std::atomic<Segment*> recycled_{nullptr};
        };
    }

    namespace detail
    {
        struct Link final
        {
            explicit Link(IpcQueueConfig config) noexcept
                : requests(config.request_segment_size), replies(config.reply_segment_size)
            {
#ifdef LUX_IPC_TESTING
                ++link_count;
                ++ring_count;
                ++allocations; // Link; queues account for their real segment allocations separately.
#endif
            }

            ~Link() noexcept
            {
#ifdef LUX_IPC_TESTING
                --link_count;
                --ring_count;
#endif
            }
            MessageQueue requests;
            MessageQueue replies;
            std::atomic<bool> open{true};
            Link* owned_next = nullptr; // immutable after publication, Channel's unique ownership list
            Link* next = nullptr; // pending then responder-only circular view, never owns storage
        };

        Link* sentinel() noexcept { return reinterpret_cast<Link*>(std::uintptr_t{1}); }

        struct Name final
        {
            explicit Name(std::string_view value) : text(value) {}
            const std::string text;
            std::atomic<Name*> next{nullptr};
            std::atomic<Channel*> current{nullptr};
            std::atomic<Channel*> retired{nullptr};
            std::atomic<std::size_t> readers{0};
        };

        struct Channel final
        {
            explicit Channel(Name* value) noexcept : name(value)
            {
                ++channel_count;
#ifdef LUX_IPC_TESTING
                ++allocations;
#endif
            }
            Name* name;
            std::atomic<std::size_t> role_uses{1};
            std::atomic<EState> state{EState::WAITING};
            std::atomic<Link*> owned{nullptr};
            std::atomic<Link*> pending{nullptr};
            std::size_t borrowed_requests = 0; // responder-only precondition counter; not lifetime ownership
            Channel* retired_next = nullptr;
        };
    }

    namespace
    {
        using namespace detail;

        void destroyChannel(Channel* channel) noexcept
        {
            auto* link = channel->owned.load();
            while (link)
            {
                auto* next = link->owned_next;
                delete link;
                link = next;
            }
            --channel_count;
            delete channel;
        }

        void collect(Name* name) noexcept
        {
            for (;;)
            {
                auto* batch = name->retired.exchange(nullptr);
                if (!batch) return;
                if (name->readers.load() == 0)
                {
                    while (batch)
                    {
                        auto* next = batch->retired_next;
                        destroyChannel(batch);
                        batch = next;
                    }
                    return;
                }
                auto* tail = batch;
                while (tail->retired_next) tail = tail->retired_next;
#ifdef LUX_IPC_TESTING
                point(testing::EPoint::RETIRE_REQUEUE);
#endif
                auto* head = name->retired.load();
                do { tail->retired_next = head; } while (!name->retired.compare_exchange_weak(head, batch));
                // Recheck AFTER republication: last reader may have left while
                // this collector exclusively held the detached batch.
                if (name->readers.load() != 0) return;
            }
        }

        void leaveReader(Name* name) noexcept
        {
            if (name->readers.fetch_sub(1) == 1) collect(name);
        }

        void releaseChannel(Channel* channel) noexcept
        {
            if (channel->role_uses.fetch_sub(1) != 1) return;
            auto* name = channel->name;
            auto* expected = channel;
            name->current.compare_exchange_strong(expected, nullptr);
            auto* head = name->retired.load();
            do { channel->retired_next = head; } while (!name->retired.compare_exchange_weak(head, channel));
#ifdef LUX_IPC_TESTING
            point(testing::EPoint::RETIRE_PUBLISHED);
#endif
            collect(name);
        }

        Channel* acquire(Name* name) noexcept
        {
            for (;;)
            {
                name->readers.fetch_add(1);
                auto* channel = name->current.load();
#ifdef LUX_IPC_TESTING
                point(testing::EPoint::CHANNEL_READ);
#endif
                if (!channel)
                {
                    auto* candidate = new Channel(name);
                    if (name->current.compare_exchange_strong(channel, candidate))
                    {
                        leaveReader(name);
                        return candidate;
                    }
                    destroyChannel(candidate);
                }
                auto count = channel->role_uses.load();
                while (count != 0)
                {
                    require(count < max_role_uses);
                    if (channel->role_uses.compare_exchange_weak(count, count + 1)) break;
                }
                if (count != 0 && channel->state.load() != EState::CLOSED)
                {
                    leaveReader(name);
                    return channel;
                }
                auto* expected = channel;
                name->current.compare_exchange_strong(expected, nullptr);
                if (count != 0) releaseChannel(channel);
                leaveReader(name);
            }
        }

    }

    IpcMessage::~IpcMessage() noexcept { reset(); }

    IpcMessage::IpcMessage(IpcMessage&& other) noexcept { *this = std::move(other); }

    IpcMessage& IpcMessage::operator=(IpcMessage&& other) noexcept
    {
        if (this == &other) return *this;
        reset();
        metadata_ = other.metadata_;
        operations_ = std::exchange(other.operations_, nullptr);
#ifdef LUX_IPC_TESTING
        testing::recordMove();
#endif
        if (operations_) operations_->move(object_, other.object_);
        return *this;
    }

    void IpcMessage::reset() noexcept
    {
        const auto* operations = std::exchange(operations_, nullptr);
        if (operations) operations->destroy(object_);
    }

    IpcCore& IpcCore::instance() noexcept
    {
        static IpcCore core;
        return core;
    }

    detail::Name* IpcCore::findName(std::string_view value) noexcept
    {
        std::unique_ptr<detail::Name> candidate;
        for (;;)
        {
            auto* location = &names_;
            auto* current = location->load();
            while (current && current->text < value)
            {
                location = &current->next;
                current = location->load();
            }
            if (current && current->text == value) return current;
            if (!candidate) candidate = std::make_unique<detail::Name>(value);
            candidate->next.store(current);
            if (location->compare_exchange_strong(current, candidate.get())) return candidate.release();
        }
    }

    IpcCore::~IpcCore() noexcept
    {
        require(channel_count.load() == 0); // includes detached but still pinned epochs
        auto* name = names_.load();
        while (name)
        {
            require(name->current.load() == nullptr && name->readers.load() == 0);
            collect(name);
            auto* next = name->next.load();
            delete name;
            name = next;
        }
    }

    Result<IpcRequester> IpcCore::createRequester(std::string_view name, IpcQueueConfig config) noexcept
    {
        const bool is_invalid_name = name.empty() || name.find('\0') != std::string_view::npos;
        const bool is_invalid_capacity = !MessageQueue::validCapacity(config.request_segment_size) ||
            !MessageQueue::validCapacity(config.reply_segment_size);
        if (is_invalid_name || is_invalid_capacity) return cxx::unexpected(EIpcError::INVALID_ARGUMENT);
        IpcRequester result;
        result.channel_ = acquire(findName(name));
        result.link_ = new detail::Link(config);
        if (!result.link_->requests.valid() || !result.link_->replies.valid())
        {
            delete std::exchange(result.link_, nullptr);
            releaseChannel(std::exchange(result.channel_, nullptr));
            return cxx::unexpected(EIpcError::WOULD_BLOCK);
        }
        auto* owned = result.channel_->owned.load();
        do { result.link_->owned_next = owned; }
        while (!result.channel_->owned.compare_exchange_weak(owned, result.link_));
        auto* head = result.channel_->pending.load();
        for (;;)
        {
            if (head == detail::sentinel())
            {
                result.reset();
                return cxx::unexpected(EIpcError::CLOSED);
            }
            result.link_->next = head;
            if (result.channel_->pending.compare_exchange_weak(head, result.link_)) return result;
        }
    }

    Result<IpcResponder> IpcCore::createResponder(std::string_view name) noexcept
    {
        if (name.empty() || name.find('\0') != std::string_view::npos)
            return cxx::unexpected(EIpcError::INVALID_ARGUMENT);
        for (;;)
        {
            auto* channel = acquire(findName(name));
            auto expected = EState::WAITING;
            if (channel->state.compare_exchange_strong(expected, EState::OPEN))
            {
                IpcResponder result;
                result.channel_ = channel;
                return result;
            }
            releaseChannel(channel);
            if (expected == EState::OPEN) return cxx::unexpected(EIpcError::ALREADY_HAS_RESPONDER);
        }
    }

    IpcRequester::~IpcRequester() noexcept { reset(); }

    IpcRequester::IpcRequester(IpcRequester&& other) noexcept
        : channel_(std::exchange(other.channel_, nullptr)), link_(std::exchange(other.link_, nullptr)) {}

    IpcRequester& IpcRequester::operator=(IpcRequester&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            channel_ = std::exchange(other.channel_, nullptr);
            link_ = std::exchange(other.link_, nullptr);
        }
        return *this;
    }

    void IpcRequester::reset() noexcept
    {
        if (!link_) return;
        std::exchange(link_, nullptr)->open.store(false);
        releaseChannel(std::exchange(channel_, nullptr));
    }

    bool IpcRequester::ready() const noexcept
    {
        return channel_ && channel_->state.load() == EState::OPEN;
    }

    Result<void> IpcRequester::trySend(IpcMessage& message) noexcept
    {
        if (!channel_) return cxx::unexpected(EIpcError::CLOSED);
        const auto state = channel_->state.load();
        if (state == EState::WAITING) return cxx::unexpected(EIpcError::NOT_READY);
        if (state == EState::CLOSED) return cxx::unexpected(EIpcError::CLOSED);
        const auto result = link_->requests.tryPush(message);
        if (result) return {};
        return cxx::unexpected(EIpcError::WOULD_BLOCK);
    }

    EIpcReceive IpcRequester::tryReceive(IpcMessage& output) noexcept
    {
        if (!channel_) return EIpcReceive::CLOSED;
        auto result = link_->replies.tryPop(output);
#ifdef LUX_IPC_TESTING
        if (!result) point(testing::EPoint::REPLY_EMPTY);
#endif
        const bool is_closed = channel_->state.load() == EState::CLOSED;
        // Recheck after CLOSED: a final publication may follow the first empty observation.
        if (!result && is_closed) result = link_->replies.tryPop(output);
        if (result)
        {
            return EIpcReceive::RECEIVED;
        }
        return is_closed ? EIpcReceive::CLOSED : EIpcReceive::EMPTY;
    }

    IpcResponder::~IpcResponder() noexcept { reset(); }

    IpcResponder::IpcResponder(IpcResponder&& other) noexcept
        : channel_(std::exchange(other.channel_, nullptr)), pending_(std::exchange(other.pending_, nullptr)),
          cursor_(std::exchange(other.cursor_, nullptr)) {}

    IpcResponder& IpcResponder::operator=(IpcResponder&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            channel_ = std::exchange(other.channel_, nullptr);
            pending_ = std::exchange(other.pending_, nullptr);
            cursor_ = std::exchange(other.cursor_, nullptr);
        }
        return *this;
    }

    void IpcResponder::reset() noexcept
    {
        if (!channel_) return;
        require(channel_->borrowed_requests == 0);
        channel_->state.store(EState::CLOSED);
        auto* expected = channel_;
        channel_->name->current.compare_exchange_strong(expected, nullptr);
        channel_->pending.exchange(detail::sentinel());
        pending_ = nullptr;
        cursor_ = nullptr;
        releaseChannel(std::exchange(channel_, nullptr));
    }

    EIpcReceive IpcResponder::tryReceive(ReceivedRequest& output) noexcept
    {
        if (!channel_) return EIpcReceive::CLOSED;
        require(!output.active());
        if (!pending_ && channel_->pending.load() != nullptr) pending_ = channel_->pending.exchange(nullptr);
        if (pending_)
        {
            auto* link = pending_;
            pending_ = link->next;
            if (cursor_)
            {
                link->next = cursor_->next;
                cursor_->next = link;
                cursor_ = link; // append behind the existing next-to-visit cursor
            }
            else
            {
                link->next = link;
                cursor_ = link;
            }
        }
        for (std::size_t visited = 0; cursor_ && visited < 8; ++visited)
        {
            auto* link = cursor_->next;
            if (!link->open.load())
            {
                if (link == cursor_) cursor_ = nullptr;
                else cursor_->next = link->next;
                continue;
            }
            cursor_ = link;
#ifdef LUX_IPC_TESTING
            point(testing::EPoint::RING_READ);
#endif
            if (link->requests.tryPop(output.message_))
            {
                ++channel_->borrowed_requests;
                output.channel_ = channel_;
                output.link_ = link;
                return EIpcReceive::RECEIVED;
            }
        }
        return EIpcReceive::EMPTY;
    }

    Result<void> IpcResponder::tryReply(ReceivedRequest& request, IpcMessage& reply) noexcept
    {
        if (!request.link_) return cxx::unexpected(EIpcError::INVALID_SOURCE);
        if (!channel_) return cxx::unexpected(EIpcError::CLOSED);
        if (request.channel_ != channel_) return cxx::unexpected(EIpcError::INVALID_SOURCE);
        auto* link = request.link_;
        if (!link->open.load()) return cxx::unexpected(EIpcError::CLOSED);
#ifdef LUX_IPC_TESTING
        point(testing::EPoint::REPLY_PUBLISH);
#endif
        const auto result = link->replies.tryPush(reply);
        if (!result) return cxx::unexpected(EIpcError::WOULD_BLOCK);
#ifdef LUX_IPC_TESTING
        point(testing::EPoint::REPLY_PUBLISHED);
#endif
        request.message_.reset();
        --channel_->borrowed_requests;
        request.link_ = nullptr;
        request.channel_ = nullptr;
        return {};
    }

    ReceivedRequest::~ReceivedRequest() noexcept { reset(); }

    ReceivedRequest::ReceivedRequest(ReceivedRequest&& other) noexcept
        : channel_(std::exchange(other.channel_, nullptr)), link_(std::exchange(other.link_, nullptr)),
          message_(std::move(other.message_)) {}

    ReceivedRequest& ReceivedRequest::operator=(ReceivedRequest&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            channel_ = std::exchange(other.channel_, nullptr);
            link_ = std::exchange(other.link_, nullptr);
            message_ = std::move(other.message_);
        }
        return *this;
    }

    void ReceivedRequest::reset() noexcept
    {
        if (!link_) return;
        message_.reset(); // disconnected disposal is a documented cold operation
        --channel_->borrowed_requests;
        link_ = nullptr;
        channel_ = nullptr;
    }

#ifdef LUX_IPC_TESTING
    namespace testing
    {
        thread_local bool moves_enabled = false;
        thread_local MoveCounts move_counts{};

        void recordMove() noexcept
        {
            if (moves_enabled) ++move_counts.moves;
        }

        void startMoves() noexcept { move_counts = {}; moves_enabled = true; }

        MoveCounts stopMoves() noexcept { moves_enabled = false; return move_counts; }
    }

    testing::Counts testing::counts() noexcept
    {
        return {channel_count.load(), link_count.load(), ring_count.load(), allocations.load(),
            segment_count.load(), segment_reuses.load()};
    }

    void testing::failSegmentAllocations(std::size_t count) noexcept
    {
        segment_allocation_failures.store(count);
    }

    void testing::setHook(Hook hook) noexcept { test_hook.store(hook); }

    void testing::startAllocations() noexcept { ipc_allocation::start(); }

    std::size_t testing::stopAllocations() noexcept { return ipc_allocation::stop(); }
#endif
}
