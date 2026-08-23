#include <lux/engine/object/Connection.hpp>

#include <lux/engine/object/detail/ObjectState.hpp>

namespace lux::object
{
    bool Connection::connected() const noexcept
    {
        const auto slot = slot_.lock();
        return slot && slot->connected.load(std::memory_order_acquire);
    }

    void Connection::disconnect() noexcept
    {
        if (const auto slot = slot_.lock())
            slot->connected.store(false, std::memory_order_release);
    }
}
