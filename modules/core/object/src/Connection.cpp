#include <lux/engine/object/Connection.hpp>
#include <lux/engine/object/detail/ObjectState.hpp>

namespace lux::object
{
    Connection::Connection(Connection&&) noexcept = default;
    Connection& Connection::operator=(Connection&& other) noexcept
    {
        if (this != &other)
        {
            disconnect();
            control_ = std::move(other.control_);
        }
        return *this;
    }
    Connection::~Connection() noexcept
    {
        disconnect();
    }

    bool Connection::connected() const noexcept
    {
        if (!control_ || !control_->connected.load(std::memory_order_acquire))
            return false;
        if (control_->storage->closed.load(std::memory_order_acquire))
            return false;
        return !control_->receiver || control_->receiver->object.load(std::memory_order_acquire) != nullptr;
    }
    void Connection::disconnect() noexcept
    {
        auto control = std::move(control_);
        if (control)
            control->storage->cancel(*control);
    }
}
