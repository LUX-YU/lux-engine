#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/object/detail/MessageEnvelope.hpp>

#include <atomic>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <utility>

namespace lux::object::detail
{
#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
    namespace
    {
        std::atomic_size_t inline_message_storage_count{0};
        std::atomic_size_t heap_message_storage_count{0};
    } // namespace

    void recordMessageStorageForTest(bool inline_value) noexcept
    {
        (inline_value ? inline_message_storage_count : heap_message_storage_count)
            .fetch_add(1, std::memory_order_relaxed);
    }

    void resetMessageStorageForTest() noexcept
    {
        inline_message_storage_count.store(0, std::memory_order_relaxed);
        heap_message_storage_count.store(0, std::memory_order_relaxed);
    }

    std::size_t inlineMessageStorageCountForTest() noexcept
    {
        return inline_message_storage_count.load(std::memory_order_relaxed);
    }

    std::size_t heapMessageStorageCountForTest() noexcept
    {
        return heap_message_storage_count.load(std::memory_order_relaxed);
    }
#endif

    struct ObjectMessageQueueState final
    {
        std::mutex mutex;
        std::deque<MessageEnvelope> messages;
        std::thread::id owner{std::this_thread::get_id()};
        bool closed{false};
    };
} // namespace lux::object::detail

namespace lux::object::detail
{
    MessageEnvelope::MessageEnvelope(MessageEnvelope&& other) noexcept
    {
        moveFrom(std::move(other));
    }

    MessageEnvelope& MessageEnvelope::operator=(MessageEnvelope&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            reset();
            moveFrom(std::move(other));
        }
        return *this;
    }

    MessageEnvelope::~MessageEnvelope()
    {
        reset();
    }

    void MessageEnvelope::invoke() noexcept
    {
        if (ops_)
            ops_->invoke(data_);
    }

    void MessageEnvelope::reset() noexcept
    {
        if (ops_)
            ops_->destroy(data_);
        data_ = nullptr;
        ops_ = nullptr;
        inline_ = false;
    }

    void MessageEnvelope::moveFrom(MessageEnvelope&& other) noexcept
    {
        if (!other.ops_)
            return;
        ops_ = other.ops_;
        inline_ = other.inline_;
        if (inline_)
        {
            data_ = storage_;
            ops_->move_inline(other.data_, data_);
        }
        else
        {
            data_ = other.data_;
        }
        other.data_ = nullptr;
        other.ops_ = nullptr;
        other.inline_ = false;
    }

    EPostStatus post(const ObjectDispatcherRef& dispatcher, MessageEnvelope&& message) noexcept
    {
        if (!dispatcher.state_)
            return EPostStatus::CLOSED;
        std::scoped_lock lock{dispatcher.state_->mutex};
        if (dispatcher.state_->closed)
            return EPostStatus::CLOSED;
        dispatcher.state_->messages.push_back(std::move(message));
        return EPostStatus::POSTED;
    }
} // namespace lux::object::detail

namespace lux::object
{
    bool ObjectDispatcherRef::isCurrent() const noexcept
    {
        if (!state_)
            return false;
        std::scoped_lock lock{state_->mutex};
        return !state_->closed && state_->owner == std::this_thread::get_id();
    }

    ObjectMessageQueue::ObjectMessageQueue() : state_(std::make_shared<detail::ObjectMessageQueueState>())
    {
    }

    ObjectMessageQueue::~ObjectMessageQueue()
    {
        close();
    }

    ObjectDispatcherRef ObjectMessageQueue::dispatcherRef() const noexcept
    {
        return ObjectDispatcherRef{state_};
    }

    std::size_t ObjectMessageQueue::dispatchPending()
    {
        if (!state_ || state_->owner != std::this_thread::get_id())
            std::abort();

        std::deque<detail::MessageEnvelope> local;
        {
            std::scoped_lock lock{state_->mutex};
            local.swap(state_->messages);
        }
        const auto count = local.size();
        for (auto& message : local)
            message.invoke();
        return count;
    }

    void ObjectMessageQueue::close() noexcept
    {
        if (!state_)
            return;
        std::deque<detail::MessageEnvelope> discarded;
        {
            std::scoped_lock lock{state_->mutex};
            state_->closed = true;
            discarded.swap(state_->messages);
        }
    }
} // namespace lux::object
