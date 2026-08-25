#pragma once

#include <lux/engine/ecs/SystemError.hpp>
#include <lux/engine/ecs/SystemTaskGraphCompiler.hpp>
#include <lux/engine/ecs/system/visibility.h>

#include <lux/engine/task/TaskGraph.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::ecs
{
    class World;
    class SystemRegistry;
    class SystemRelations;

    namespace detail
    {
        struct SystemExecutionAccess;
        struct SystemExecutionTestAccess;
    }

    class LUX_ENGINE_ECS_SYSTEM_PUBLIC SystemExecutionScratch final
    {
    public:
        SystemExecutionScratch();
        ~SystemExecutionScratch();

        SystemExecutionScratch(SystemExecutionScratch&&) noexcept;
        SystemExecutionScratch& operator=(SystemExecutionScratch&&) noexcept;

        SystemExecutionScratch(const SystemExecutionScratch&) = delete;
        SystemExecutionScratch& operator=(const SystemExecutionScratch&) = delete;

        [[nodiscard]] lux::cxx::expected<void, SystemFailure> prepare(
            const CompiledSystemTaskGraph& compilation,
            std::size_t reserve_change_records = 0U
        ) noexcept;

        [[nodiscard]] std::size_t systemCapacity() const noexcept;
        [[nodiscard]] std::uint64_t laneBindCount() const noexcept;
        [[nodiscard]] std::uint64_t journalStreamBindCount() const noexcept;
        [[nodiscard]] std::uint64_t recordAppendCount() const noexcept;
        [[nodiscard]] std::uint64_t perRecordLookupCount() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;

        friend struct detail::SystemExecutionAccess;
        friend struct detail::SystemExecutionTestAccess;
    };

    struct EcsExecutionContext final
    {
        World& world;
        SystemRegistry& systems;
        const SystemRelations& relations;
        SystemExecutionScratch& scratch;
        float delta_seconds{};
        std::uint64_t tick_index{};
    };

    [[nodiscard]] LUX_ENGINE_ECS_SYSTEM_PUBLIC
    lux::cxx::expected<void, SystemFailure> executeSystemTaskGraph(
        lux::task::TaskExecutionBackendRef backend,
        const CompiledSystemTaskGraph& compilation,
        EcsExecutionContext& context
    ) noexcept;
}
