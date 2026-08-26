#pragma once

#include <lux/engine/simulation/ecs/ComponentSnapshotSet.hpp>
#include <lux/engine/simulation/ecs/SnapshotTypes.hpp>
#include <lux/engine/simulation/ecs/EcsState.hpp>
#include <lux/engine/simulation/ecs/snapshot/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <memory>

namespace lux::simulation::ecs
{
    class LUX_ENGINE_SIMULATION_ECS_SNAPSHOT_PUBLIC EcsSnapshot final
    {
      public:
        EcsSnapshot() noexcept;
        EcsSnapshot(const EcsSnapshot&) = delete;
        EcsSnapshot& operator=(const EcsSnapshot&) = delete;
        EcsSnapshot(EcsSnapshot&&) noexcept;
        EcsSnapshot& operator=(EcsSnapshot&&) noexcept;
        ~EcsSnapshot() noexcept;

        [[nodiscard]] static lux::cxx::expected<EcsSnapshot, SnapshotError>
        capture(
            const EcsState& state,
            const ComponentSnapshotSet& components
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<std::unique_ptr<EcsState>, SnapshotError>
        instantiate() const noexcept;

        [[nodiscard]] lux::cxx::expected<void, SnapshotError>
        restore(EcsState& state) const noexcept;

        void clear() noexcept;
        [[nodiscard]] bool empty() const noexcept;

      private:
        struct Impl;
        explicit EcsSnapshot(std::unique_ptr<Impl> impl) noexcept;

        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::simulation::ecs
