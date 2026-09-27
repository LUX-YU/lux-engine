#pragma once

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
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        Connection(Connection&&) noexcept;
        Connection& operator=(Connection&&) noexcept;
        ~Connection() noexcept;

        [[nodiscard]] bool connected() const noexcept;
        void disconnect() noexcept;

    private:
        friend class LuxObject;
        explicit Connection(lux::cxx::intrusive_ptr<detail::ConnectionControl> control) noexcept
            : control_(std::move(control))
        {}

        lux::cxx::intrusive_ptr<detail::ConnectionControl> control_;
    };
}
