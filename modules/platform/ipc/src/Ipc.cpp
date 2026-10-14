#include <lux/engine/platform/ipc/IpcCore.hpp>

#include <algorithm>
#include <exception>
#include <limits>
#include <string>
#include <lux/cxx/concurrent/LockFreeQueue.hpp>
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
        constexpr auto closed_bit = std::size_t{1} << (sizeof(std::size_t) * 8 - 1);
        enum class EState { WAITING, OPEN, CLOSED };
        static_assert(std::atomic<EState>::is_always_lock_free);
        std::atomic<std::size_t> channel_count{0};

#ifdef LUX_IPC_TESTING
        std::atomic<std::size_t> link_count{0}, ring_count{0}, allocations{0};
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

    namespace detail
    {
        struct Rings final
        {
            explicit Rings(IpcQueueConfig config) noexcept
                : requests(config.request_capacity), replies(config.reply_capacity) {}
            cxx::SpscLockFreeRingQueue<IpcMessage> requests;
            cxx::SpscLockFreeRingQueue<IpcReply> replies;
        };

        struct Link final
        {
            explicit Link(IpcQueueConfig config) noexcept
                : rings(new Rings(config)), reply_capacity(config.reply_capacity)
            {
#ifdef LUX_IPC_TESTING
                ++link_count;
                ++ring_count;
                allocations.fetch_add(4); // Link, Rings, two queue storage allocations.
#endif
            }
            ~Link() noexcept
            {
                require(rings == nullptr);
#ifdef LUX_IPC_TESTING
                --link_count;
#endif
            }
            std::atomic<std::size_t> refs{2}; // requester and pending/view
            std::atomic<std::size_t> access{0};
            Rings* rings;
            std::size_t reply_capacity;
            Link* next = nullptr; // pending, then responder's intrusive circular view
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
            std::atomic<std::size_t> refs{1};
            std::atomic<EState> state{EState::WAITING};
            std::atomic<Link*> pending{nullptr};
            Channel* retired_next = nullptr;
        };
    }

    namespace
    {
        using namespace detail;

        void releaseLink(Link* link) noexcept
        {
            if (link->refs.fetch_sub(1) == 1) delete link;
        }

        void releaseList(Link* link) noexcept
        {
            while (link && link != sentinel())
            {
                auto* next = link->next;
                releaseLink(link);
                link = next;
            }
        }

        void destroyChannel(Channel* channel) noexcept
        {
            releaseList(channel->pending.load());
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
            if (channel->refs.fetch_sub(1) != 1) return;
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
                auto count = channel->refs.load();
                while (count != 0)
                {
                    require(count < closed_bit);
                    if (channel->refs.compare_exchange_weak(count, count + 1)) break;
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

        bool enter(Link* link) noexcept
        {
            auto access = link->access.load();
            while (!(access & closed_bit))
            {
                require(access < closed_bit - 1);
                if (link->access.compare_exchange_weak(access, access + 1))
                {
#ifdef LUX_IPC_TESTING
                    point(testing::EPoint::RING_ENTER);
#endif
                    return true;
                }
            }
            return false;
        }

        void leave(Link* link) noexcept
        {
            const auto before = link->access.fetch_sub(1);
            if (before == closed_bit + 1) link->access.notify_all();
        }

        bool validCapacity(std::size_t value) noexcept
        {
            return value != 0 && value <= (std::numeric_limits<std::uint32_t>::max)() / 2 &&
                (value & (value - 1)) == 0;
        }
    }

    IpcMessage::~IpcMessage() noexcept { reset(); }

    IpcMessage::IpcMessage(IpcMessage&& other) noexcept
        : inline_(other.inline_), data_(std::exchange(other.data_, nullptr)),
          owner_(std::exchange(other.owner_, nullptr)), destroy_(std::exchange(other.destroy_, nullptr)),
          size_(std::exchange(other.size_, 0)) {}

    IpcMessage& IpcMessage::operator=(IpcMessage&& other) noexcept
    {
        if (this == &other) return *this;
        reset();
        inline_ = other.inline_;
        data_ = std::exchange(other.data_, nullptr);
        owner_ = std::exchange(other.owner_, nullptr);
        destroy_ = std::exchange(other.destroy_, nullptr);
        size_ = std::exchange(other.size_, 0);
        return *this;
    }

    void IpcMessage::reset() noexcept
    {
        if (owner_) destroy_(owner_);
        owner_ = nullptr;
        data_ = nullptr;
        destroy_ = nullptr;
        size_ = 0;
    }

    std::span<const std::byte> IpcMessage::bytes() const noexcept
    {
        return {owner_ ? data_ : inline_.data(), size_};
    }

    Result<IpcMessage> IpcMessage::copyInline(std::span<const std::byte> bytes) noexcept
    {
        const bool is_oversized = bytes.size() > inline_capacity;
        const bool is_missing_bytes = !bytes.empty() && !bytes.data();
        if (is_oversized || is_missing_bytes) return cxx::unexpected(EIpcError::INVALID_ARGUMENT);
        IpcMessage result;
        std::copy(bytes.begin(), bytes.end(), result.inline_.begin());
        result.size_ = bytes.size();
        return result;
    }

    Result<IpcMessage> IpcMessage::takeBytes(std::unique_ptr<std::byte[]>& owner, std::size_t size) noexcept
    {
        if (!owner) return cxx::unexpected(EIpcError::INVALID_ARGUMENT);
        IpcMessage result;
        result.data_ = owner.get();
        result.owner_ = owner.release();
        result.size_ = size;
        result.destroy_ = [](void* value) noexcept { delete[] static_cast<std::byte*>(value); };
        return result;
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
        const bool is_invalid_capacity = !validCapacity(config.request_capacity) ||
            !validCapacity(config.reply_capacity);
        if (is_invalid_name || is_invalid_capacity) return cxx::unexpected(EIpcError::INVALID_ARGUMENT);
        IpcRequester result;
        result.channel_ = acquire(findName(name));
        result.link_ = new detail::Link(config);
        auto* head = result.channel_->pending.load();
        for (;;)
        {
            if (head == detail::sentinel())
            {
                releaseLink(result.link_); // no pending ownership transfer occurred
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
        : channel_(std::exchange(other.channel_, nullptr)), link_(std::exchange(other.link_, nullptr)),
          outstanding_(std::exchange(other.outstanding_, 0)) {}

    IpcRequester& IpcRequester::operator=(IpcRequester&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            channel_ = std::exchange(other.channel_, nullptr);
            link_ = std::exchange(other.link_, nullptr);
            outstanding_ = std::exchange(other.outstanding_, 0);
        }
        return *this;
    }

    void IpcRequester::reset() noexcept
    {
        if (!link_) return;
        auto* link = std::exchange(link_, nullptr);
        auto access = link->access.fetch_or(closed_bit) | closed_bit;
        while (access != closed_bit)
        {
            link->access.wait(access);
            access = link->access.load();
        }
        delete std::exchange(link->rings, nullptr);
#ifdef LUX_IPC_TESTING
        --ring_count;
#endif
        releaseLink(link);
        releaseChannel(std::exchange(channel_, nullptr));
        outstanding_ = 0;
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
        if (outstanding_ == link_->reply_capacity) return cxx::unexpected(EIpcError::WOULD_BLOCK);
        ++outstanding_;
        const auto result = link_->rings->requests.tryPush(std::move(message));
        if (result == cxx::EQueuePushResult::ACCEPTED) return {};
        --outstanding_;
        return cxx::unexpected(EIpcError::WOULD_BLOCK);
    }

    Result<std::optional<IpcReply>> IpcRequester::tryReceive() noexcept
    {
        if (!channel_) return cxx::unexpected(EIpcError::CLOSED);
        auto result = link_->rings->replies.tryPopValue();
#ifdef LUX_IPC_TESTING
        if (!result.value) point(testing::EPoint::REPLY_EMPTY);
#endif
        const bool is_closed = channel_->state.load() == EState::CLOSED;
        // A producer may publish between the initial empty observation and its
        // CLOSED store. Re-read after acquiring CLOSED before reporting loss.
        if (!result.value && is_closed) result = link_->rings->replies.tryPopValue();
        if (result.value)
        {
            require(outstanding_ != 0);
            --outstanding_;
            return std::move(result.value);
        }
        if (is_closed) return cxx::unexpected(EIpcError::CLOSED);
        return std::optional<IpcReply>{};
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
        channel_->state.store(EState::CLOSED);
        auto* expected = channel_;
        channel_->name->current.compare_exchange_strong(expected, nullptr);
        releaseList(channel_->pending.exchange(detail::sentinel()));
        releaseList(std::exchange(pending_, nullptr));
        if (cursor_)
        {
            auto* head = cursor_->next;
            cursor_->next = nullptr;
            cursor_ = nullptr;
            releaseList(head);
        }
        releaseChannel(std::exchange(channel_, nullptr));
    }

    Result<std::optional<ReceivedRequest>> IpcResponder::tryReceive() noexcept
    {
        if (!channel_) return cxx::unexpected(EIpcError::CLOSED);
        if (!pending_) pending_ = channel_->pending.exchange(nullptr);
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
            if (!enter(link))
            {
                if (link == cursor_) cursor_ = nullptr;
                else cursor_->next = link->next;
                releaseLink(link);
                continue;
            }
            cursor_ = link;
            auto result = link->rings->requests.tryPopValue();
            leave(link);
            if (result.value)
            {
                ReceivedRequest received;
                link->refs.fetch_add(1); // view pin remains held by this responder
                channel_->refs.fetch_add(1);
                received.channel_ = channel_;
                received.link_ = link;
                received.message_ = std::move(*result.value);
                return std::optional<ReceivedRequest>{std::move(received)};
            }
        }
        return std::optional<ReceivedRequest>{};
    }

    Result<void> IpcResponder::tryReply(ReceivedRequest& request, IpcMessage& reply) noexcept
    {
        if (!request.link_) return cxx::unexpected(EIpcError::INVALID_SOURCE);
        if (!channel_) return cxx::unexpected(EIpcError::CLOSED);
        if (request.channel_ != channel_) return cxx::unexpected(EIpcError::INVALID_SOURCE);
        auto* link = request.link_;
        if (!enter(link)) return cxx::unexpected(EIpcError::CLOSED);
        const auto result = link->rings->replies.tryEmplace(
            EIpcDelivery::REPLY, std::move(reply), std::move(request.message_)
        );
        require(result == cxx::EQueuePushResult::ACCEPTED);
        leave(link);
        releaseLink(std::exchange(request.link_, nullptr));
        releaseChannel(std::exchange(request.channel_, nullptr));
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
        if (channel_->state.load() == EState::OPEN && enter(link_))
        {
            const auto result = link_->rings->replies.tryEmplace(
                EIpcDelivery::ABORTED, IpcMessage{}, std::move(message_)
            );
            require(result == cxx::EQueuePushResult::ACCEPTED);
            leave(link_);
        }
        message_.reset(); // disconnected disposal is a documented cold operation
        releaseLink(std::exchange(link_, nullptr));
        releaseChannel(std::exchange(channel_, nullptr));
    }

#ifdef LUX_IPC_TESTING
    testing::Counts testing::counts() noexcept
    {
        return {channel_count.load(), link_count.load(), ring_count.load(), allocations.load()};
    }

    void testing::setHook(Hook hook) noexcept { test_hook.store(hook); }

    void testing::startAllocations() noexcept { ipc_allocation::start(); }

    std::size_t testing::stopAllocations() noexcept { return ipc_allocation::stop(); }
#endif
}
