#pragma once

#include <lux/engine/ecs/SystemTaskGraphCompiler.hpp>

#include <lux/engine/ecs/system/detail/SystemRegistryAccess.hpp>

#include <memory>
#include <vector>

namespace lux::ecs::detail
{
    struct SystemTaskTarget final
    {
        SystemId id{};
        std::shared_ptr<SystemRecord> system;
        std::size_t scratch_index{};
    };

    struct SystemPublishTarget final
    {
        std::vector<std::size_t> scratch_indices;
    };

    struct SystemCompilationState final
    {
        const SystemRegistry* registry{};
        const SystemRelations* relations{};
        std::vector<SystemTaskTarget> system_tasks;
        std::vector<SystemPublishTarget> publish_tasks;
    };

    struct SystemExecutionAccess final
    {
        [[nodiscard]] static lux::cxx::expected<void, SystemFailure> execute(
            lux::task::TaskExecutionBackendRef backend,
            const SystemTaskGraphCompilation& compilation,
            EcsExecutionContext& context
        ) noexcept;

        static void invokeSystem(void* target, void* context) noexcept;
        static void publishChanges(void* target, void* context) noexcept;
        static void applyCommands(void* target, void* context) noexcept;
    };
}
