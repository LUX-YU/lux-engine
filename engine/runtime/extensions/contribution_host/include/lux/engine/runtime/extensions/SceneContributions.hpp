#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/ecs/SceneServices.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/runtime/extensions/ModuleLifetime.hpp>
#include <lux/engine/runtime/extensions/OperationTicket.hpp>
#include <lux/engine/core/extension_abi/StableId.hpp>
#include <lux/engine/runtime/extensions/ContributionConfig.hpp>
#include <lux/engine/runtime/extensions/contribution_visibility.h>

#include <cstddef>
#include <concepts>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace lux::events
{
    class DomainEvents;
}

namespace lux::runtime
{
    namespace detail
    {
        struct SceneContributionBatchBuilderAccess;
    }

    enum class ESceneContributionCatalogError : std::uint8_t
    {
        INVALID_DESCRIPTOR,
        DUPLICATE_CONTRIBUTION,
        ID_COLLISION,
        MISSING_BUILD_CALLBACK,
        MISSING_DEPENDENCY
    };

    enum class ESceneContributionBuildError : std::uint8_t
    {
        INVALID_CONFIG,
        NULL_SYSTEM,
        DUPLICATE_SYSTEM,
        DUPLICATE_SERVICE,
        MISSING_SERVICE,
        BUILD_REJECTED
    };

    struct SceneContributionBuildFailure final
    {
        ESceneContributionBuildError code{ESceneContributionBuildError::BUILD_REJECTED};
        lux::ecs::TypeToken type{};
    };

    struct SceneContributionBuildContext final
    {
        std::uint64_t scene_identity{0u};
        lux::ecs::SceneServices& services;
    };

    class LUX_RUNTIME_CONTRIBUTION_PUBLIC SceneContributionBatchBuilder final
    {
    public:
        SceneContributionBatchBuilder();
        ~SceneContributionBatchBuilder();
        SceneContributionBatchBuilder(const SceneContributionBatchBuilder&) = delete;
        SceneContributionBatchBuilder& operator=(const SceneContributionBatchBuilder&) =
            delete;
        SceneContributionBatchBuilder(SceneContributionBatchBuilder&&) noexcept;
        SceneContributionBatchBuilder& operator=(SceneContributionBatchBuilder&&) noexcept;

        template <class System>
            requires std::derived_from<System, lux::ecs::ISystem> &&
                     (!std::same_as<System, lux::ecs::ISystem>)
        [[nodiscard]] lux::cxx::expected<void, SceneContributionBuildFailure> add(
            std::unique_ptr<System> system,
            int phase = lux::ecs::kPhaseSimulation)
        {
            if (assembly_)
            {
                const auto result = assembly_->add(std::move(system), phase);
                if (result)
                    return {};
                return lux::cxx::unexpected(SceneContributionBuildFailure{
                    result.error() == lux::ecs::EScheduleBuildError::NullSystem
                        ? ESceneContributionBuildError::NULL_SYSTEM
                        : ESceneContributionBuildError::DUPLICATE_SYSTEM,
                    lux::ecs::systemType<System>()});
            }
            const auto result = systems_.add(std::move(system), phase);
            if (result)
                return {};
            return lux::cxx::unexpected(SceneContributionBuildFailure{
                result.error() == lux::ecs::EScheduleBatchError::NULL_SYSTEM
                    ? ESceneContributionBuildError::NULL_SYSTEM
                    : ESceneContributionBuildError::DUPLICATE_SYSTEM,
                lux::ecs::systemType<System>()});
        }

