#include <lux/engine/runtime/extensions/SceneContributions.hpp>

#include <lux/engine/ecs/SceneServices.hpp>
#include <lux/engine/events/DomainEvents.hpp>

#include <moodycamel/concurrentqueue.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <optional>

namespace lux::runtime
{
    namespace
    {
        using TicketPublisher = lux::extensions::OperationTicketPublisher<
            ESceneContributionActivationPhase,
            ESceneContributionActivationError,
            SceneContributionActivationResult>;

        [[nodiscard]] bool sameContribution(
            const lux::extensions::ContributionId& lhs,
            lux::extensions::ContributionIdView rhs) noexcept
        {
            return lux::extensions::sameStableId(lhs.view(), rhs);
        }

    }

    SceneContributionBatchBuilder::SceneContributionBatchBuilder() = default;
    SceneContributionBatchBuilder::~SceneContributionBatchBuilder() = default;
    SceneContributionBatchBuilder::SceneContributionBatchBuilder(
        SceneContributionBatchBuilder&&) noexcept = default;
    SceneContributionBatchBuilder& SceneContributionBatchBuilder::operator=(
        SceneContributionBatchBuilder&&) noexcept = default;

    lux::cxx::expected<void, ESceneContributionCatalogError> SceneContributionCatalog::add(
        SceneContributionDescriptor descriptor)
    {
        std::vector<SceneContributionDescriptor> batch;
        batch.push_back(std::move(descriptor));
        return addBatch(std::move(batch));
    }

    lux::cxx::expected<void, ESceneContributionCatalogError>
    SceneContributionCatalog::validateBatch(
        std::span<const SceneContributionDescriptor> descriptors)
        const noexcept
    {
        const auto locate = [this, descriptors](
            lux::extensions::ContributionIdView id) noexcept
            -> ESceneContributionCatalogError
        {
            for (const auto& existing : descriptors_)
            {
                if (lux::extensions::stableIdCollision(
                        existing.id.view(), id))
                    return ESceneContributionCatalogError::ID_COLLISION;
                if (lux::extensions::sameStableId(existing.id.view(), id))
                    return ESceneContributionCatalogError::DUPLICATE_CONTRIBUTION;
            }
            for (const auto& pending : descriptors)
            {
                if (lux::extensions::stableIdCollision(
                        pending.id.view(), id))
                    return ESceneContributionCatalogError::ID_COLLISION;
                if (lux::extensions::sameStableId(pending.id.view(), id))
                    return ESceneContributionCatalogError::DUPLICATE_CONTRIBUTION;
            }
            return ESceneContributionCatalogError::MISSING_DEPENDENCY;
        };

        for (std::size_t index = 0u; index < descriptors.size(); ++index)
        {
            const auto& descriptor = descriptors[index];
            if (!descriptor.id.isValid() || !descriptor.provider.isValid() ||
                !lux::extensions::isCanonicalStableName(
                    descriptor.id.name()) ||
                !lux::extensions::isCanonicalStableName(
                    descriptor.provider.name()))
            {
                return lux::cxx::unexpected(
                    ESceneContributionCatalogError::INVALID_DESCRIPTOR);
            }
            if (!descriptor.build)
                return lux::cxx::unexpected(
                    ESceneContributionCatalogError::MISSING_BUILD_CALLBACK);

            for (const auto& existing : descriptors_)
            {
                if (lux::extensions::stableIdCollision(
                        existing.id.view(), descriptor.id.view()))
                    return lux::cxx::unexpected(
                        ESceneContributionCatalogError::ID_COLLISION);
                if (lux::extensions::sameStableId(
                        existing.id.view(), descriptor.id.view()))
                    return lux::cxx::unexpected(
                        ESceneContributionCatalogError::DUPLICATE_CONTRIBUTION);
            }
            for (std::size_t other = 0u; other < index; ++other)
            {
                if (lux::extensions::stableIdCollision(
                        descriptors[other].id.view(), descriptor.id.view()))
                    return lux::cxx::unexpected(
                        ESceneContributionCatalogError::ID_COLLISION);
                if (lux::extensions::sameStableId(
                        descriptors[other].id.view(), descriptor.id.view()))
                    return lux::cxx::unexpected(
                        ESceneContributionCatalogError::DUPLICATE_CONTRIBUTION);
            }

            for (const auto& dependency :
                 descriptor.required_contributions)
            {
                const auto status = locate(dependency.view());
                if (status == ESceneContributionCatalogError::ID_COLLISION)
                    return lux::cxx::unexpected(status);
                if (status == ESceneContributionCatalogError::MISSING_DEPENDENCY)
                    return lux::cxx::unexpected(status);
                // DUPLICATE_CONTRIBUTION means the exact dependency exists.
            }
        }
        return {};
    }

