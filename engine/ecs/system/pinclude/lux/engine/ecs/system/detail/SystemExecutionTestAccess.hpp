#pragma once

#include <lux/engine/ecs/SystemExecution.hpp>
#include <lux/engine/ecs/SystemRegistry.hpp>
#include <lux/engine/ecs/SystemTaskGraphCompiler.hpp>
#include <lux/engine/ecs/system/visibility.h>

#include <lux/engine/ecs/system/detail/SystemRegistryAccess.hpp>

namespace lux::ecs::detail
{
    struct LUX_ENGINE_ECS_SYSTEM_PUBLIC SystemExecutionTestAccess final
    {
        template <class Type>
        [[nodiscard]] static Type& system(
            SystemRegistry& registry,
            SystemId id
        ) noexcept
        {
            const auto record = SystemRegistryAccess::record(registry, id);
            require(record && record->type == lux::cxx::typeToken<Type>());
            return *static_cast<Type*>(record->object);
        }

        static void failNextCommandPush(
            const CompiledSystemTaskGraph& compilation,
            SystemExecutionScratch& scratch,
            SystemId id
        ) noexcept;
    };
}