        /// Add a system and expose the exact staged instance as a borrowed
        /// scene service. The enclosing dependency closure preflights every
        /// system before either table is externally observable; removal
        /// destroys systems before retiring the corresponding service
        /// generation.
        template <class System>
            requires std::derived_from<System, lux::ecs::ISystem> &&
                     (!std::same_as<System, lux::ecs::ISystem>)
        [[nodiscard]] lux::cxx::expected<void, SceneContributionBuildFailure>
        addServiceSystem(
            std::unique_ptr<System> system,
            int phase = lux::ecs::kPhaseSimulation)
        {
            constexpr auto type = lux::ecs::typeToken<System>();
            if (!system)
            {
                return lux::cxx::unexpected(SceneContributionBuildFailure{
                    ESceneContributionBuildError::NULL_SYSTEM,
                    type});
            }
            System* const service = system.get();
            if (assembly_)
            {
                const auto added = assembly_->add(std::move(system), phase);
                if (!added)
                {
                    return lux::cxx::unexpected(SceneContributionBuildFailure{
                        added.error() == lux::ecs::EScheduleBuildError::NullSystem
                            ? ESceneContributionBuildError::NULL_SYSTEM
                            : ESceneContributionBuildError::DUPLICATE_SYSTEM,
                        type});
                }
                const auto published = assembly_->services().adopt(*service);
                if (!published)
                {
                    return lux::cxx::unexpected(SceneContributionBuildFailure{
                        published.error() == lux::ecs::
                                ESceneServiceRegistrationError::DuplicateType
                            ? ESceneContributionBuildError::DUPLICATE_SERVICE
                            : ESceneContributionBuildError::MISSING_SERVICE,
                        type});
                }
                ++published_service_count_;
                return {};
            }
            const auto added = systems_.add(std::move(system), phase);
            if (!added)
            {
                return lux::cxx::unexpected(SceneContributionBuildFailure{
                    added.error() == lux::ecs::EScheduleBatchError::NULL_SYSTEM
                        ? ESceneContributionBuildError::NULL_SYSTEM
                        : ESceneContributionBuildError::DUPLICATE_SYSTEM,
                    type});
            }
            const auto published = services_.addBorrowed(*service);
            if (!published)
            {
                return lux::cxx::unexpected(SceneContributionBuildFailure{
                    published.error() == lux::ecs::
                            ESceneServiceRegistrationError::DuplicateType
                        ? ESceneContributionBuildError::DUPLICATE_SERVICE
                        : ESceneContributionBuildError::MISSING_SERVICE,
                    type});
            }
            ++published_service_count_;
            return {};
        }

        template <class T>
        [[nodiscard]] lux::cxx::expected<T*, SceneContributionBuildFailure>
        publishServiceAndGet(std::unique_ptr<T> value)
        {
            constexpr auto type = lux::ecs::typeToken<T>();
            if (!value)
            {
                return lux::cxx::unexpected(SceneContributionBuildFailure{
                    ESceneContributionBuildError::MISSING_SERVICE,
                    type});
            }
            const auto added = assembly_
                ? assembly_->services().emplace(std::move(value))
                : services_.add(std::move(value));
            if (!added)
                return lux::cxx::unexpected(SceneContributionBuildFailure{
                    added.error() == lux::ecs::
                            ESceneServiceRegistrationError::DuplicateType
                        ? ESceneContributionBuildError::DUPLICATE_SERVICE
                        : ESceneContributionBuildError::MISSING_SERVICE,
                    type});
            ++published_service_count_;
            return *added;
        }

        template <class T>
        [[nodiscard]] lux::cxx::expected<void, SceneContributionBuildFailure>
        publishService(std::unique_ptr<T> value)
        {
            auto added = publishServiceAndGet(std::move(value));
            if (!added)
                return lux::cxx::unexpected(added.error());
            return {};
        }

        /// Resolve a service from the unpublished dependency closure first,
        /// then from the live scene. This is the only build-time lookup which
        /// lets one atomic contribution closure consume a service provided by
        /// an earlier descriptor in that same closure.
        template <class T>
        [[nodiscard]] T* findService(
            const SceneContributionBuildContext& context) noexcept
        {
            if (assembly_)
                return assembly_->services().borrow<T>();
            if (auto* staged = services_.get<T>())
                return staged;
            return context.services.get<T>();
        }

        [[nodiscard]] bool containsService(
            lux::ecs::SceneServiceType type,
            const SceneContributionBuildContext& context) const noexcept
        {
            return assembly_
                ? assembly_->services().contains(type)
                : services_.contains(type) || context.services.contains(type);
        }

