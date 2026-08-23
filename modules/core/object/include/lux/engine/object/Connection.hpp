#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include <lux/cxx/memory/intrusive_ptr.hpp>
#include <lux/engine/core/visibility.h>
#include <lux/engine/object/ObjectFwd.hpp>

namespace lux::object
{
    class LuxObject;

    class LUX_CORE_PUBLIC Connection final
    {
    public:
        Connection() noexcept = default;

        [[nodiscard]] bool connected() const noexcept;
        void disconnect() noexcept;

    private:
        friend class LuxObject;
        Connection(
            lux::cxx::intrusive_ptr<detail::ObjectState> sender,
            lux::cxx::intrusive_ptr<detail::ConnectionControl> control
        ) noexcept
            : sender_(std::move(sender)), control_(std::move(control))
        {
        }

        lux::cxx::intrusive_ptr<detail::ObjectState> sender_;
        lux::cxx::intrusive_ptr<detail::ConnectionControl> control_;
    };

    class ScopedConnection final
    {
    public:
        ScopedConnection() noexcept = default;
        explicit ScopedConnection(Connection connection) noexcept
            : connection_(std::move(connection))
        {
        }

        ScopedConnection(const ScopedConnection&) = delete;
        ScopedConnection& operator=(const ScopedConnection&) = delete;
        ScopedConnection(ScopedConnection&&) noexcept = default;
        ScopedConnection& operator=(ScopedConnection&& other) noexcept
        {
            if (this != std::addressof(other))
            {
                connection_.disconnect();
                connection_ = std::move(other.connection_);
            }
            return *this;
        }

        ~ScopedConnection() { connection_.disconnect(); }

        [[nodiscard]] Connection& connection() noexcept { return connection_; }
        [[nodiscard]] const Connection& connection() const noexcept
        {
            return connection_;
        }

    private:
        Connection connection_;
    };
} // namespace lux::object
