#pragma once

#include <lux/engine/ecs/SystemExecution.hpp>
#include <lux/engine/ecs/system/detail/SystemCompilation.hpp>
#include <lux/engine/ecs/system/detail/SystemRegistryAccess.hpp>

namespace lux::ecs::detail
{
    struct SystemExecutionAccess final
    {
        [[nodiscard]] static lux::cxx::expected<void, SystemFailure> execute(
            lux::task::TaskExecutionBackendRef backend,
            const CompiledSystemTaskGraph& compilation,
            EcsExecutionContext& context
        ) noexcept;

        static void invokeSystem(void* target, void* context) noexcept;
        static void publishChanges(void* target, void* context) noexcept;
        static void applyCommands(void* target, void* context) noexcept;
    };
}