    private:
        friend class SceneContributionCatalog;
        friend struct detail::SceneContributionBatchBuilderAccess;
        explicit SceneContributionBatchBuilder(
            lux::ecs::ScheduleBuilder& assembly) noexcept
            : assembly_(&assembly)
        {}

        lux::ecs::ScheduleBuilder* assembly_{nullptr};
        lux::ecs::ScheduleMutationBatch systems_;
        lux::ecs::SceneServiceMutationBatch services_;
        std::size_t published_service_count_{0u};
    };

    enum class ESceneContributionAssemblyError : std::uint8_t
    {
        UNKNOWN_CONTRIBUTION,
        MISSING_DEPENDENCY,
        DEPENDENCY_CYCLE,
        CONFIG_VERSION_MISMATCH,
        REQUIRED_SERVICE_MISSING,
        SERVICE_CONFLICT,
        BUILD_FAILED
    };

    struct SceneContributionAssemblyFailure final
    {
        ESceneContributionAssemblyError code{
            ESceneContributionAssemblyError::BUILD_FAILED};
        lux::extensions::ContributionId contribution;
        SceneContributionBuildFailure build{};
    };

    /// One explicitly selected scene contribution and its cooked
    /// configuration. Dependencies which are not selected explicitly still
    /// use their descriptor defaults.
    struct SceneContributionSelection final
    {
        lux::extensions::ContributionId id;
        ContributionConfig config;
    };

    struct SceneContributionDescriptor final
    {
        SceneContributionDescriptor() = default;
        SceneContributionDescriptor(
            const SceneContributionDescriptor&) = delete;
        SceneContributionDescriptor& operator=(
            const SceneContributionDescriptor&) = delete;
        SceneContributionDescriptor(
            SceneContributionDescriptor&&) noexcept = default;
        SceneContributionDescriptor& operator=(
            SceneContributionDescriptor&&) noexcept = default;

        lux::extensions::ContributionId id;
        std::string display_name;
        std::vector<lux::extensions::ContributionId> required_contributions;
        std::vector<lux::ecs::TypeToken> required_services;
        std::vector<lux::ecs::TypeToken> provided_services;
        std::uint32_t config_schema_version{0u};
        ContributionConfig default_config;
        lux::cxx::move_only_function<
            lux::cxx::expected<void, SceneContributionBuildFailure>(
                SceneContributionBatchBuilder&,
                const SceneContributionBuildContext&,
                ContributionConfig)>
            build;
        lux::extensions::ExtensionId provider;
        lux::extensions::ModuleLease module;
    };

    /// Ownership receipt for contributions staged into one unpublished
    /// ScheduleBuilder transaction. Only SceneContributionHost may claim it
    /// after commit; dropping it keeps the staged systems fixed-lifetime.
    class LUX_RUNTIME_CONTRIBUTION_PUBLIC SceneContributionBootstrap final
    {
    public:
        SceneContributionBootstrap() = default;
        SceneContributionBootstrap(const SceneContributionBootstrap&) = delete;
        SceneContributionBootstrap& operator=(
            const SceneContributionBootstrap&) = delete;
        SceneContributionBootstrap(SceneContributionBootstrap&&) noexcept =
            default;
        SceneContributionBootstrap& operator=(
            SceneContributionBootstrap&&) noexcept = default;

    private:
        friend class SceneContributionCatalog;
        friend class SceneContributionHost;

        struct Entry final
        {
            lux::extensions::ContributionId id;
            ContributionConfig config;
            EActivationPersistence persistence{
                EActivationPersistence::SCENE};
            bool root{false};
            lux::ecs::ScheduleBuilder::Checkpoint first;
            lux::ecs::ScheduleBuilder::Checkpoint last;
            lux::extensions::ModuleLease module;
        };

        lux::ecs::ScheduleBuilder* builder_{nullptr};
        std::vector<Entry> entries_;
    };

