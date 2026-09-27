#pragma once
#include <lux/engine/scene/WorldResidency.hpp>
#include <atomic>
#include <map>
#include <unordered_map>

namespace lux::scene
{
    namespace detail
    {
        struct PartitionRetentionRecord final
        {
            std::atomic<std::size_t> uses{};
        };
    }
    struct WorldResidency::Impl final
    {
        struct Resident final
        {
            std::shared_ptr<const world::WorldPartitionData> source;
            std::vector<simulation::ecs::Entity> entities;
            std::shared_ptr<detail::PartitionRetentionRecord> protection{
                std::make_shared<detail::PartitionRetentionRecord>()
            };
            std::size_t component_bytes{};
            bool dirty{}, unknown{};
            EPartitionRetention retention{};
        };
        struct Membership final
        {
            std::uint32_t partition;
            std::size_t component_bytes;
        };

        Impl(
            simulation::ecs::Registry& owner,
            std::shared_ptr<const world::WorldDescription> value,
            simulation::ecs::ComponentSchemaSet schema_set,
            WorldResidencyLimits capacity
        )
            : owner_registry(&owner), world(std::move(value)), schemas(std::move(schema_set)), limits(capacity)
        {}
        [[nodiscard]] Resident* find(std::uint32_t ordinal) noexcept;
        [[nodiscard]] const Resident* find(std::uint32_t ordinal) const noexcept;
        [[nodiscard]] std::size_t componentBytes(const simulation::ecs::Registry&, simulation::ecs::Entity) const;
        void entityDestroyed(simulation::ecs::Entity) noexcept;
        void updateAccounting(simulation::ecs::Registry&, simulation::ecs::ComponentOperations::MembershipChanges&);
        [[nodiscard]] std::size_t unload(simulation::ecs::Registry&, const std::map<std::uint32_t, bool>&);
        [[nodiscard]] WorldResidencyResult<void>
        adopt(simulation::ecs::Registry&, std::span<const std::shared_ptr<const world::WorldPartitionData>>);
        [[nodiscard]] WorldResidencyStatistics statistics() const noexcept;
        [[nodiscard]] bool partitionOf(simulation::ecs::Entity, partition::PartitionOrdinal&) const noexcept;
        [[nodiscard]] WorldResidencyResult<PartitionRetention> retain(partition::PartitionOrdinal);
        [[nodiscard]] WorldResidencyResult<void> setDirty(partition::PartitionOrdinal, bool);
        void clearDirty() noexcept;
        [[nodiscard]] std::vector<ResidentPartition> resident() const;
        [[nodiscard]] WorldResidencyResult<void> validateAdditional(std::size_t, std::size_t) const noexcept;

        [[nodiscard]] WorldResidencyResult<PreparedChange>
        prepareCreate(simulation::ecs::Registry&, std::span<const ObjectInput>) noexcept;
        [[nodiscard]] WorldResidencyResult<PreparedChange>
        prepareErase(simulation::ecs::Registry&, std::span<const world::WorldObjectId>) noexcept;

        const simulation::ecs::Registry* owner_registry; // Identity check only; never accessed in destruction.
        std::shared_ptr<const world::WorldDescription> world;
        simulation::ecs::ComponentSchemaSet schemas;
        WorldResidencyLimits limits;
        simulation::ecs::WorldEntityMap identities;
        std::vector<Resident> residents;
        std::unordered_map<simulation::ecs::Entity, Membership> membership;
        bool has_dirty{};
    };
}
