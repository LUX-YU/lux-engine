#pragma once
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/process/world_loading/WorldStorageSource.hpp>
#include <lux/engine/scene/Observer.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <lux/engine/scene/WorldMaterializer.hpp>
#include <lux/engine/scene/world_loading/visibility.h>

#include <memory>
#include <variant>

namespace lux::scene
{
struct LUX_TYPE_INFO(runtime) WorldLoadingConfiguration final
{
    std::vector<partition::PartitionOrdinal> LUX_MEMBER(display_name = BootstrapPartitions) bootstrap;
};

[[nodiscard]] LUX_ENGINE_SCENE_WORLD_LOADING_PUBLIC serialization::PortableValueCodec
worldLoadingConfigurationCodec() noexcept;

struct WorldLoadingLimits final
{
    std::size_t in_flight{4};
    std::size_t partitions{64};
    std::size_t entities{100000};
    std::size_t read_bytes{16 * 1024 * 1024};
    std::size_t staging_bytes{64 * 1024 * 1024};
    std::size_t source_bytes{128 * 1024 * 1024};
    std::size_t component_bytes{64 * 1024 * 1024};
    std::size_t requests_per_turn{4};
};

enum class EWorldLoadingError : std::uint8_t
{
    INVALID_CONFIGURATION,
    INVALID_PARTITION,
    NOT_RESIDENT,
    SOURCE_BUSY,
    CAPACITY,
    READ_FAILURE,
    CANCELLED,
    MATERIALIZE_FAILURE,
    TASK_REJECTED
};
struct WorldLoadingFailure final
{
    EWorldLoadingError code{};
    partition::PartitionOrdinal partition;
    std::variant<std::monostate, process::world_loading::WorldStorageRuntimeFailure, WorldMaterializeFailure,
                 process::ETaskStartError>
        cause;
};
template <class T> using WorldLoadingResult = lux::cxx::expected<T, WorldLoadingFailure>;

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
struct WorldLoadingStatistics final
{
    std::uint64_t demand_revision{}, reads{}, adopted{}, rejected{}, unloaded{};
    std::size_t in_flight{}, reserved_read_bytes{}, staged_bytes{}, resident_partitions{}, resident_entities{},
        resident_source_bytes{}, resident_component_bytes{};
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
    PartitionRetention(PartitionRetention &&) noexcept;
    PartitionRetention &operator=(PartitionRetention &&) noexcept;
    PartitionRetention(const PartitionRetention &) = delete;
    PartitionRetention &operator=(const PartitionRetention &) = delete;
    ~PartitionRetention();

  private:
    friend class WorldLoadingSystem;
    explicit PartitionRetention(std::shared_ptr<detail::PartitionRetentionRecord>);
    std::shared_ptr<detail::PartitionRetentionRecord> record_;
};

struct WorldLoadingServices final
{
    process::world_loading::WorldStorageSource source;
    process::TaskScope &tasks; // Host-owned: closes after all systems have been destroyed.
    WorldLoadingLimits limits;
    std::vector<partition::PartitionOrdinal> bootstrap;
};

class LUX_ENGINE_SCENE_WORLD_LOADING_PUBLIC WorldLoadingSystem final
{
  public:
    static constexpr system::SystemTypeDescription Description{
        .canonical_name = "lux.scene.world_loading",
        .version = 1,
        .configuration_schema_name = "lux.scene.WorldLoadingConfiguration",
        .configuration_schema_version = 1,
        .multiplicity = system::ESystemMultiplicity::SINGLE_PER_OWNER};
    WorldLoadingSystem(simulation::ecs::Registry &, simulation::ecs::ComponentSchemaSet, WorldLoadingServices);
    ~WorldLoadingSystem() noexcept;
    WorldLoadingSystem(const WorldLoadingSystem &) = delete;
    WorldLoadingSystem &operator=(const WorldLoadingSystem &) = delete;

    [[nodiscard]] SceneStageResult maintain(SceneStageContext &) noexcept;
    [[nodiscard]] WorldLoadingResult<void> replaceSource(process::world_loading::WorldStorageSource);
    [[nodiscard]] WorldLoadingResult<PartitionRetention> retain(partition::PartitionOrdinal);
    [[nodiscard]] bool partitionOf(simulation::ecs::Entity, partition::PartitionOrdinal &) const noexcept;
    [[nodiscard]] WorldLoadingResult<void> setDirty(partition::PartitionOrdinal, bool);
    void clearDirty() noexcept;
    [[nodiscard]] WorldLoadingResult<void> retry(partition::PartitionOrdinal);
    [[nodiscard]] WorldLoadingResult<void> validateAdditional(std::size_t entities,
                                                              std::size_t component_bytes) const noexcept;
    // Adopt a complete, already decoded source set before publishing the instance.
    // Uses the same validation/materialization path as asynchronous demand reads.
    [[nodiscard]] WorldLoadingResult<void> adoptCaptured(
        std::span<const std::shared_ptr<const world::WorldPartitionData>>);
    // Persistent identity mapping is owned here; the author editing protocol
    // commits its prepared mapping at the same structural safe point as Registry.
    [[nodiscard]] simulation::ecs::WorldEntityMap &identities() noexcept;
    [[nodiscard]] WorldLoadingResult<void> replaceEntities(partition::PartitionOrdinal,
                                                           std::vector<simulation::ecs::Entity>);
    [[nodiscard]] const simulation::ecs::WorldEntityMap &identities() const noexcept;
    [[nodiscard]] std::vector<ResidentPartition> resident() const;
    [[nodiscard]] const WorldLoadingResult<void> &status() const noexcept;
    [[nodiscard]] WorldLoadingStatistics statistics() const noexcept;
    [[nodiscard]] const world::WorldPartitionData *source(partition::PartitionOrdinal) const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
[[nodiscard]] LUX_ENGINE_SCENE_WORLD_LOADING_PUBLIC SceneSystemRegistration worldLoadingSystemRegistration() noexcept;
} // namespace lux::scene
