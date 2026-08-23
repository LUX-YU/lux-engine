#include <lux/engine/object/Connection.hpp>

#include <lux/engine/object/detail/ObjectState.hpp>

namespace lux::object
{
    bool Connection::connected() const noexcept
    {
        if (!control_ || !control_->connected.load(std::memory_order_acquire))
        {
            return false;
        }
        if (control_->receiver &&
            !control_->receiver->object.load(std::memory_order_acquire))
        {
            return false;
        }
        return sender_ && sender_->object.load(std::memory_order_acquire) != nullptr;
    }

    void Connection::disconnect() noexcept
    {
        if (!sender_ || !control_)
            return;
        sender_->requestDisconnect(control_.get());
    }
} // namespace lux::object