    lux::cxx::expected<void, ESceneContributionCatalogError>
    SceneContributionCatalog::addBatch(
        std::vector<SceneContributionDescriptor> descriptors)
    {
        if (auto checked = validateBatch(descriptors); !checked)
            return checked;
        descriptors_.reserve(descriptors_.size() + descriptors.size());
        for (auto& descriptor : descriptors)
            descriptors_.push_back(std::move(descriptor));
        return {};
    }

    SceneContributionDescriptor* SceneContributionCatalog::find(
        lux::extensions::ContributionIdView id) noexcept
    {
        for (auto& descriptor : descriptors_)
            if (sameContribution(descriptor.id, id))
                return &descriptor;
        return nullptr;
    }

    const SceneContributionDescriptor* SceneContributionCatalog::find(
        lux::extensions::ContributionIdView id) const noexcept
    {
        for (const auto& descriptor : descriptors_)
            if (sameContribution(descriptor.id, id))
                return &descriptor;
        return nullptr;
    }

    std::span<const SceneContributionDescriptor> SceneContributionCatalog::all()
        const noexcept
    {
        return descriptors_;
    }

    lux::cxx::expected<void, SceneContributionAssemblyFailure>
    SceneContributionCatalog::assembleDefaults(
        lux::ecs::ScheduleBuilder& assembly,
        std::span<const lux::extensions::ContributionIdView> selected)
    {
        std::vector<SceneContributionSelection> configured;
        configured.reserve(selected.size());
        for (const auto id : selected)
        {
            const auto* descriptor = find(id);
            configured.push_back(SceneContributionSelection{
                lux::extensions::ContributionId{id.name()},
                descriptor ? descriptor->default_config
                           : ContributionConfig{}});
        }
        return assemble(assembly, configured);
    }

    lux::cxx::expected<void, SceneContributionAssemblyFailure>
    SceneContributionCatalog::assemble(
        lux::ecs::ScheduleBuilder& assembly,
        std::span<const SceneContributionSelection> selected)
    {
        auto staged = stageBootstrap(assembly, selected);
        if (!staged)
            return lux::cxx::unexpected(staged.error());
        return {};
    }

