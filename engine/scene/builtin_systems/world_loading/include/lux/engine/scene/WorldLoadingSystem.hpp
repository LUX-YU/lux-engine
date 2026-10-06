#pragma once
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/process/world_loading/WorldStorageSource.hpp>
#include <lux/engine/scene/Observer.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <lux/engine/scene/WorldResidency.hpp>
#include <lux/engine/scene/WorldLoadingConfiguration.hpp>
#include <lux/engine/scene/world_loading/visibility.h>

#include <memory>
#include <variant>

namespace lux::scene
{
    struct WorldLoadingLimits final
    {
        std::size_t in_flight{4};
        WorldResidencyLimits residency;
        std::size_t read_bytes{16 * 1024 * 1024};
        std::size_t staging_bytes{64 * 1024 * 1024};
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
        std::variant<
            std::monostate,
            process::world_loading::WorldStorageRuntimeFailure,
            WorldMaterializeFailure,
            WorldResidencyFailure,
            process::EExecutionError>
            cause;
    };
    // Compact cross-system classification. status() retains the complete storage/materialization diagnostics.
    [[nodiscard]] LUX_ENGINE_SCENE_WORLD_LOADING_PUBLIC error::Error toError(const WorldLoadingFailure&) noexcept;
    template <class T> using WorldLoadingResult = lux::cxx::expected<T, WorldLoadingFailure>;

    struct WorldLoadingStatistics final
    {
        std::uint64_t demand_revision{}, reads{}, adopted{}, rejected{}, unloaded{};
        std::size_t in_flight{}, reserved_read_bytes{}, staged_bytes{};
    };

    struct WorldLoadingServices final
    {
        process::world_loading::WorldStorageSource source;
        process::TaskScope& tasks; // Host-owned: closes after all systems have been destroyed.
        WorldLoadingLimits limits;
        std::vector<partition::PartitionOrdinal> bootstrap;
        std::vector<std::shared_ptr<const world::WorldPartitionData>> initial_partitions;
    };

    class LUX_ENGINE_SCENE_WORLD_LOADING_PUBLIC WorldLoadingSystem final
    {
    public:
        inline static constexpr std::string_view SupportedWorldTypes[]{"*"};
        static constexpr system::SystemTypeDescription Description{
            .canonical_name = "lux.scene.world_loading",
            .version = 1,
            .configuration_schema_name = "lux.scene.WorldLoadingConfiguration",
            .configuration_schema_version = 1,
            .multiplicity = system::ESystemMultiplicity::SINGLE_PER_OWNER,
            .supported_world_types = SupportedWorldTypes
        };
        WorldLoadingSystem(simulation::ecs::Registry&, simulation::ecs::ComponentSchemaSet, WorldLoadingServices);
        ~WorldLoadingSystem() noexcept;
        WorldLoadingSystem(const WorldLoadingSystem&) = delete;
        WorldLoadingSystem& operator=(const WorldLoadingSystem&) = delete;

        [[nodiscard]] SceneStageResult maintain(SceneStageContext&) noexcept;
        [[nodiscard]] WorldLoadingResult<void> replaceSource(process::world_loading::WorldStorageSource);
        [[nodiscard]] WorldLoadingResult<void> retry(partition::PartitionOrdinal);
        [[nodiscard]] const WorldLoadingResult<void>& status() const noexcept;
        [[nodiscard]] WorldLoadingStatistics statistics() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    [[nodiscard]] LUX_ENGINE_SCENE_WORLD_LOADING_PUBLIC SceneSystemRegistration
    worldLoadingSystemRegistration() noexcept;
} // namespace lux::scene
