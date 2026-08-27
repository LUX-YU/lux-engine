#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include <lux/cxx/memory/intrusive_ptr.hpp>
#include <lux/engine/core/visibility.h>
#include <lux/engine/object/detail/ObjectStorageFwd.hpp>

namespace lux::object
{
    class LuxObject;

    class LUX_CORE_PUBLIC Connection final
    {
    public:
        Connection() noexcept = default;

        [[nodiscard]] bool connected() const noexcept;
        /**
         * Empties this handle. Foreign-thread disconnect requires the sender's
         * dispatcher to accept topology cleanup; otherwise the process fails
         * the Object lifetime contract instead of retaining a partial link.
         */
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
        explicit ScopedConnection(Connection connection) noexcept : connection_(std::move(connection))
        {
        }

        ScopedConnection(const ScopedConnection&) = delete;
        ScopedConnection& operator=(const ScopedConnection&) = delete;
        ScopedConnection(ScopedConnection&&) noexcept = default;
        ScopedConnection& operator=(ScopedConnection&& other) noexcept
        {
            if (this != std::addressof(other))
            {
                reset();
                connection_ = std::move(other.connection_);
            }
            return *this;
        }

        ~ScopedConnection()
        {
            reset();
        }

        [[nodiscard]] bool connected() const noexcept
        {
            return connection_.connected();
        }
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return connected();
        }
        void reset() noexcept
        {
            connection_.disconnect();
        }
        [[nodiscard]] Connection release() noexcept
        {
            return std::exchange(connection_, {});
        }

    private:
        Connection connection_;
    };
} // namespace lux::object
