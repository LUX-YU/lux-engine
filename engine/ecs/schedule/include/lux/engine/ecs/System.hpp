#pragma once

#include <lux/engine/ecs/SystemAccess.hpp>
#include <lux/engine/ecs/SystemFrame.hpp>
#include <lux/engine/ecs/SystemStart.hpp>
#include <lux/engine/ecs/schedule/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>

namespace lux::ecs
{
    enum class ESystemStartError : std::uint8_t
    {
        REJECTED,
        ALLOCATION_FAILURE,
    };

    struct SystemStartError final
    {
        ESystemStartError code{ESystemStartError::REJECTED};
    };

    class LUX_ENGINE_ECS_SCHEDULE_PUBLIC System
    {
      public:
        virtual ~System() = default;

        [[nodiscard]] virtual SystemAccess access() const noexcept
        {
            return {};
        }

        // Before publication, start() has transactional RAII semantics. If a
        // later staged System fails to start, destroying this object must
        // completely roll back every resource acquired here. requestStop()
        // applies only after successful Schedule publication.
        [[nodiscard]] virtual lux::cxx::expected<void, SystemStartError>
        start(SystemStart&) noexcept
        {
            return {};
        }

        virtual void update(SystemFrame&) noexcept = 0;

        virtual void requestStop() noexcept {}

        [[nodiscard]] virtual bool stopped() const noexcept
        {
            return true;
        }
    };
} // namespace lux::ecs
