#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/process/world_loading/WorldPartitionLoadSender.hpp>
#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/scene/detail/WorldResidencyImpl.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <unordered_map>

namespace lux::scene
{
    namespace ecs = simulation::ecs;
    namespace loading = process::world_loading;
    using Ordinal = partition::PartitionOrdinal;

    error::Error toError(const WorldLoadingFailure& failure) noexcept
    {
        constexpr std::string_view names[]{
            "invalid_configuration",
            "invalid_partition",
            "not_resident",
            "source_busy",
            "capacity",
            "read_failure",
            "cancelled",
            "materialize_failure",
            "task_rejected"
        };
        const auto code = static_cast<std::uint64_t>(failure.code);
        const std::string name =
            "lux.scene.world_loading." + (code < std::size(names) ? std::string(names[code]) : std::string("unknown"));
        const bool retryable =
            failure.code == EWorldLoadingError::SOURCE_BUSY || failure.code == EWorldLoadingError::CAPACITY;
        const auto recovery = retryable ? error::ERecovery::RETRYABLE : error::ERecovery::PERMANENT;
        std::string_view cause_name = "none";
        std::uint64_t cause_code{};
        std::visit(
            [&](const auto& cause) noexcept
            {
                using T = std::decay_t<decltype(cause)>;
                if constexpr (std::is_same_v<T, loading::WorldStorageRuntimeFailure>)
                {
                    cause_name = "storage";
                    cause_code = static_cast<std::uint64_t>(cause.code);
                }
                else if constexpr (std::is_same_v<T, WorldMaterializeFailure>)
                {
                    cause_name = "materialize";
                    cause_code = static_cast<std::uint64_t>(cause.code);
                }
                else if constexpr (std::is_same_v<T, WorldResidencyFailure>)
                {
                    cause_name = "residency";
                    cause_code = static_cast<std::uint64_t>(cause.code);
                }
                else if constexpr (std::is_same_v<T, process::EExecutionError>)
                {
                    cause_name = "execution";
                    cause_code = static_cast<std::uint64_t>(cause);
                }
            },
            failure.cause
        );
        return error::makeError(
            {name + "." + std::string(cause_name),
             "World loading code {0}, partition {1}, cause code {2}",
             recovery,
             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
            {code, failure.partition.value, cause_code}
        );
    }

    namespace
    {
        std::shared_ptr<const world::WorldDescription> worldOwner(const loading::WorldStorageSource& source)
        {
            if (!source)
                return {};
            auto owner = std::make_shared<loading::WorldStorageSource>(source);
            return {owner, &owner->world()};
        }
        WorldLoadingFailure residencyFailure(WorldResidencyFailure failure)
        {
            const auto code = failure.code == EWorldResidencyError::CAPACITY ? EWorldLoadingError::CAPACITY
                              : failure.code == EWorldResidencyError::MATERIALIZE_FAILURE
                                  ? EWorldLoadingError::MATERIALIZE_FAILURE
                                  : EWorldLoadingError::INVALID_PARTITION;
            return {code, failure.partition, failure};
        }
    }

    struct WorldLoadingSystem::Impl final
    {
        struct Read final
        {
            enum class EState : std::uint8_t
            {
                PENDING,
                VALUE,
                ERROR,
                CANCELLED
            };
            std::atomic<EState> state{EState::PENDING};
            std::shared_ptr<const world::WorldPartitionData> data;
            loading::WorldStorageRuntimeFailure error;
            std::stop_source cancel;
            Ordinal partition;
            std::uint64_t source_epoch{};
            bool obsolete{}; // Owner-thread qualification, never accessed by completion.
        };
        ecs::Registry& registry;
        WorldLoadingServices services;
        WorldResidency& residency;
        std::map<std::uint32_t, bool> wanted; // Union: true if at least one demand is required.
        std::vector<std::shared_ptr<Read>> reads;
        ecs::ComponentOperations::MembershipChanges& component_changes;
        entt::scoped_connection construct, update, destroy, entity_destroy;
        WorldLoadingResult<void> result;
        WorldLoadingStatistics stats;
        std::uint64_t source_epoch{1};
        bool demand_dirty{true}, stopping{};

        Impl(ecs::Registry& registry, ecs::ComponentSchemaSet schemas, WorldLoadingServices services)
            : registry(registry), services(std::move(services)), residency(registry.ctx().emplace<WorldResidency>(
                                                                     registry,
                                                                     worldOwner(this->services.source),
                                                                     std::move(schemas),
                                                                     this->services.limits.residency
                                                                 )),
              component_changes(registry.storage<entt::reactive>(entt::hashed_string{"scene.loading.membership"}.value()
              )),
              construct(registry.on_construct<Observer>().connect<&Impl::changed>(*this)),
              update(registry.on_update<Observer>().connect<&Impl::changed>(*this)),
              destroy(registry.on_destroy<Observer>().connect<&Impl::changed>(*this)),
              entity_destroy(registry.on_destroy<ecs::Entity>().connect<&Impl::entityDestroyed>(*this))
        {
            for (const auto& schema : this->residency.impl_->schemas.all())
            {
                if (schema.semantic_kind != ecs::EComponentSemanticKind::RUNTIME_DERIVED)
                {
                    schema.operations.trackMembership(component_changes);
                }
            }
            const auto& limits = this->services.limits;
            if (!this->services.source || !limits.in_flight || !limits.residency.partitions ||
                !limits.residency.entities || !limits.read_bytes || limits.read_bytes > limits.staging_bytes)
            {
                result = lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::INVALID_CONFIGURATION});
            }
            if (result && !this->services.initial_partitions.empty())
            {
                refreshDemands();
                for (const auto& [ordinal, required] : wanted)
                    if (required &&
                        std::ranges::none_of(this->services.initial_partitions, [ordinal](const auto& source) {
                            return source && source->partition().value == ordinal;
                        }))
                        fail({EWorldLoadingError::NOT_RESIDENT, {ordinal}});
                if (result)
                {
                    const auto loaded = residency.impl_->adopt(registry, this->services.initial_partitions);
                    if (!loaded)
                        fail(residencyFailure(loaded.error()));
                    else
                        stats.adopted += this->services.initial_partitions.size();
                }
                this->services.initial_partitions.clear();
            }
        }