    class LUX_RUNTIME_CONTRIBUTION_PUBLIC SceneContributionCatalog final
    {
    public:
        SceneContributionCatalog() = default;
        SceneContributionCatalog(const SceneContributionCatalog&) = delete;
        SceneContributionCatalog& operator=(const SceneContributionCatalog&) = delete;
        SceneContributionCatalog(SceneContributionCatalog&&) noexcept = default;
        SceneContributionCatalog& operator=(SceneContributionCatalog&&) noexcept = default;

        [[nodiscard]] lux::cxx::expected<void, ESceneContributionCatalogError> add(
            SceneContributionDescriptor descriptor);
        [[nodiscard]] lux::cxx::expected<void, ESceneContributionCatalogError>
        validateBatch(
            std::span<const SceneContributionDescriptor> descriptors)
            const noexcept;
        [[nodiscard]] lux::cxx::expected<void, ESceneContributionCatalogError> addBatch(
            std::vector<SceneContributionDescriptor> descriptors);
        [[nodiscard]] SceneContributionDescriptor* find(
            lux::extensions::ContributionIdView id) noexcept;
        [[nodiscard]] const SceneContributionDescriptor* find(
            lux::extensions::ContributionIdView id) const noexcept;
        [[nodiscard]] std::span<const SceneContributionDescriptor> all()
            const noexcept;

        /// Assemble the selected dependency closure into the caller's single
        /// unpublished ScheduleBuilder transaction. This is the cold-start
        /// path used by built-in scene packs: render integrations may publish
        /// staging services before this call and finalize them afterwards.
        /// No Schedule or SceneServices state becomes live until the caller
        /// commits its builder.
        [[nodiscard]] lux::cxx::expected<
            void,
            SceneContributionAssemblyFailure>
        assembleDefaults(
            lux::ecs::ScheduleBuilder& assembly,
            std::span<const lux::extensions::ContributionIdView> selected);

        /// Assemble a manifest-authored contribution set into the caller's
        /// unpublished transaction. An explicitly selected contribution uses
        /// exactly its supplied config; dependency-only nodes use defaults.
        [[nodiscard]] lux::cxx::expected<
            void,
            SceneContributionAssemblyFailure>
        assemble(
            lux::ecs::ScheduleBuilder& assembly,
            std::span<const SceneContributionSelection> selected);

        /// Stage a manifest-authored closure and retain the exact ownership
        /// partitions needed for SceneContributionHost to adopt it after the
        /// outer ScheduleBuilder commit.
        [[nodiscard]] lux::cxx::expected<
            SceneContributionBootstrap,
            SceneContributionAssemblyFailure>
        stageBootstrap(
            lux::ecs::ScheduleBuilder& assembly,
            std::span<const SceneContributionSelection> selected);

    private:
        std::vector<SceneContributionDescriptor> descriptors_;
    };

    enum class ESceneContributionActivationPhase : std::uint8_t
    {
        QUEUED,
        RESOLVING_DEPENDENCIES,
        BUILDING,
        INSTALLING,
        ACTIVE,
        REMOVING,
        INACTIVE
    };

    enum class ESceneContributionActivationError : std::uint8_t
    {
        NONE,
        QUEUE_FULL,
        BYTE_BUDGET_EXHAUSTED,
        STOPPING,
        UNKNOWN_CONTRIBUTION,
        MISSING_DEPENDENCY,
        DEPENDENCY_CYCLE,
        CONFIG_VERSION_MISMATCH,
        REQUIRED_SERVICE_MISSING,
        SERVICE_CONFLICT,
        BUILD_FAILED,
        SCHEDULE_REJECTED,
        REQUIRED_BY_OTHER_CONTRIBUTION,
        NOT_ACTIVE,
        ROLLBACK_FAILED
    };

    struct SceneContributionActivationResult final
    {
        lux::extensions::ContributionId contribution;
        std::uint64_t generation{0u};
        bool active{false};
    };

    using SceneContributionOperationTicket = lux::extensions::OperationTicket<
        ESceneContributionActivationPhase,
        ESceneContributionActivationError,
        SceneContributionActivationResult>;

