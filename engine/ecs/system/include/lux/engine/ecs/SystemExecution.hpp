#pragma once

#include <lux/engine/ecs/SystemError.hpp>
#include <lux/engine/ecs/SystemStart.hpp>
#include <lux/engine/ecs/system/visibility.h>

#include <lux/engine/task/TaskGraph.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace lux::ecs
{
    class World;
    class SystemRegistry;
    class EcsExecutionContext;
    struct SystemTaskGraphCompilation;

    struct SystemScratchLaneLayout final
    {
        std::vector<std::uint64_t> write_storages;
    };

    struct SystemExecutionScratchLayout final
    {
        std::vector<SystemScratchLaneLayout> systems;
        std::size_t task_count{};
    };

    namespace detail
    {
        struct SystemExecutionAccess;
    }

    [[nodiscard]] LUX_ENGINE_ECS_SYSTEM_PUBLIC
    lux::cxx::expected<void, SystemFailure> executeSystemTaskGraph(
        lux::task::TaskExecutionBackendRef backend,
        const SystemTaskGraphCompilation& compilation,
        EcsExecutionContext& context
    ) noexcept;

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
            const SystemTaskGraphCompilation& compilation,
            std::size_t reserve_change_records = 0U
        ) noexcept;

        [[nodiscard]] std::size_t systemCapacity() const noexcept;
        [[nodiscard]] std::uint64_t laneBindCount() const noexcept;
        [[nodiscard]] std::uint64_t perRecordLookupCount() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;

        friend struct detail::SystemExecutionAccess;
    };

    class LUX_ENGINE_ECS_SYSTEM_PUBLIC EcsExecutionContext final
    {
    public:
        EcsExecutionContext(
            World& world,
            const SystemRegistry& systems,
            SystemExecutionScratch& scratch,
            float delta_seconds,
            std::uint64_t tick_index
        ) noexcept;

        ~EcsExecutionContext() noexcept;

        EcsExecutionContext(const EcsExecutionContext&) = delete;
        EcsExecutionContext& operator=(const EcsExecutionContext&) = delete;
        EcsExecutionContext(EcsExecutionContext&&) = delete;
        EcsExecutionContext& operator=(EcsExecutionContext&&) = delete;

    private:
        [[nodiscard]] SystemStart startContext() const noexcept
        {
            return SystemStart(*world_);
        }

        World* world_{};
        const SystemRegistry* systems_{};
        SystemExecutionScratch* scratch_{};
        float delta_seconds_{};
        std::uint64_t tick_index_{};
        bool executing_{};

        friend struct detail::SystemExecutionAccess;
    };
}