        ~Impl()
        {
            component_changes.reset();
            component_changes.clear();
            construct.release();
            update.release();
            destroy.release();
            entity_destroy.release();
            for (const auto& read : reads)
            {
                read->cancel.request_stop();
            }
            // Read senders retain source, operation/result and provider code. No callback
            // borrows this instance. Registry owns installed components until Scene exit.
        }
        void changed(ecs::Registry&, ecs::Entity) noexcept
        {
            demand_dirty = true;
        }
        void entityDestroyed(ecs::Registry&, ecs::Entity entity) noexcept
        {
            component_changes.remove(entity);
            residency.impl_->entityDestroyed(entity);
        }

        void fail(WorldLoadingFailure failure)
        {
            if (result)
            {
                result = lux::cxx::unexpected(std::move(failure));
            }
        }
        void recount()
        {
            stats.in_flight = stats.reserved_read_bytes = stats.staged_bytes = 0;
            for (const auto& read : reads)
            {
                if (read->state.load(std::memory_order_acquire) == Read::EState::PENDING)
                {
                    ++stats.in_flight;
                    stats.reserved_read_bytes += services.limits.read_bytes;
                }
                else if (read->state.load(std::memory_order_acquire) == Read::EState::VALUE)
                {
                    stats.staged_bytes += read->data->retainedBytes();
                }
            }
        }
        bool requiredMissing() const
        {
            for (const auto& [ordinal, required] : wanted)
            {
                if (required && !residency.source({ordinal}))
                {
                    return true;
                }
            }
            return false;
        }
        SceneStageResult progress() const
        {
            const bool missing = requiredMissing();
            if (!result && result.error().code != EWorldLoadingError::CAPACITY &&
                (missing || result.error().code == EWorldLoadingError::INVALID_PARTITION))
            {
                return lux::cxx::unexpected(
                    SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, {}, toError(result.error())}
                );
            }
            return missing ? ESceneProgress::PENDING : ESceneProgress::COMPLETE;
        }
        void refreshDemands()
        {
            if (!demand_dirty)
            {
                return;
            }
            demand_dirty = false;
            std::map<std::uint32_t, bool> next;
            WorldLoadingResult<void> validation;
            auto add = [&](Ordinal partition, bool required) {
                if (partition.value >= services.source.world().partitionCount())
                {
                    validation =
                        lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::INVALID_PARTITION, partition});
                    return;
                }
                next[partition.value] |= required;
            };
            for (const auto partition : services.bootstrap)
            {
                add(partition, true);
            }
            for (const auto entity : registry.view<const Observer>())
            {
                const auto& observer = registry.get<Observer>(entity);
                for (const auto partition : observer.partitions)
                {
                    add(partition, observer.required);
                }
            }
            if (!validation)
            {
                fail(validation.error());
                return;
            }
            if (!result && result.error().code == EWorldLoadingError::INVALID_PARTITION)
            {
                result = {};
            }
            if (next == wanted)
            {
                return;
            }
            wanted = std::move(next);
            ++stats.demand_revision;
            // A new demand revision does not invalidate a still-needed, same-source read.
            if (!result && result.error().code != EWorldLoadingError::INVALID_CONFIGURATION)
            {
                result = {};
            }
            for (const auto& read : reads)
            {
                if (!wanted.contains(read->partition.value))
                {
                    read->obsolete = true;
                    read->cancel.request_stop();
                }
            }
        }

        void adopt(SceneStageContext& context)
        {
            if (!context.allow_structure)
            {
                return;
            }
            std::vector<std::shared_ptr<Read>> batch;
            const bool required_batch = requiredMissing();
            for (const auto& [ordinal, required] : wanted)
            {
                if (residency.source({ordinal}) || (required_batch && !required))
                {
                    continue;
                }
                const auto found = std::ranges::find_if(reads, [&](const auto& read) {
                    return !read->obsolete && read->source_epoch == source_epoch && read->partition.value == ordinal;
                });
                if (found == reads.end() || (*found)->state.load(std::memory_order_acquire) == Read::EState::PENDING)
                {
                    return; // One complete explicit set; no partition installs prematurely.
                }
                const auto& read = *found;
                const auto state = read->state.load(std::memory_order_acquire);
                if (state != Read::EState::VALUE)
                {
                    fail(
                        state == Read::EState::ERROR
                            ? WorldLoadingFailure{EWorldLoadingError::READ_FAILURE, read->partition, read->error}
                            : WorldLoadingFailure{EWorldLoadingError::CANCELLED, read->partition}
                    );
                    return;
                }
                batch.push_back(read);
            }
            if (batch.empty())
            {
                return;
            }
            std::vector<std::shared_ptr<const world::WorldPartitionData>> sources;
            sources.reserve(batch.size());
            for (const auto& read : batch)
                sources.push_back(read->data);
            const auto loaded = residency.impl_->adopt(registry, sources);
            if (!loaded)
            {
                fail(residencyFailure(loaded.error()));
                ++stats.rejected;
                return;
            }
            for (const auto& read : batch)
                read->obsolete = true;
            stats.adopted += batch.size();
            std::erase_if(reads, [](const auto& read) {
                return read->obsolete && read->state.load(std::memory_order_acquire) != Read::EState::PENDING;
            });
            result = {};
            context.publication_needed = true;
            recount();
        }

        SceneStageResult maintain(SceneStageContext& context)
        {
            if (!services.source || (!result && result.error().code == EWorldLoadingError::INVALID_CONFIGURATION))
            {
                return lux::cxx::unexpected(
                    SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, {}, toError(result.error())}
                );
            }
            if (context.stop.stop_requested())
            {
                stopping = true;
                for (const auto& read : reads)
                {
                    read->obsolete = true;
                    read->cancel.request_stop();
                }
            }
            std::erase_if(reads, [](const auto& read) {
                return read->obsolete && read->state.load(std::memory_order_acquire) != Read::EState::PENDING;
            });
            residency.impl_->updateAccounting(registry, component_changes);
            recount();
            if (stopping)
            {
                return reads.empty() ? ESceneProgress::COMPLETE : ESceneProgress::PENDING;
            }
            refreshDemands();
            if (context.allow_structure)
            {
                const auto unloaded = residency.impl_->unload(registry, wanted);
                stats.unloaded += unloaded;
                context.publication_needed |= unloaded != 0;
            }
            if (!result && result.error().code == EWorldLoadingError::CAPACITY)
            {
                result = {}; // Capacity can recover after a protection token is released.
            }
            if (!result)
            {
                return progress();
            }
            // Required demand is admitted before optional work, preserving finite starts.
            for (const bool required_pass : {true, false})
            {
                for (const auto& [ordinal, required] : wanted)
                {
                    if (required != required_pass)
                    {
                        continue;
                    }
                    if (residency.source({ordinal}) || std::ranges::any_of(reads, [ordinal, this](const auto& read) {
                            return read->partition.value == ordinal && read->source_epoch == source_epoch;
                        }))
                    {
                        continue;
                    }
                    const auto& limits = services.limits;
                    if (stats.in_flight == limits.in_flight)
                    {
                        break;
                    }
                    if (stats.reserved_read_bytes + stats.staged_bytes > limits.staging_bytes ||
                        limits.read_bytes > limits.staging_bytes - stats.reserved_read_bytes - stats.staged_bytes)
                    {
                        fail({EWorldLoadingError::CAPACITY, {ordinal}});
                        break;
                    }
                    auto read = std::make_shared<Read>();
                    read->partition = {ordinal};
                    read->source_epoch = source_epoch;
                    auto value = stdexec::then(
                        loading::loadWorldPartition(
                            services.source,
                            read->partition,
                            limits.read_bytes,
                            read->cancel.get_token()
                        ),
                        [](world::WorldPartitionData data) noexcept
                            -> lux::cxx::expected<
                                std::shared_ptr<const world::WorldPartitionData>,
                                loading::WorldStorageRuntimeFailure> {
                            return std::make_shared<const world::WorldPartitionData>(std::move(data));
                        }
                    );
                    auto task = stdexec::upon_error(
                        std::move(value),
                        [](loading::WorldStorageRuntimeFailure failure) noexcept
                            -> lux::cxx::expected<
                                std::shared_ptr<const world::WorldPartitionData>,
                                loading::WorldStorageRuntimeFailure> { return lux::cxx::unexpected(failure); }
                    );
                    auto accepted = services.tasks.submit(
                        {"Load world partition", "world"},
                        [sender = std::move(task)](process::TaskReporter) mutable noexcept {
                            return std::move(sender);
                        },
                        [read, execution = &services.tasks.execution()](auto&& completed) noexcept {
                            if (completed)
                            {
                                read->data = std::move(*completed);
                                read->state.store(Read::EState::VALUE, std::memory_order_release);
                            }
                            else if (const auto* failure = completed.error().domainFailure())
                            {
                                read->error = *failure;
                                read->state.store(Read::EState::ERROR, std::memory_order_release);
                            }
                            else
                                read->state.store(Read::EState::CANCELLED, std::memory_order_release);
                            execution->wake();
                        }
                    );
                    if (!accepted)
                    {
                        fail({EWorldLoadingError::TASK_REJECTED, read->partition, accepted.error()});
                        break;
                    }
                    reads.push_back(std::move(read));
                    ++stats.reads;
                    recount();
                }
            }
            if (stats.staged_bytes > services.limits.staging_bytes)
            {
                fail({EWorldLoadingError::CAPACITY});
            }
            if (result)
            {
                adopt(context);
            }
            return progress();
        }
        WorldLoadingResult<void> replaceSource(loading::WorldStorageSource source)
        {
            if (!source)
            {
                return lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::INVALID_CONFIGURATION});
            }
            if (residency.statistics().resident_partitions != 0)
            {
                return lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::SOURCE_BUSY});
            }
            if (source_epoch == (std::numeric_limits<std::uint64_t>::max)())
            {
                return lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::CAPACITY});
            }
            for (const auto& read : reads)
            {
                read->obsolete = true;
                read->cancel.request_stop();
            }
            residency.impl_->world = worldOwner(source);
            services.source = std::move(source);
            ++source_epoch;
            demand_dirty = true;
            result = {};
            return {};
        }
        WorldLoadingResult<void> retry(Ordinal partition)
        {
            if (!wanted.contains(partition.value))
            {
                return lux::cxx::unexpected(WorldLoadingFailure{EWorldLoadingError::INVALID_PARTITION, partition});
            }
            for (const auto& read : reads)
            {
                const bool is_requested_partition = read->partition == partition;
                const bool is_unfinished_partition =
                    is_requested_partition && read->state.load(std::memory_order_acquire) != Impl::Read::EState::VALUE;
                if (is_unfinished_partition)
                {
                    read->obsolete = true;
                    read->cancel.request_stop();
                }
            }
            result = {};
            return {};
        }
    };

    WorldLoadingSystem::WorldLoadingSystem(
        ecs::Registry& registry,
        ecs::ComponentSchemaSet schemas,
        WorldLoadingServices services
    )
        : impl_(std::make_unique<Impl>(registry, std::move(schemas), std::move(services)))
    {}
    WorldLoadingSystem::~WorldLoadingSystem() noexcept = default;
    SceneStageResult WorldLoadingSystem::maintain(SceneStageContext& context) noexcept
    {
        return impl_->maintain(context);
    }
    WorldLoadingResult<void> WorldLoadingSystem::replaceSource(loading::WorldStorageSource source)
    {
        return impl_->replaceSource(std::move(source));
    }
    WorldLoadingResult<void> WorldLoadingSystem::retry(Ordinal partition)
    {
        return impl_->retry(partition);
    }
    const WorldLoadingResult<void>& WorldLoadingSystem::status() const noexcept
    {
        return impl_->result;
    }
    WorldLoadingStatistics WorldLoadingSystem::statistics() const noexcept
    {
        return impl_->stats;
    }
    SceneSystemRegistration worldLoadingSystemRegistration() noexcept
    {
        static constexpr std::array requirements{SceneSystemRequirementSpec{
            "world_loading",
            "lux.world.loading",
            lux::cxx::typeToken<WorldLoadingServices>(),
            false
        }};
        return {
            .type = system::systemTypeId(WorldLoadingSystem::Description.canonical_name),
            .cpp_type = lux::cxx::typeToken<WorldLoadingSystem>(),
            .description = &WorldLoadingSystem::Description,
            .configuration = worldLoadingConfigurationCodec(),
            .requirements = requirements,
            .install = +[](SceneSystemInstaller& builder,
                           SceneSystemDescription input) noexcept -> lux::cxx::expected<void, SceneSystemBuildFailure> {
                auto* services = builder.require<WorldLoadingServices>(input.instanceId(), "world_loading");
                if (!services || !services->source)
                {
                    return lux::cxx::unexpected(
                        SceneSystemBuildFailure{ESceneSystemBuildError::MISSING_REQUIREMENT, input.instanceId()}
                    );
                }
                auto configuration = builder.decodeConfiguration<WorldLoadingConfiguration>(input);
                if (!configuration)
                {
                    return lux::cxx::unexpected(configuration.error());
                }
                auto configured = *services;
                configured.bootstrap.insert(
                    configured.bootstrap.end(),
                    configuration->bootstrap.begin(),
                    configuration->bootstrap.end()
                );
                auto instance = builder.emplaceSystem<WorldLoadingSystem>(
                    input.instanceId(),
                    builder.registry(),
                    builder.components(),
                    std::move(configured)
                );
                if (!instance)
                {
                    return lux::cxx::unexpected(instance.error());
                }
                if (!(**instance).status())
                {
                    return lux::cxx::unexpected(SceneSystemBuildFailure{
                        ESceneSystemBuildError::INVALID_DESCRIPTION,
                        input.instanceId(),
                        {},
                        0,
                        {},
                        toError((**instance).status().error())
                    });
                }
                return builder.addMaintenanceTask<WorldLoadingSystem>(
                    input.instanceId(),
                    [](WorldLoadingSystem& system, SceneStageContext& context) noexcept {
                        return system.maintain(context);
                    }
                );
            }
        };
    }
} // namespace lux::scene
