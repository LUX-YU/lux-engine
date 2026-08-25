#pragma once

#include <lux/engine/ecs/Entity.hpp>
#include <lux/engine/ecs/Query.hpp>
#include <lux/engine/ecs/SystemError.hpp>
#include <lux/engine/ecs/system/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lux::ecs
{
    class World;
    class SystemContext;

    namespace detail
    {
        struct SystemExecutionAccess;
    }

    /** Per-System transient changes. Lanes are bound once, records append O(1). */
    class LUX_ENGINE_ECS_SYSTEM_PUBLIC ChangeSet final
    {
    public:
        ChangeSet() = default;
        ~ChangeSet() = default;

        ChangeSet(ChangeSet&&) noexcept = default;
        ChangeSet& operator=(ChangeSet&&) noexcept = default;

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
        [[nodiscard]] std::uint64_t journalStreamBindCount() const noexcept;
        [[nodiscard]] std::uint64_t recordAppendCount() const noexcept;
        [[nodiscard]] std::uint64_t perRecordLookupCount() const noexcept;

    private:
        struct Lane final
        {
            std::uint64_t storage{};
            std::vector<Entity> records;
        };

        std::vector<Lane> lanes_;
        std::vector<detail::BoundWorldChangeStream> publish_streams_;
        std::uint64_t lane_bind_count_{};
        std::uint64_t journal_stream_bind_count_{};
        std::uint64_t record_append_count_{};
        std::uint64_t per_record_lookup_count_{};
        bool overflow_{};

        [[nodiscard]] detail::ChangeStreamBinder binder() noexcept;
        [[nodiscard]] bool publish(World& world) noexcept;

        friend class SystemContext;
        friend struct detail::SystemExecutionAccess;
    };
}