    struct SceneContributionStateChanged final
    {
        lux::extensions::ContributionId contribution;
        std::uint64_t generation{0u};
        bool active{false};
    };

    struct SceneContributionActivationSnapshot final
    {
        lux::extensions::ContributionId contribution;
        ContributionConfig config;
        EActivationPersistence persistence{EActivationPersistence::SCENE};
        lux::extensions::ExtensionId provider;
        std::uint64_t generation{0u};
        bool root{false};
    };

    struct SceneContributionQueueConfig final
    {
        std::size_t capacity{128u};
        std::size_t byte_budget{1024u * 1024u};
    };

    enum class EContributionDisableMode : std::uint8_t
    {
        REJECT_DEPENDENTS,
        CASCADE
    };

    namespace detail
    {
        struct SceneContributionEndpoint;
    }

    class LUX_RUNTIME_CONTRIBUTION_PUBLIC SceneContributions final
    {
    public:
        SceneContributions() noexcept = default;

        [[nodiscard]] SceneContributionOperationTicket requestEnable(
            lux::extensions::ContributionIdView id,
            ContributionConfig config = {},
            EActivationPersistence persistence =
                EActivationPersistence::SCENE) const;
        [[nodiscard]] SceneContributionOperationTicket requestDisable(
            lux::extensions::ContributionIdView id,
            EContributionDisableMode mode =
                EContributionDisableMode::REJECT_DEPENDENTS) const;
        [[nodiscard]] explicit operator bool() const noexcept;

    private:
        friend class SceneContributionHost;
        explicit SceneContributions(
            std::shared_ptr<detail::SceneContributionEndpoint> endpoint) noexcept;
        std::shared_ptr<detail::SceneContributionEndpoint> endpoint_;
    };

    struct SceneContributionCloseReport final
    {
        std::size_t removed{0u};
        std::size_t failed{0u};
        std::size_t rejected_queued{0u};
        std::size_t pending_systems{0u};
        bool owner_work_pending{false};

        [[nodiscard]] bool complete() const noexcept
        {
            return failed == 0u && pending_systems == 0u;
        }
    };

    class LUX_RUNTIME_CONTRIBUTION_PUBLIC SceneContributionHost final
    {
    public:
        SceneContributionHost(
            lux::ecs::Schedule& schedule,
            lux::ecs::SceneServices& services,
            SceneContributionCatalog& catalog,
            lux::events::DomainEvents* events = nullptr,
            SceneContributionQueueConfig queue = {});
        ~SceneContributionHost() noexcept;
        SceneContributionHost(const SceneContributionHost&) = delete;
        SceneContributionHost& operator=(const SceneContributionHost&) = delete;

        [[nodiscard]] SceneContributions facade() const noexcept;
        [[nodiscard]] std::size_t processSafePoint(
            std::size_t budget = 32u) noexcept;
        [[nodiscard]] bool active(
            lux::extensions::ContributionIdView id) const noexcept;
        [[nodiscard]] const lux::ecs::SceneServices& services() const noexcept;
        [[nodiscard]] std::vector<SceneContributionActivationSnapshot>
        activationSnapshot() const;
        /// Adopt a successfully committed cold-start transaction. Failure is
        /// an invariant mismatch (wrong builder, overlap, or pre-commit use)
        /// and leaves the host unchanged.
        [[nodiscard]] bool adoptBootstrap(
            SceneContributionBootstrap bootstrap) noexcept;
        /// Closes admission and asks every installed system to quiesce. No
        /// system or service is removed until a later close() observes all
        /// batches complete.
        void requestClose() noexcept;
        /// Completes a whole-Schedule shutdown without applying the dynamic
        /// removal contract to scene-bound systems. The caller must destroy
        /// the Schedule before destroying this host so borrowed contribution
        /// services outlive every system destructor.
        [[nodiscard]] SceneContributionCloseReport
        sealForScheduleTeardown() noexcept;
        /// Dynamically removes every contribution batch. Every contained
        /// system must explicitly support dynamic removal.
        [[nodiscard]] SceneContributionCloseReport close() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
