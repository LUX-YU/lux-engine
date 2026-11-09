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
    void Connection::observeSourceClose(void* context, void (*callback)(void*) noexcept) noexcept
    {
        control_->close_context = context;
        control_->source_closed = callback;
    }

    void Connection::disconnect() noexcept
    {
        auto control = std::move(control_);
        if (control)
        {
            // Source-close watches are installed and revoked on the object's affinity thread.
            if (control->source_closed)
            {
                if (!ObjectRuntime::instance().isCurrent())
                {
                    detail::failObjectContract();
                }
                control->source_closed = nullptr;
                control->close_context = nullptr;
            }
            control->storage->cancel(*control);
        }
    }
}