    lux::cxx::expected<
        SceneContributionBootstrap,
        SceneContributionAssemblyFailure>
    SceneContributionCatalog::stageBootstrap(
        lux::ecs::ScheduleBuilder& assembly,
        std::span<const SceneContributionSelection> selected)
    {
        struct AssemblyRollback final
        {
            lux::ecs::ScheduleBuilder* builder{};
            lux::ecs::ScheduleBuilder::Checkpoint checkpoint;
            bool committed{false};

            ~AssemblyRollback() noexcept
            {
                if (!committed && builder)
                    static_cast<void>(builder->rollbackTo(checkpoint));
            }
        } rollback{&assembly, assembly.checkpoint()};
        if (!rollback.checkpoint.valid())
        {
            return lux::cxx::unexpected(SceneContributionAssemblyFailure{
                ESceneContributionAssemblyError::BUILD_FAILED,
                {},
                SceneContributionBuildFailure{
                    ESceneContributionBuildError::BUILD_REJECTED}});
        }

        std::vector<SceneContributionDescriptor*> order;
        std::vector<lux::extensions::ContributionId> visiting;
        std::vector<lux::extensions::ContributionId> visited;

        const auto fail = [](
            ESceneContributionAssemblyError code,
            lux::extensions::ContributionIdView id,
            SceneContributionBuildFailure build = {})
        {
            return lux::cxx::unexpected(SceneContributionAssemblyFailure{
                code,
                lux::extensions::ContributionId{id.name()},
                build});
        };

        const auto visit = [&](auto&& self,
                               lux::extensions::ContributionIdView id,
                               bool dependency)
            -> lux::cxx::expected<void, SceneContributionAssemblyFailure>
        {
            if (std::ranges::any_of(
                    visited,
                    [id](const auto& value) noexcept
                    {
                        return sameContribution(value, id);
                    }))
                return {};
            if (std::ranges::any_of(
                    visiting,
                    [id](const auto& value) noexcept
                    {
                        return sameContribution(value, id);
                    }))
                return fail(
                    ESceneContributionAssemblyError::DEPENDENCY_CYCLE,
                    id);

            auto* descriptor = find(id);
            if (!descriptor)
                return fail(
                    dependency
                        ? ESceneContributionAssemblyError::MISSING_DEPENDENCY
                        : ESceneContributionAssemblyError::UNKNOWN_CONTRIBUTION,
                    id);
            visiting.push_back(descriptor->id);
            for (const auto& dependency : descriptor->required_contributions)
            {
                if (auto result = self(self, dependency.view(), true); !result)
                    return result;
            }
            visiting.pop_back();
            visited.push_back(descriptor->id);
            order.push_back(descriptor);
            return {};
        };

        for (const auto& selection : selected)
        {
            if (auto result = visit(
                    visit, selection.id.view(), false); !result)
                return lux::cxx::unexpected(result.error());
        }

        SceneContributionBootstrap bootstrap;
        bootstrap.builder_ = &assembly;
        bootstrap.entries_.reserve(order.size());
        SceneContributionBatchBuilder builder{assembly};
        SceneContributionBuildContext context{
            0u,
            assembly.baseServices()};
        for (auto* descriptor : order)
        {
            const auto first = assembly.checkpoint();
            const auto services_before = builder.published_service_count_;
            for (const auto required : descriptor->required_services)
            {
                if (!builder.containsService(required, context))
                    return fail(
                        ESceneContributionAssemblyError::
                            REQUIRED_SERVICE_MISSING,
                        descriptor->id.view(),
                        SceneContributionBuildFailure{
                            ESceneContributionBuildError::MISSING_SERVICE,
                            required});
            }
            for (const auto provided : descriptor->provided_services)
            {
                if (builder.containsService(provided, context))
                    return fail(
                        ESceneContributionAssemblyError::SERVICE_CONFLICT,
                        descriptor->id.view(),
                        SceneContributionBuildFailure{
                            ESceneContributionBuildError::DUPLICATE_SERVICE,
                            provided});
            }

            const auto explicit_config = std::ranges::find_if(
                selected,
                [descriptor](const SceneContributionSelection& value)
                {
                    return sameContribution(
                        value.id, descriptor->id.view());
                });
            auto config = explicit_config == selected.end()
                ? descriptor->default_config
                : explicit_config->config;
            if (config.schema_version != descriptor->config_schema_version)
                return fail(
                    ESceneContributionAssemblyError::CONFIG_VERSION_MISMATCH,
                    descriptor->id.view());
            const auto built = descriptor->build(
                builder,
                context,
                config);
            if (!built)
                return fail(
                    ESceneContributionAssemblyError::BUILD_FAILED,
                    descriptor->id.view(),
                    built.error());
            const auto services_after = builder.published_service_count_;
            if (services_after - services_before !=
                descriptor->provided_services.size())
            {
                return fail(
                    ESceneContributionAssemblyError::SERVICE_CONFLICT,
                    descriptor->id.view(),
                    SceneContributionBuildFailure{
                        services_after - services_before <
                                descriptor->provided_services.size()
                            ? ESceneContributionBuildError::MISSING_SERVICE
                            : ESceneContributionBuildError::DUPLICATE_SERVICE});
            }
            for (const auto provided : descriptor->provided_services)
            {
                if (!builder.containsService(provided, context))
                    return fail(
                        ESceneContributionAssemblyError::SERVICE_CONFLICT,
                        descriptor->id.view(),
                        SceneContributionBuildFailure{
                            ESceneContributionBuildError::MISSING_SERVICE,
                            provided});
            }
            const auto last = assembly.checkpoint();
            bootstrap.entries_.push_back(SceneContributionBootstrap::Entry{
                descriptor->id,
                std::move(config),
                EActivationPersistence::SCENE,
                explicit_config != selected.end(),
                first,
                last,
                descriptor->module});
        }
        rollback.committed = true;
        return std::move(bootstrap);
    }

    namespace detail
    {
        struct SceneContributionBatchBuilderAccess final
        {
            [[nodiscard]] static auto& systems(
                SceneContributionBatchBuilder& builder) noexcept
            {
                return builder.systems_;
            }

            [[nodiscard]] static const auto& services(
                const SceneContributionBatchBuilder& builder) noexcept
            {
                return builder.services_;
            }

            [[nodiscard]] static std::size_t serviceCount(
                const SceneContributionBatchBuilder& builder) noexcept
            {
                return builder.published_service_count_;
            }

            [[nodiscard]] static auto takeServices(
                SceneContributionBatchBuilder& builder) noexcept
            {
                return std::move(builder.services_);
            }
        };

        enum class ESceneContributionCommandKind : std::uint8_t
        {
            ENABLE,
            DISABLE
        };

        struct SceneContributionCommand final
        {
            SceneContributionCommand(
                ESceneContributionCommandKind value_kind,
                lux::extensions::ContributionId value_id,
                ContributionConfig value_config,
                EActivationPersistence value_persistence,
                EContributionDisableMode value_disable_mode,
                std::uint64_t value_generation)
                : kind(value_kind)
                , id(std::move(value_id))
                , config(std::move(value_config))
                , persistence(value_persistence)
                , disable_mode(value_disable_mode)
                , accounted_bytes(config.bytes.size())
                , publisher(ESceneContributionActivationPhase::QUEUED, value_generation)
            {}

