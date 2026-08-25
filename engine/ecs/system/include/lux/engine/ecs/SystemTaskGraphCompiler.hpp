#pragma once

#include <lux/engine/ecs/SystemError.hpp>
#include <lux/engine/ecs/SystemExecution.hpp>
#include <lux/engine/ecs/system/visibility.h>

#include <lux/engine/task/TaskGraph.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::ecs
{
    class SystemRegistry;
    class SystemRelations;

    namespace detail
    {
        struct SystemCompilationState;
        struct SystemExecutionAccess;
    }

    struct SystemTaskGraphCompilation final
    {
        lux::task::TaskGraph graph;
        std::uint64_t registry_revision{};
        std::uint64_t relations_revision{};
        SystemExecutionScratchLayout scratch_layout;

        SystemTaskGraphCompilation() = default;
        SystemTaskGraphCompilation(SystemTaskGraphCompilation&&) noexcept = default;
        SystemTaskGraphCompilation& operator=(
            SystemTaskGraphCompilation&&
        ) noexcept = default;

        SystemTaskGraphCompilation(const SystemTaskGraphCompilation&) = delete;
        SystemTaskGraphCompilation& operator=(
            const SystemTaskGraphCompilation&
        ) = delete;

    private:
        std::shared_ptr<detail::SystemCompilationState> state_;

        friend class SystemTaskGraphCompiler;
        friend class SystemExecutionScratch;
        friend struct detail::SystemExecutionAccess;
    };

    class LUX_ENGINE_ECS_SYSTEM_PUBLIC SystemTaskGraphCompiler final
    {
    public:
        [[nodiscard]] lux::cxx::expected<
            SystemTaskGraphCompilation,
            SystemFailure
        > compile(
            const SystemRegistry& registry,
            const SystemRelations& relations
        ) const noexcept;
    };
}
