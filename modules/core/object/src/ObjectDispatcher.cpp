#include <lux/engine/object/ObjectDispatcher.hpp>

#include <cstdlib>
#include <deque>
#include <mutex>
#include <utility>

namespace lux::object
{
    struct ObjectDispatcher::Impl final
    {
        std::mutex mutex;
        std::deque<lux::cxx::move_only_function<void()>> messages;
        std::thread::id owner{std::this_thread::get_id()};
        bool closed{false};
    };

    ObjectDispatcher::ObjectDispatcher()
        : impl_(std::make_unique<Impl>())
    {
    }

    ObjectDispatcher::~ObjectDispatcher()
    {
        close();
    }

    EPostStatus ObjectDispatcher::post(lux::cxx::move_only_function<void()> message)
    {
        std::scoped_lock lock{impl_->mutex};
        if (impl_->closed) return EPostStatus::CLOSED;
        impl_->messages.push_back(std::move(message));
        return EPostStatus::POSTED;
    }

    std::size_t ObjectDispatcher::dispatchPending()
    {
        if (!isOwnerThread()) std::abort();
        std::deque<lux::cxx::move_only_function<void()>> local;
        {
            std::scoped_lock lock{impl_->mutex};
            local.swap(impl_->messages);
        }
        const auto count = local.size();
        for (auto& message : local) message();
        return count;
    }

    void ObjectDispatcher::close() noexcept
    {
        std::scoped_lock lock{impl_->mutex};
        impl_->closed = true;
        impl_->messages.clear();
    }

    std::thread::id ObjectDispatcher::ownerThread() const noexcept
    {
        return impl_->owner;
    }

    bool ObjectDispatcher::isOwnerThread() const noexcept
    {
        return ownerThread() == std::this_thread::get_id();
    }
}