            ESceneContributionCommandKind kind{ESceneContributionCommandKind::ENABLE};
            lux::extensions::ContributionId id;
            ContributionConfig config;
            EActivationPersistence persistence{EActivationPersistence::SCENE};
            EContributionDisableMode disable_mode{
                EContributionDisableMode::REJECT_DEPENDENTS};
            std::size_t accounted_bytes{0u};
            TicketPublisher publisher;
        };

        struct SceneContributionEndpoint final
        {
            explicit SceneContributionEndpoint(SceneContributionQueueConfig value)
                : config(value)
                , queue(value.capacity)
            {}

            [[nodiscard]] SceneContributionOperationTicket submit(
                ESceneContributionCommandKind kind,
                lux::extensions::ContributionIdView id,
                ContributionConfig config_value,
                EActivationPersistence persistence,
                EContributionDisableMode disable_mode)
            {
                const auto generation = next_generation.fetch_add(
                    1u,
                    std::memory_order_relaxed);
                SceneContributionCommand command{
                    kind,
                    lux::extensions::ContributionId{id.name()},
                    std::move(config_value),
                    persistence,
                    disable_mode,
                    generation};
                auto ticket = command.publisher.ticket();
                if (!admission_open.load(std::memory_order_acquire))
                {
                    command.publisher.fail(ESceneContributionActivationError::STOPPING);
                    return ticket;
                }
                if (!id.isValid())
                {
                    command.publisher.fail(
                        ESceneContributionActivationError::UNKNOWN_CONTRIBUTION);
                    return ticket;
                }

                auto current = bytes_inflight.load(std::memory_order_relaxed);
                for (;;)
                {
                    if (command.accounted_bytes > config.byte_budget -
                            std::min(current, config.byte_budget))
                    {
                        command.publisher.fail(
                            ESceneContributionActivationError::BYTE_BUDGET_EXHAUSTED);
                        return ticket;
                    }
                    if (bytes_inflight.compare_exchange_weak(
                            current,
                            current + command.accounted_bytes,
                            std::memory_order_acq_rel,
                            std::memory_order_relaxed))
                        break;
                }
                const auto accounted_bytes = command.accounted_bytes;
                auto terminal = command.publisher;
                if (!queue.try_enqueue(std::move(command)))
                {
                    bytes_inflight.fetch_sub(
                        accounted_bytes,
                        std::memory_order_release);
                    terminal.fail(ESceneContributionActivationError::QUEUE_FULL);
                }
                return ticket;
            }

            SceneContributionQueueConfig config;
            moodycamel::ConcurrentQueue<SceneContributionCommand> queue;
            std::atomic<std::size_t> bytes_inflight{0u};
            std::atomic<std::uint64_t> next_generation{1u};
            std::atomic<bool> admission_open{true};
        };
    }

    SceneContributions::SceneContributions(
        std::shared_ptr<detail::SceneContributionEndpoint> endpoint) noexcept
        : endpoint_(std::move(endpoint))
    {}

    SceneContributionOperationTicket SceneContributions::requestEnable(
        lux::extensions::ContributionIdView id,
        ContributionConfig config,
        EActivationPersistence persistence) const
    {
        if (!endpoint_)
        {
            TicketPublisher publisher(ESceneContributionActivationPhase::QUEUED, 0u);
            auto ticket = publisher.ticket();
            publisher.fail(ESceneContributionActivationError::STOPPING);
            return ticket;
        }
        return endpoint_->submit(
            detail::ESceneContributionCommandKind::ENABLE,
            id,
            std::move(config),
            persistence,
            EContributionDisableMode::REJECT_DEPENDENTS);
    }

    SceneContributionOperationTicket SceneContributions::requestDisable(
        lux::extensions::ContributionIdView id,
        EContributionDisableMode mode) const
    {
        if (!endpoint_)
        {
            TicketPublisher publisher(ESceneContributionActivationPhase::QUEUED, 0u);
            auto ticket = publisher.ticket();
            publisher.fail(ESceneContributionActivationError::STOPPING);
            return ticket;
        }
        return endpoint_->submit(
            detail::ESceneContributionCommandKind::DISABLE,
            id,
            {},
            EActivationPersistence::TRANSIENT,
            mode);
    }

    SceneContributions::operator bool() const noexcept
    {
        return endpoint_ &&
            endpoint_->admission_open.load(std::memory_order_acquire);
    }

    struct SceneContributionHost::Impl final
    {
        struct Active final
        {
            lux::extensions::ContributionId id;
            ContributionConfig config;
            EActivationPersistence persistence{
                EActivationPersistence::SCENE};
            bool root{false};
            lux::ecs::InstalledSystemBatch systems;
            lux::ecs::InstalledSceneServiceBatch services;
            std::uint64_t generation{0u};
            lux::extensions::ModuleLease module;
        };

