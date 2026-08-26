#pragma once

#include <lux/engine/world/WorldPartition.hpp>
#include <lux/engine/world/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <memory>
#include <span>

namespace lux::world
{
    struct WorldDataSnapshotView final
    {
        const WorldDataSchemaId* schema{};
        std::uint32_t version{};
        std::span<const std::byte> payload;

        [[nodiscard]] bool valid() const noexcept
        {
            return schema != nullptr && schema->valid() && version != 0U;
        }
    };

    struct WorldObjectSnapshotView final
    {
        WorldObjectId id;
        std::span<const WorldDataSnapshotView> data;

        [[nodiscard]] bool valid() const noexcept
        {
            if (!id.valid())
                return false;
            for (const auto& value : data)
                if (!value.valid())
                    return false;
            return true;
        }
    };

    /** Mutable, query-free, cold-build derived partition state. */
    class LUX_ENGINE_WORLD_PUBLIC WorldPartitionWorkspace
    {
      public:
        WorldPartitionWorkspace() noexcept;
        virtual ~WorldPartitionWorkspace();

        [[nodiscard]] virtual const WorldPartitionerDescriptor&
        descriptor() const noexcept = 0;

        [[nodiscard]] virtual lux::cxx::expected<void, WorldPartitionFailure>
        rebuild(const WorldDescription& world) noexcept = 0;

        [[nodiscard]] virtual lux::cxx::expected<void, WorldPartitionFailure>
        objectAdded(WorldObjectSnapshotView object) noexcept = 0;

        [[nodiscard]] virtual lux::cxx::expected<void, WorldPartitionFailure>
        objectChanged(WorldObjectSnapshotView object) noexcept = 0;

        [[nodiscard]] virtual lux::cxx::expected<void, WorldPartitionFailure>
        objectRemoved(WorldObjectId object) noexcept = 0;

        [[nodiscard]] virtual lux::cxx::expected<
            WorldPartitionBuildProduct,
            WorldPartitionFailure>
        freeze(const WorldDescription& world) const noexcept = 0;
    };

    /** Cold-path policy/factory supplied by an upper-layer interpreter. */
    class LUX_ENGINE_WORLD_PUBLIC WorldPartitioner
    {
      public:
        WorldPartitioner() noexcept;
        virtual ~WorldPartitioner();

        [[nodiscard]] virtual WorldPartitionerDescriptor
        descriptor() const noexcept = 0;

        [[nodiscard]] virtual lux::cxx::expected<
            std::unique_ptr<WorldPartitionWorkspace>,
            WorldPartitionFailure>
        createWorkspace(const WorldDescription& world) const noexcept = 0;
    };
} // namespace lux::world
