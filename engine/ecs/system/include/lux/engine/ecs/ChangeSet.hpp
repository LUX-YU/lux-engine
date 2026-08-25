#pragma once

#include <lux/engine/ecs/Entity.hpp>
#include <lux/engine/ecs/Query.hpp>
#include <lux/engine/ecs/SystemError.hpp>
#include <lux/engine/ecs/system/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace lux::ecs
{
    class World;
    class SystemContext;

    namespace detail
    {
        struct SystemExecutionAccess;
    }

    class LUX_ENGINE_ECS_SYSTEM_PUBLIC ChangeSet final
    {
    public:
        ChangeSet();
        ~ChangeSet();

        ChangeSet(ChangeSet&&) noexcept;
        ChangeSet& operator=(ChangeSet&&) noexcept;

        ChangeSet(const ChangeSet&) = delete;
        ChangeSet& operator=(const ChangeSet&) = delete;

        [[nodiscard]] lux::cxx::expected<void, SystemFailure> prepare(
            std::span<const std::uint64_t> write_storages,
            std::size_t reserve_records = 0U
        ) noexcept;

        void reset() noexcept;

        [[nodiscard]] bool overflowed() const noexcept;
        [[nodiscard]] std::size_t recordCount() const noexcept;
        [[nodiscard]] std::uint64_t laneBindCount() const noexcept;
        [[nodiscard]] std::uint64_t perRecordLookupCount() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;

        [[nodiscard]] detail::ChangeStreamBinder binder() noexcept;
        [[nodiscard]] bool publish(World& world) noexcept;

        friend class SystemContext;
        friend struct detail::SystemExecutionAccess;
    };
}
