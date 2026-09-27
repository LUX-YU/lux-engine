#pragma once
#include <lux/engine/scene/WorldMaterializer.hpp>
#include <lux/engine/scene/world_loading/visibility.h>
#include <memory>
#include <optional>

namespace lux::scene
{
    struct WorldResidencyLimits final
    {
        std::size_t partitions{64};
        std::size_t entities{100000};
        std::size_t source_bytes{128 * 1024 * 1024};
        std::size_t component_bytes{64 * 1024 * 1024};
    };
    struct WorldResidencyStatistics final
    {
        std::size_t resident_partitions{}, resident_entities{}, resident_source_bytes{}, resident_component_bytes{};
    };
    enum class EWorldResidencyError : std::uint8_t
    {
        INVALID_PARTITION,
        NOT_RESIDENT,
        INVALID_OBJECT,
        CAPACITY,
        MATERIALIZE_FAILURE,
        REFERENCE_IN_USE
    };
    struct WorldResidencyFailure final
    {
        EWorldResidencyError code{};
        partition::PartitionOrdinal partition;
        WorldMaterializeFailure cause;
    };
    template <class T> using WorldResidencyResult = lux::cxx::expected<T, WorldResidencyFailure>;

    enum class EPartitionRetention : std::uint8_t
    {
        NONE,
        DEMAND,
        DIRTY,
        EXTERNAL,
        REFERENCE,
        UNKNOWN_PAYLOAD
    };
    struct ResidentPartition final
    {
        partition::PartitionOrdinal partition;
        std::size_t entities{}, source_bytes{}, component_bytes{};
        EPartitionRetention retention{};
    };
    namespace detail
    {
        struct PartitionRetentionRecord;
    }
    // External consumers (including history operations) hold actual residency protection.
    // The token has no callback or pointer into a destructed system/Registry.
    class LUX_ENGINE_SCENE_WORLD_LOADING_PUBLIC PartitionRetention final
    {
    public:
        PartitionRetention(PartitionRetention&&) noexcept;
        PartitionRetention& operator=(PartitionRetention&&) noexcept;
        PartitionRetention(const PartitionRetention&) = delete;
        PartitionRetention& operator=(const PartitionRetention&) = delete;
        ~PartitionRetention();

    private:
        friend class WorldResidency;
        explicit PartitionRetention(std::shared_ptr<detail::PartitionRetentionRecord>);
        std::shared_ptr<detail::PartitionRetentionRecord> record_;
    };

    class WorldLoadingSystem;
    class LUX_ENGINE_SCENE_WORLD_LOADING_PUBLIC WorldResidency final
    {
    public:
        struct ObjectInput final
        {
            partition::PartitionOrdinal partition;
            WorldObjectInput object;
        };
        class LUX_ENGINE_SCENE_WORLD_LOADING_PUBLIC PreparedChange final
        {
        public:
            ~PreparedChange() noexcept;
            PreparedChange(PreparedChange&&) noexcept;
            PreparedChange& operator=(PreparedChange&&) noexcept;
            [[nodiscard]] simulation::ecs::Entity entity(world::WorldObjectId) const noexcept;
            [[nodiscard]] std::span<const simulation::ecs::Entity> references() const noexcept;
            [[nodiscard]] std::size_t retainedBytes() const noexcept;
            void commit() noexcept;

        private:
            friend class WorldResidency;
            struct Impl;
            explicit PreparedChange(std::unique_ptr<Impl>) noexcept;
            std::unique_ptr<Impl> impl_;
        };

        WorldResidency(
            simulation::ecs::Registry&,
            std::shared_ptr<const world::WorldDescription>,
            simulation::ecs::ComponentSchemaSet,
            WorldResidencyLimits
        );
        ~WorldResidency() noexcept;
        WorldResidency(const WorldResidency&) = delete;
        WorldResidency& operator=(const WorldResidency&) = delete;
        [[nodiscard]] const world::WorldDescription& description() const noexcept;
        [[nodiscard]] const simulation::ecs::WorldEntityMap& identities() const noexcept;
        [[nodiscard]] bool partitionOf(simulation::ecs::Entity, partition::PartitionOrdinal&) const noexcept;
        [[nodiscard]] const world::WorldPartitionData* source(partition::PartitionOrdinal) const noexcept;
        [[nodiscard]] WorldResidencyStatistics statistics() const noexcept;
        [[nodiscard]] std::vector<ResidentPartition> resident() const;
        [[nodiscard]] WorldResidencyResult<PartitionRetention> retain(partition::PartitionOrdinal);
        [[nodiscard]] WorldResidencyResult<void> setDirty(partition::PartitionOrdinal, bool);
        void clearDirty() noexcept;
        [[nodiscard]] WorldResidencyResult<void> validateAdditional(std::size_t entities, std::size_t bytes)
            const noexcept;
        [[nodiscard]] WorldResidencyResult<PreparedChange>
        prepareCreate(simulation::ecs::Registry&, std::span<const ObjectInput>) noexcept;
        [[nodiscard]] WorldResidencyResult<PreparedChange>
        prepareErase(simulation::ecs::Registry&, std::span<const world::WorldObjectId>) noexcept;

    private:
        friend class WorldLoadingSystem;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
