#pragma once

#include <exception>
#include <lux/engine/object/ObjectDispatcher.hpp>

namespace lux::test
{
    // Test/installed-consumer owner. Declare before the tested owners so their final releases
    // precede its final safe point. Reclamation is entirely the production dispatcher's responsibility.
    class ObjectQueue final
    {
    public:
        ObjectQueue() : queue_(create()) {}
        ~ObjectQueue() = default;
        ObjectQueue(const ObjectQueue&) = delete;
        ObjectQueue& operator=(const ObjectQueue&) = delete;
        [[nodiscard]] object::ObjectDispatcherRef dispatcherRef() const noexcept
        {
            return queue_.dispatcherRef();
        }
        [[nodiscard]] std::size_t collect() noexcept
        {
            return queue_.collectRetired();
        }
        [[nodiscard]] std::size_t pending() const noexcept
        {
            return queue_.pendingRetirements();
        }

    private:
        static object::ObjectMessageQueue create()
        {
            auto queue = object::ObjectMessageQueue::create(64);
            if (!queue)
            {
                std::terminate();
            }
            return std::move(*queue);
        }
        object::ObjectMessageQueue queue_;
    };
} // namespace lux::test
