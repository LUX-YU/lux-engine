#pragma once

#include <memory>

#include <lux/engine/core/visibility.h>

namespace lux::object
{
    namespace detail
    {
        struct ObjectSlot;
    }

    class LUX_CORE_PUBLIC Connection final
    {
    public:
        Connection() noexcept = default;

        [[nodiscard]] bool connected() const noexcept;
        void disconnect() noexcept;

    private:
        friend class LuxObject;
        explicit Connection(std::weak_ptr<detail::ObjectSlot> slot) noexcept
            : slot_(std::move(slot))
        {
        }

        std::weak_ptr<detail::ObjectSlot> slot_;
    };

    class ScopedConnection final
    {
    public:
        ScopedConnection() noexcept = default;
        explicit ScopedConnection(Connection connection) noexcept
            : connection_(std::move(connection))
        {
        }

        ScopedConnection(const ScopedConnection &) = delete;
        ScopedConnection &operator=(const ScopedConnection &) = delete;
        ScopedConnection(ScopedConnection &&) noexcept = default;
        ScopedConnection &operator=(ScopedConnection &&other) noexcept
        {
            if (this != &other)
            {
                connection_.disconnect();
                connection_ = std::move(other.connection_);
            }
            return *this;
        }

        ~ScopedConnection()
        {
            connection_.disconnect();
        }

        [[nodiscard]] Connection &connection() noexcept { return connection_; }
        [[nodiscard]] const Connection &connection() const noexcept { return connection_; }

    private:
        Connection connection_;
    };
}
