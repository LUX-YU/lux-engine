#include <lux/engine/object/ObjectDispatcher.hpp>

#include <cstdlib>
#include <deque>
#include <mutex>
#include <utility>

namespace lux::object::detail
{
    struct ObjectMessageQueueState final
    {
        std::mutex mutex;
        std::deque<ObjectMessage> messages;
        std::thread::id owner{std::this_thread::get_id()};
        bool closed{false};
    };
} // namespace lux::object::detail

namespace lux::object
{
    ObjectMessage::ObjectMessage(ObjectMessage&& other) noexcept
    {
        moveFrom(std::move(other));
    }

    ObjectMessage& ObjectMessage::operator=(ObjectMessage&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            reset();
            moveFrom(std::move(other));
        }
        return *this;
    }

    ObjectMessage::~ObjectMessage() { reset(); }

    void ObjectMessage::invoke() noexcept
    {
        if (ops_)
            ops_->invoke(data_);
    }

    void ObjectMessage::reset() noexcept
    {
        if (ops_)
            ops_->destroy(data_);
        data_ = nullptr;
        ops_ = nullptr;
        inline_ = false;
    }

    void ObjectMessage::moveFrom(ObjectMessage&& other) noexcept
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

    bool ObjectDispatcherRef::isCurrent() const noexcept
    {
        if (!state_)
            return false;
        std::scoped_lock lock{state_->mutex};
        return !state_->closed && state_->owner == std::this_thread::get_id();
    }

    EPostStatus ObjectDispatcherRef::post(ObjectMessage&& message) const noexcept
    {
        if (!state_)
            return EPostStatus::CLOSED;
        std::scoped_lock lock{state_->mutex};
        if (state_->closed)
            return EPostStatus::CLOSED;
        state_->messages.push_back(std::move(message));
        return EPostStatus::POSTED;
    }

    ObjectMessageQueue::ObjectMessageQueue()
        : state_(std::make_shared<detail::ObjectMessageQueueState>())
    {
    }

    ObjectMessageQueue::~ObjectMessageQueue() { close(); }

    ObjectDispatcherRef ObjectMessageQueue::dispatcherRef() const noexcept
    {
        return ObjectDispatcherRef{state_};
    }

    std::size_t ObjectMessageQueue::dispatchPending()
    {
        if (!state_ || state_->owner != std::this_thread::get_id())
            std::abort();

        std::deque<ObjectMessage> local;
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
        std::scoped_lock lock{state_->mutex};
        state_->closed = true;
        state_->messages.clear();
    }
} // namespace lux::object
