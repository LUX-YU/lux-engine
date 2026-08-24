#pragma once

#include <lux/engine/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/snapshot/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <memory>

namespace lux::ecs
{
    enum class ESnapshotError : std::uint8_t
    {
        WORLD_BUSY,
        UNKNOWN_COMPONENT_STORAGE,
        INVALID_COPY_SCHEMA,
        ALLOCATION_FAILURE,
    };

    struct SnapshotError final
    {
        ESnapshotError code{ESnapshotError::ALLOCATION_FAILURE};
        std::uint64_t storage_id{};
        ComponentSchemaId schema;
    };

    class LUX_ENGINE_ECS_SNAPSHOT_PUBLIC WorldSnapshot final
    {
      public:
        WorldSnapshot() noexcept = default;
        WorldSnapshot(const WorldSnapshot&) = delete;
        WorldSnapshot& operator=(const WorldSnapshot&) = delete;
        WorldSnapshot(WorldSnapshot&&) noexcept;
        WorldSnapshot& operator=(WorldSnapshot&&) noexcept;
        ~WorldSnapshot() noexcept;

        [[nodiscard]] static lux::cxx::expected<WorldSnapshot, SnapshotError>
        capture(
            const World& world,
            const ComponentSchemaSet& schemas
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<std::unique_ptr<World>, SnapshotError>
        instantiate(WorldConfig config = {}) const noexcept;

        [[nodiscard]] lux::cxx::expected<void, SnapshotError>
        restore(World& world) const noexcept;

        void clear() noexcept;
        [[nodiscard]] bool empty() const noexcept;

      private:
        struct Impl;
        explicit WorldSnapshot(std::unique_ptr<Impl> impl) noexcept;

        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::ecs
