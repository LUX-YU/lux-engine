#pragma once

#include <lux/engine/world/WorldPartition.hpp>
#include <lux/engine/world/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <memory>
#include <span>

namespace lux::world
{
    enum class EWorldPartitionWorkspaceState : std::uint8_t
    {
        STALE,
        SYNCHRONIZED,
    };

    /**
     * Call-scoped borrowed data. Implementations may copy derived values, but
     * must not retain schema pointers or payload spans after the call returns.
     */
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

        [[nodiscard]] virtual const WorldPartitionerDescriptor& descriptor() const noexcept = 0;

        [[nodiscard]] EWorldPartitionWorkspaceState state() const noexcept;

        [[nodiscard]] lux::cxx::expected<void, WorldPartitionFailure> rebuild(const WorldDescription& world) noexcept;

        [[nodiscard]] lux::cxx::expected<void, WorldPartitionFailure>
        objectAdded(WorldObjectSnapshotView object) noexcept;

        [[nodiscard]] lux::cxx::expected<void, WorldPartitionFailure>
        objectChanged(WorldObjectSnapshotView object) noexcept;

        [[nodiscard]] lux::cxx::expected<void, WorldPartitionFailure> objectRemoved(WorldObjectId object) noexcept;

        [[nodiscard]] lux::cxx::expected<WorldPartitionBuildProduct, WorldPartitionFailure>
        freeze(const WorldDescription& world) const noexcept;

    protected:
        [[nodiscard]] virtual lux::cxx::expected<void, WorldPartitionFailure>
        doRebuild(const WorldDescription& world) noexcept = 0;

        [[nodiscard]] virtual lux::cxx::expected<void, WorldPartitionFailure>
        doObjectAdded(WorldObjectSnapshotView object) noexcept = 0;

        [[nodiscard]] virtual lux::cxx::expected<void, WorldPartitionFailure>
        doObjectChanged(WorldObjectSnapshotView object) noexcept = 0;

        [[nodiscard]] virtual lux::cxx::expected<void, WorldPartitionFailure>
        doObjectRemoved(WorldObjectId object) noexcept = 0;

        [[nodiscard]] virtual lux::cxx::expected<WorldPartitionBuildProduct, WorldPartitionFailure>
        doFreeze(const WorldDescription& world) const noexcept = 0;

    private:
        EWorldPartitionWorkspaceState state_{EWorldPartitionWorkspaceState::STALE};
    };

    /** Cold-path policy/factory supplied by an upper-layer interpreter. */
    class LUX_ENGINE_WORLD_PUBLIC WorldPartitioner
    {
    public:
        WorldPartitioner() noexcept;
        virtual ~WorldPartitioner();

        [[nodiscard]] virtual WorldPartitionerDescriptor descriptor() const noexcept = 0;

        [[nodiscard]] lux::cxx::expected<std::unique_ptr<WorldPartitionWorkspace>, WorldPartitionFailure>
        createWorkspace(const WorldDescription& world) const noexcept;

    protected:
        [[nodiscard]] virtual lux::cxx::expected<std::unique_ptr<WorldPartitionWorkspace>, WorldPartitionFailure>
        createWorkspaceImplementation() const noexcept = 0;
    };
} // namespace lux::world
