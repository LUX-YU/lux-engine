#pragma once

#include <lux/engine/ecs/SystemError.hpp>
#include <lux/engine/ecs/SystemRelations.hpp>
#include <lux/engine/ecs/system/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::ecs
{
    class SystemRegistry;

    namespace detail
    {
        struct SystemExecutionAccess;
        struct SystemExecutionTestAccess;
    }

    /** Immutable L1 compilation. Runtime System ownership remains in Registry. */
    class LUX_ENGINE_ECS_SYSTEM_PUBLIC CompiledSystemTaskGraph final
    {
    public:
        CompiledSystemTaskGraph() noexcept;
        ~CompiledSystemTaskGraph();

        CompiledSystemTaskGraph(CompiledSystemTaskGraph&&) noexcept;
        CompiledSystemTaskGraph& operator=(
            CompiledSystemTaskGraph&&
        ) noexcept;

        CompiledSystemTaskGraph(const CompiledSystemTaskGraph&) = delete;
        CompiledSystemTaskGraph& operator=(
            const CompiledSystemTaskGraph&
        ) = delete;

        [[nodiscard]] std::size_t systemCount() const noexcept;
        [[nodiscard]] std::size_t taskCount() const noexcept;
        [[nodiscard]] std::size_t dependencyCount() const noexcept;
        [[nodiscard]] SystemRegistryId sourceRegistry() const noexcept;
        [[nodiscard]] std::uint64_t sourceRegistryRevision() const noexcept;
        [[nodiscard]] SystemRelationsId sourceRelations() const noexcept;
        [[nodiscard]] std::uint64_t sourceRelationsRevision() const noexcept;

    private:
        struct Impl;
        explicit CompiledSystemTaskGraph(
            std::unique_ptr<const Impl> impl
        ) noexcept;

        std::unique_ptr<const Impl> impl_;

        friend LUX_ENGINE_ECS_SYSTEM_PUBLIC lux::cxx::expected<
            CompiledSystemTaskGraph,
            SystemFailure
        > compileSystemTaskGraph(
            const SystemRegistry&,
            const SystemRelations&
        ) noexcept;
        friend class SystemExecutionScratch;
        friend struct detail::SystemExecutionAccess;
        friend struct detail::SystemExecutionTestAccess;
    };

    [[nodiscard]] LUX_ENGINE_ECS_SYSTEM_PUBLIC lux::cxx::expected<
        CompiledSystemTaskGraph,
        SystemFailure
    > compileSystemTaskGraph(
        const SystemRegistry& registry,
        const SystemRelations& relations
    ) noexcept;
}