        Impl(
            lux::ecs::Schedule& value_schedule,
            lux::ecs::SceneServices& value_services,
            SceneContributionCatalog& value_catalog,
            lux::events::DomainEvents* value_events,
            SceneContributionQueueConfig queue)
            : schedule(value_schedule)
            , services(value_services)
            , catalog(value_catalog)
            , events(value_events)
            , endpoint(std::make_shared<detail::SceneContributionEndpoint>(queue))
            , scene_identity(allocateSceneIdentity())
        {}

        [[nodiscard]] static std::uint64_t allocateSceneIdentity() noexcept
        {
            static std::atomic<std::uint64_t> next{1u};
            return next.fetch_add(1u, std::memory_order_relaxed);
        }

        [[nodiscard]] std::size_t findActive(
            lux::extensions::ContributionIdView id) const noexcept
        {
            for (std::size_t index = 0u; index < active.size(); ++index)
                if (sameContribution(active[index].id, id))
                    return index;
            return active.size();
        }

        void publishFact(const Active& value, bool is_active)
        {
            if (events)
            {
                events->publish(SceneContributionStateChanged{
                    value.id,
                    value.generation,
                    is_active});
            }
        }

        [[nodiscard]] ESceneContributionActivationError removeAt(std::size_t index)
        {
            if (index >= active.size())
                return ESceneContributionActivationError::NOT_ACTIVE;
            auto& value = active[index];
            if (value.systems.valid())
            {
                if (!schedule.requestBatchClose(value.systems))
                    return ESceneContributionActivationError::SCHEDULE_REJECTED;
                const auto close_state =
                    schedule.batchCloseState(value.systems);
                if (!close_state.valid || !close_state.complete)
                    return ESceneContributionActivationError::SCHEDULE_REJECTED;
                auto removed = schedule.removeBatch(std::move(value.systems));
                if (!removed)
                    return ESceneContributionActivationError::SCHEDULE_REJECTED;
            }
            publishFact(value, false);
            value.services.reset();
            active.erase(active.begin() + static_cast<std::ptrdiff_t>(index));
            return ESceneContributionActivationError::NONE;
        }

        [[nodiscard]] bool dependsOn(
            const Active& candidate,
            lux::extensions::ContributionIdView dependency) const noexcept
        {
            const auto* descriptor = catalog.find(candidate.id.view());
            if (!descriptor)
                return false;
            return std::ranges::any_of(
                descriptor->required_contributions,
                [dependency](const auto& required) noexcept
                {
                    return lux::extensions::sameStableId(
                        required.view(), dependency);
                });
        }

        [[nodiscard]] std::optional<ESceneContributionActivationError> enable(
            detail::SceneContributionCommand& command)
        {
            command.publisher.setPhase(
                ESceneContributionActivationPhase::RESOLVING_DEPENDENCIES);
            std::vector<SceneContributionDescriptor*> order;
            std::vector<lux::extensions::ContributionId> visiting;
            std::vector<lux::extensions::ContributionId> visited;

            const auto visit = [&](auto&& self,
                                   lux::extensions::ContributionIdView id)
                -> std::optional<ESceneContributionActivationError>
            {
                if (findActive(id) != active.size())
                    return std::nullopt;
                if (std::ranges::any_of(
                        visiting,
                        [id](const auto& value) noexcept
                        {
                            return sameContribution(value, id);
                        }))
                    return ESceneContributionActivationError::DEPENDENCY_CYCLE;
                if (std::ranges::any_of(
                        visited,
                        [id](const auto& value) noexcept
                        {
                            return sameContribution(value, id);
                        }))
                    return std::nullopt;
                auto* descriptor = catalog.find(id);
                if (!descriptor)
                    return ESceneContributionActivationError::MISSING_DEPENDENCY;
                visiting.push_back(descriptor->id);
                for (const auto& dependency :
                     descriptor->required_contributions)
                {
                    if (auto failure = self(self, dependency.view()))
                        return failure;
                }
                visiting.pop_back();
                visited.push_back(descriptor->id);
                order.push_back(descriptor);
                return std::nullopt;
            };

            if (auto failure = visit(visit, command.id.view()))
                return failure;
            if (order.empty())
            {
                const auto existing = findActive(command.id.view());
                if (existing != active.size())
                {
                    active[existing].root = true;
                    active[existing].persistence = command.persistence;
                }
                return std::nullopt;
            }

            command.publisher.setPhase(ESceneContributionActivationPhase::BUILDING);

            struct PendingActive final
            {
                SceneContributionDescriptor* descriptor{nullptr};
                ContributionConfig config;
                EActivationPersistence persistence{
                    EActivationPersistence::SCENE};
                bool root{false};
                std::size_t system_count{0u};
                std::size_t service_count{0u};
            };

            SceneContributionBatchBuilder builder;
            const SceneContributionBuildContext context{
                scene_identity,
                services};
            std::vector<PendingActive> pending;
            pending.reserve(order.size());

            for (auto* descriptor : order)
            {
                const bool root = lux::extensions::sameStableId(
                    descriptor->id.view(), command.id.view());
                ContributionConfig config = root
                    ? command.config
                    : descriptor->default_config;
                if (config.schema_version == 0u && config.bytes.empty())
                    config = descriptor->default_config;
                if (config.schema_version != descriptor->config_schema_version)
                {
                    return ESceneContributionActivationError::
                        CONFIG_VERSION_MISMATCH;
                }

                for (const auto required : descriptor->required_services)
                {
                    if (!builder.containsService(required, context))
                    {
                        return ESceneContributionActivationError::
                            REQUIRED_SERVICE_MISSING;
                    }
                }
                for (const auto provided : descriptor->provided_services)
                {
                    if (builder.containsService(provided, context))
                    {
                        return ESceneContributionActivationError::
                            SERVICE_CONFLICT;
                    }
                }

                const auto systems_before =
                    detail::SceneContributionBatchBuilderAccess::systems(
                        builder).size();
                const auto services_before =
                    detail::SceneContributionBatchBuilderAccess::serviceCount(
                        builder);
                const auto built = descriptor->build(builder, context, config);
                if (!built)
                    return ESceneContributionActivationError::BUILD_FAILED;

                const auto systems_after =
                    detail::SceneContributionBatchBuilderAccess::systems(
                        builder).size();
                const auto services_after =
                    detail::SceneContributionBatchBuilderAccess::serviceCount(
                        builder);
                if (services_after - services_before !=
                    descriptor->provided_services.size())
                {
                    return ESceneContributionActivationError::SERVICE_CONFLICT;
                }
                for (const auto provided : descriptor->provided_services)
                {
                    if (!detail::SceneContributionBatchBuilderAccess::services(
                            builder).contains(provided))
                    {
                        return ESceneContributionActivationError::
                            REQUIRED_SERVICE_MISSING;
                    }
                }

                pending.push_back(PendingActive{
                    descriptor,
                    std::move(config),
                    root
                        ? command.persistence
                        : EActivationPersistence::SCENE,
                    root,
                    systems_after - systems_before,
                    services_after - services_before});
            }

            command.publisher.setPhase(
                ESceneContributionActivationPhase::INSTALLING);
            std::vector<std::size_t> system_partitions;
            std::vector<std::size_t> service_partitions;
            system_partitions.reserve(pending.size());
            service_partitions.reserve(pending.size());
            std::size_t system_total = 0u;
            std::size_t service_total = 0u;
            for (const auto& value : pending)
            {
                system_partitions.push_back(value.system_count);
                service_partitions.push_back(value.service_count);
                system_total += value.system_count;
                service_total += value.service_count;
            }

            std::vector<lux::ecs::InstalledSceneServiceBatch>
                installed_services(pending.size());
            std::vector<lux::ecs::InstalledSystemBatch>
                installed_systems(pending.size());
            active.reserve(active.size() + pending.size());
            if (service_total != 0u)
            {
                auto installed = services.installPartitioned(
                    detail::SceneContributionBatchBuilderAccess::takeServices(
                        builder),
                    service_partitions);
                if (!installed)
                    return ESceneContributionActivationError::SERVICE_CONFLICT;
                installed_services = std::move(*installed);
            }

            if (system_total != 0u)
            {
                auto installed = schedule.installBatchPartitioned(
                    std::move(
                        detail::SceneContributionBatchBuilderAccess::systems(
                            builder)),
                    system_partitions);
                if (!installed)
                {
                    for (auto it = installed_services.rbegin();
                         it != installed_services.rend();
                         ++it)
                    {
                        it->reset();
                    }
                    return ESceneContributionActivationError::SCHEDULE_REJECTED;
                }
                installed_systems = std::move(*installed);
            }

            const auto first_active = active.size();
            const auto generation =
                command.publisher.ticket().snapshot().generation;
            for (std::size_t index = 0u; index < pending.size(); ++index)
            {
                Active value;
                value.id = pending[index].descriptor->id;
                value.config = std::move(pending[index].config);
                value.persistence = pending[index].persistence;
                value.root = pending[index].root;
                value.systems = std::move(installed_systems[index]);
                value.services = std::move(installed_services[index]);
                value.generation = generation;
                value.module = pending[index].descriptor->module;
                active.push_back(std::move(value));
            }
            for (std::size_t index = first_active;
                 index < active.size();
                 ++index)
            {
                publishFact(active[index], true);
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<ESceneContributionActivationError> disable(
            detail::SceneContributionCommand& command)
        {
            command.publisher.setPhase(ESceneContributionActivationPhase::REMOVING);
            auto target = findActive(command.id.view());
            if (target == active.size())
                return ESceneContributionActivationError::NOT_ACTIVE;

            for (;;)
            {
                std::size_t dependent = active.size();
                for (std::size_t index = active.size(); index > 0u; --index)
                {
                    if (dependsOn(active[index - 1u], command.id.view()))
                    {
                        dependent = index - 1u;
                        break;
                    }
                }
                if (dependent == active.size())
                    break;
                if (command.disable_mode != EContributionDisableMode::CASCADE)
                {
                    return ESceneContributionActivationError::
                        REQUIRED_BY_OTHER_CONTRIBUTION;
                }
                const auto failure = removeAt(dependent);
                if (failure != ESceneContributionActivationError::NONE)
                    return failure;
            }
            target = findActive(command.id.view());
            const auto failure = removeAt(target);
            if (failure != ESceneContributionActivationError::NONE)
                return failure;
            return std::nullopt;
        }

        lux::ecs::Schedule& schedule;
        lux::ecs::SceneServices& services;
        SceneContributionCatalog& catalog;
        lux::events::DomainEvents* events{nullptr};
        std::shared_ptr<detail::SceneContributionEndpoint> endpoint;
        std::vector<Active> active;
        std::uint64_t scene_identity{0u};
        std::size_t rejected_during_close{0u};
        bool close_requested{false};
        bool close_request_failed{false};
        bool closed{false};
    };

    SceneContributionHost::SceneContributionHost(
        lux::ecs::Schedule& schedule,
        lux::ecs::SceneServices& services,
        SceneContributionCatalog& catalog,
        lux::events::DomainEvents* events,
        SceneContributionQueueConfig queue)
        : impl_(std::make_unique<Impl>(
              schedule,
              services,
              catalog,
              events,
              queue))
    {}

    bool SceneContributionHost::adoptBootstrap(
        SceneContributionBootstrap bootstrap) noexcept
    {
        if (!impl_ || !impl_->active.empty() || !bootstrap.builder_ ||
            !bootstrap.builder_->committedTo(
                impl_->schedule,
                impl_->services))
        {
            return false;
        }
        for (const auto& entry : bootstrap.entries_)
        {
            if (!bootstrap.builder_->canClaimCommittedRange(
                    entry.first,
                    entry.last))
            {
                return false;
            }
        }

        impl_->active.reserve(bootstrap.entries_.size());
        const auto generation =
            impl_->endpoint->next_generation.fetch_add(
                1u,
                std::memory_order_relaxed);
        for (auto& entry : bootstrap.entries_)
        {
            auto installed = bootstrap.builder_->claimCommittedRange(
                entry.first,
                entry.last);
            if (!installed)
                std::abort();
            Impl::Active active;
            active.id = std::move(entry.id);
            active.config = std::move(entry.config);
            active.persistence = entry.persistence;
            active.root = entry.root;
            active.systems = std::move(installed->systems);
            active.services = std::move(installed->services);
            active.generation = generation;
            active.module = std::move(entry.module);
            impl_->active.push_back(std::move(active));
        }
        for (const auto& active : impl_->active)
            impl_->publishFact(active, true);
        bootstrap.builder_ = nullptr;
        bootstrap.entries_.clear();
        return true;
    }

    SceneContributionHost::~SceneContributionHost() noexcept
    {
        if (impl_ && !impl_->closed)
            impl_->endpoint->admission_open.store(false, std::memory_order_release);
    }

    SceneContributions SceneContributionHost::facade() const noexcept
    {
        return impl_ ? SceneContributions{impl_->endpoint} : SceneContributions{};
    }

    std::size_t SceneContributionHost::processSafePoint(
        std::size_t budget) noexcept
    {
        if (!impl_ || impl_->closed)
            return 0u;
        std::size_t processed = 0u;
        detail::SceneContributionCommand command{
            detail::ESceneContributionCommandKind::DISABLE,
            {},
            {},
            EActivationPersistence::TRANSIENT,
            EContributionDisableMode::REJECT_DEPENDENTS,
            0u};
        while (processed < budget && impl_->endpoint->queue.try_dequeue(command))
        {
            impl_->endpoint->bytes_inflight.fetch_sub(
                command.accounted_bytes,
                std::memory_order_release);
            const auto failure = command.kind == detail::ESceneContributionCommandKind::ENABLE
                ? impl_->enable(command)
                : impl_->disable(command);
            if (failure)
            {
                command.publisher.fail(*failure);
            }
            else
            {
                const bool now_active = impl_->findActive(command.id.view()) !=
                    impl_->active.size();
                command.publisher.setPhase(
                    now_active
                        ? ESceneContributionActivationPhase::ACTIVE
                        : ESceneContributionActivationPhase::INACTIVE);
                command.publisher.succeed(SceneContributionActivationResult{
                    command.id,
                    command.publisher.ticket().snapshot().generation,
                    now_active});
            }
            ++processed;
        }
        return processed;
    }

    bool SceneContributionHost::active(
        lux::extensions::ContributionIdView id) const noexcept
    {
        return impl_ && !impl_->closed &&
               impl_->findActive(id) != impl_->active.size();
    }

    const lux::ecs::SceneServices& SceneContributionHost::services() const noexcept
    {
        return impl_->services;
    }

    std::vector<SceneContributionActivationSnapshot>
    SceneContributionHost::activationSnapshot() const
    {
        std::vector<SceneContributionActivationSnapshot> result;
        if (!impl_ || impl_->closed)
            return result;
        result.reserve(impl_->active.size());
        for (const auto& active : impl_->active)
        {
            const auto* descriptor = impl_->catalog.find(active.id.view());
            if (!descriptor)
                continue;
            result.push_back(SceneContributionActivationSnapshot{
                active.id,
                active.config,
                active.persistence,
                descriptor->provider,
                active.generation,
                active.root});
        }
        return result;
    }

    void SceneContributionHost::requestClose() noexcept
    {
        if (!impl_ || impl_->closed || impl_->close_requested)
            return;
        impl_->close_requested = true;
        impl_->endpoint->admission_open.store(false,
                                               std::memory_order_release);

        detail::SceneContributionCommand queued{
            detail::ESceneContributionCommandKind::DISABLE,
            {},
            {},
            EActivationPersistence::TRANSIENT,
            EContributionDisableMode::REJECT_DEPENDENTS,
            0u};
        while (impl_->endpoint->queue.try_dequeue(queued))
        {
            queued.publisher.fail(ESceneContributionActivationError::STOPPING);
            ++impl_->rejected_during_close;
        }
        impl_->endpoint->bytes_inflight.store(0u,
                                               std::memory_order_release);
        for (auto active = impl_->active.rbegin();
             active != impl_->active.rend(); ++active)
        {
            if (active->systems.valid() &&
                !impl_->schedule.requestBatchClose(active->systems))
            {
                impl_->close_request_failed = true;
            }
        }
    }

    SceneContributionCloseReport SceneContributionHost::close() noexcept
    {
        SceneContributionCloseReport report;
        if (!impl_ || impl_->closed)
            return report;
        requestClose();
        report.rejected_queued = impl_->rejected_during_close;
        impl_->rejected_during_close = 0u;
        if (impl_->close_request_failed)
        {
            report.failed = 1u;
            return report;
        }

        for (const auto& active : impl_->active)
        {
            if (!active.systems.valid())
                continue;
            const auto state = impl_->schedule.batchCloseState(active.systems);
            if (!state.valid)
            {
                ++report.failed;
                return report;
            }
            report.pending_systems += state.pending_systems;
            report.owner_work_pending =
                report.owner_work_pending || state.owner_work_pending;
        }
        if (report.pending_systems != 0u)
            return report;

        while (!impl_->active.empty())
        {
            const auto failure = impl_->removeAt(impl_->active.size() - 1u);
            if (failure == ESceneContributionActivationError::NONE)
                ++report.removed;
            else
            {
                ++report.failed;
                break;
            }
        }
        impl_->closed = report.complete();
        return report;
    }

    SceneContributionCloseReport
    SceneContributionHost::sealForScheduleTeardown() noexcept
    {
        SceneContributionCloseReport report;
        if (!impl_ || impl_->closed)
            return report;
        requestClose();
        report.rejected_queued = impl_->rejected_during_close;
        impl_->rejected_during_close = 0u;
        if (impl_->close_request_failed)
        {
            report.failed = 1u;
            return report;
        }

        for (const auto& active : impl_->active)
        {
            if (!active.systems.valid())
                continue;
            const auto state = impl_->schedule.batchCloseState(active.systems);
            if (!state.valid)
            {
                ++report.failed;
                return report;
            }
            report.pending_systems += state.pending_systems;
            report.owner_work_pending =
                report.owner_work_pending || state.owner_work_pending;
        }
        if (report.pending_systems != 0u)
            return report;

        // Dynamic disable uses removeAt(), which requires every system to
        // opt into onRemoved(). Whole-scene teardown is different: many
        // systems intentionally borrow scene services until their ordinary
        // Schedule destructor runs. Mark the public activation state closed,
        // but retain batches, services and module leases until the caller has
        // destroyed the Schedule.
        for (auto active = impl_->active.rbegin();
             active != impl_->active.rend();
             ++active)
        {
            impl_->publishFact(*active, false);
        }
        impl_->closed = true;
        return report;
    }
}
