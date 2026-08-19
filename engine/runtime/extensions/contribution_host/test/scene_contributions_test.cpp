#include <lux/engine/runtime/extensions/SceneContributions.hpp>

#include <lux/engine/ecs/SceneServices.hpp>
#include <lux/engine/ecs/World.hpp>

#include <cstdio>
#include <array>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace
{
    int failures = 0;

    void expect(bool value, const char* message)
    {
        std::fprintf(stderr, "[%s] %s\n", value ? " ok " : "FAIL", message);
        if (!value)
            ++failures;
    }

    struct RootSystem final : lux::ecs::ISystem
    {
        explicit RootSystem(std::vector<int>& output) noexcept : output(output) {}
        void update(const lux::ecs::SystemUpdateContext&) override
        {
            output.push_back(1);
        }
        void onRemoved(const lux::ecs::SystemRemovalContext&) override
        {
            output.push_back(-1);
        }
        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }
        std::vector<int>& output;
    };

    struct LeafSystem final : lux::ecs::ISystem
    {
        explicit LeafSystem(std::vector<int>& output) noexcept : output(output) {}
        [[nodiscard]] std::span<const lux::ecs::SystemType>
        prerequisites() const noexcept override
        {
            static constexpr lux::ecs::SystemType required[]{
                lux::ecs::systemType<RootSystem>()};
            return required;
        }
        void update(const lux::ecs::SystemUpdateContext&) override
        {
            output.push_back(2);
        }
        void onRemoved(const lux::ecs::SystemRemovalContext&) override
        {
            output.push_back(-2);
        }
        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }
        std::vector<int>& output;
    };

    struct MissingSystem final : lux::ecs::ISystem
    {
        void update(const lux::ecs::SystemUpdateContext&) override {}
    };

    struct BrokenSystem final : lux::ecs::ISystem
    {
        [[nodiscard]] std::span<const lux::ecs::SystemType>
        prerequisites() const noexcept override
        {
            static constexpr lux::ecs::SystemType required[]{
                lux::ecs::systemType<MissingSystem>()};
            return required;
        }
        void update(const lux::ecs::SystemUpdateContext&) override {}
        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }
    };

    struct UndeclaredService final
    {
        std::uint32_t value{17u};
    };

    struct BoundedCloseRootSystem final : lux::ecs::ISystem
    {
        explicit BoundedCloseRootSystem(std::vector<int>& output) noexcept
            : output(output)
        {}
        void update(const lux::ecs::SystemUpdateContext&) override
        {
            if (closing && remaining_ticks != 0u)
                --remaining_ticks;
        }
        void requestClose() noexcept override
        {
            if (closing)
                return;
            closing = true;
            output.push_back(10);
        }
        [[nodiscard]] bool closeComplete() const noexcept override
        {
            return closing && remaining_ticks == 0u;
        }
        [[nodiscard]] bool closeNeedsOwnerTick() const noexcept override
        {
            return closing && remaining_ticks != 0u;
        }
        void onRemoved(const lux::ecs::SystemRemovalContext&) override
        {
            output.push_back(-10);
        }
        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }
        std::vector<int>& output;
        std::uint32_t remaining_ticks{2u};
        bool closing{false};
    };

    struct BoundedCloseLeafSystem final : lux::ecs::ISystem
    {
        explicit BoundedCloseLeafSystem(std::vector<int>& output) noexcept
            : output(output)
        {}
        [[nodiscard]] std::span<const lux::ecs::SystemType>
        prerequisites() const noexcept override
        {
            static constexpr lux::ecs::SystemType required[]{
                lux::ecs::systemType<BoundedCloseRootSystem>()};
            return required;
        }
        void update(const lux::ecs::SystemUpdateContext&) override
        {
            if (closing && remaining_ticks != 0u)
                --remaining_ticks;
        }
        void requestClose() noexcept override
        {
            if (closing)
                return;
            closing = true;
            output.push_back(20);
        }
        [[nodiscard]] bool closeComplete() const noexcept override
        {
            return closing && remaining_ticks == 0u;
        }
        [[nodiscard]] bool closeNeedsOwnerTick() const noexcept override
        {
            return closing && remaining_ticks != 0u;
        }
        void onRemoved(const lux::ecs::SystemRemovalContext&) override
        {
            output.push_back(-20);
        }
        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }
        std::vector<int>& output;
        std::uint32_t remaining_ticks{1u};
        bool closing{false};
    };

    struct ExternalCloseSystem final : lux::ecs::ISystem
    {
        void update(const lux::ecs::SystemUpdateContext&) override {}
        void requestClose() noexcept override { closing = true; }
        void requestClose(lux::ecs::SystemCloseProgressSink value)
            noexcept override
        {
            requestClose();
            progress = value;
        }
        [[nodiscard]] bool closeComplete() const noexcept override
        {
            return closing && completed;
        }
        [[nodiscard]] bool closeNeedsOwnerTick() const noexcept override
        {
            return false;
        }
        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }
        void completeExternal() noexcept
        {
            completed = true;
            progress.notify();
        }

        lux::ecs::SystemCloseProgressSink progress;
        bool closing{false};
        bool completed{false};
    };

    lux::runtime::SceneContributionDescriptor makeRoot(
        std::vector<int>& trace)
    {
        lux::runtime::SceneContributionDescriptor descriptor;
        descriptor.id = lux::scene::SceneFeatureId{"org.test.scene.root"};
        descriptor.display_name = "Root";
        descriptor.provided_services = {
            lux::ecs::typeToken<RootSystem>()};
        descriptor.config_schema_version = 0u;
        descriptor.provider = lux::extensions::ExtensionId{"org.test.root"};
        descriptor.build = [&trace](
            lux::runtime::SceneContributionBatchBuilder& builder,
            const lux::runtime::SceneContributionBuildContext&,
            lux::runtime::ContributionConfig)
            -> lux::cxx::expected<
                void,
                lux::runtime::SceneContributionBuildFailure>
        {
            return builder.addServiceSystem(
                std::make_unique<RootSystem>(trace));
        };
        return descriptor;
    }

    lux::runtime::SceneContributionDescriptor makeLeaf(
        std::vector<int>& trace)
    {
        lux::runtime::SceneContributionDescriptor descriptor;
        descriptor.id = lux::scene::SceneFeatureId{"org.test.scene.leaf"};
        descriptor.display_name = "Leaf";
        descriptor.required_contributions.push_back(
            lux::scene::SceneFeatureId{"org.test.scene.root"});
        descriptor.required_services = {
            lux::ecs::typeToken<RootSystem>()};
        descriptor.config_schema_version = 0u;
        descriptor.provider = lux::extensions::ExtensionId{"org.test.leaf"};
        descriptor.build = [&trace](
            lux::runtime::SceneContributionBatchBuilder& builder,
            const lux::runtime::SceneContributionBuildContext& context,
            lux::runtime::ContributionConfig)
            -> lux::cxx::expected<
                void,
                lux::runtime::SceneContributionBuildFailure>
        {
            const auto* service = builder.findService<RootSystem>(context);
            if (!service)
            {
                return lux::cxx::unexpected(
                    lux::runtime::SceneContributionBuildFailure{
                        lux::runtime::ESceneContributionBuildError::
                            MISSING_SERVICE,
                        lux::ecs::typeToken<RootSystem>()});
            }
            return builder.add(std::make_unique<LeafSystem>(trace));
        };
        return descriptor;
    }

    lux::runtime::SceneContributionDescriptor makeBrokenLeaf()
    {
        lux::runtime::SceneContributionDescriptor descriptor;
        descriptor.id = lux::scene::SceneFeatureId{
            "org.test.scene.broken_leaf"};
        descriptor.display_name = "Broken leaf";
        descriptor.required_contributions.push_back(
            lux::scene::SceneFeatureId{"org.test.scene.root"});
        descriptor.required_services = {
            lux::ecs::typeToken<RootSystem>()};
        descriptor.config_schema_version = 0u;
        descriptor.provider = lux::extensions::ExtensionId{
            "org.test.broken_leaf"};
        descriptor.build = [](
            lux::runtime::SceneContributionBatchBuilder& builder,
            const lux::runtime::SceneContributionBuildContext& context,
            lux::runtime::ContributionConfig)
            -> lux::cxx::expected<
                void,
                lux::runtime::SceneContributionBuildFailure>
        {
            if (!builder.findService<RootSystem>(context))
            {
                return lux::cxx::unexpected(
                    lux::runtime::SceneContributionBuildFailure{
                        lux::runtime::ESceneContributionBuildError::
                            MISSING_SERVICE,
                        lux::ecs::typeToken<RootSystem>()});
            }
            return builder.add(std::make_unique<BrokenSystem>());
        };
        return descriptor;
    }

    lux::runtime::SceneContributionDescriptor makeFailingAfterStage()
    {
        lux::runtime::SceneContributionDescriptor descriptor;
        descriptor.id = lux::scene::SceneFeatureId{
            "org.test.scene.failing_after_stage"};
        descriptor.display_name = "Fails after staging";
        descriptor.required_contributions.push_back(
            lux::scene::SceneFeatureId{"org.test.scene.root"});
        descriptor.required_services = {
            lux::ecs::typeToken<RootSystem>()};
        descriptor.config_schema_version = 0u;
        descriptor.provider = lux::extensions::ExtensionId{
            "org.test.failing_after_stage"};
        descriptor.build = [](
            lux::runtime::SceneContributionBatchBuilder& builder,
            const lux::runtime::SceneContributionBuildContext&,
            lux::runtime::ContributionConfig)
            -> lux::cxx::expected<
                void,
                lux::runtime::SceneContributionBuildFailure>
        {
            const auto staged = builder.add(
                std::make_unique<BrokenSystem>());
            if (!staged)
                return lux::cxx::unexpected(staged.error());
            return lux::cxx::unexpected(
                lux::runtime::SceneContributionBuildFailure{
                    lux::runtime::ESceneContributionBuildError::
                        BUILD_REJECTED,
                    lux::ecs::typeToken<BrokenSystem>()});
        };
        return descriptor;
    }

    lux::runtime::SceneContributionDescriptor makeUndeclaredService()
    {
        lux::runtime::SceneContributionDescriptor descriptor;
        descriptor.id = lux::scene::SceneFeatureId{
            "org.test.scene.undeclared_service"};
        descriptor.display_name = "Publishes an undeclared service";
        descriptor.config_schema_version = 0u;
        descriptor.provider = lux::extensions::ExtensionId{
            "org.test.undeclared_service"};
        descriptor.build = [](
            lux::runtime::SceneContributionBatchBuilder& builder,
            const lux::runtime::SceneContributionBuildContext&,
            lux::runtime::ContributionConfig)
            -> lux::cxx::expected<
                void,
                lux::runtime::SceneContributionBuildFailure>
        {
            // Intentionally absent from descriptor.provided_services.
            return builder.publishService(
                std::make_unique<UndeclaredService>());
        };
        return descriptor;
    }

    lux::runtime::SceneContributionDescriptor makeBoundedClose(
        std::vector<int>& trace)
    {
        lux::runtime::SceneContributionDescriptor descriptor;
        descriptor.id = lux::scene::SceneFeatureId{
            "org.test.scene.bounded_close"};
        descriptor.display_name = "Bounded close";
        descriptor.config_schema_version = 0u;
        descriptor.provider = lux::extensions::ExtensionId{
            "org.test.bounded_close"};
        descriptor.build = [&trace](
            lux::runtime::SceneContributionBatchBuilder& builder,
            const lux::runtime::SceneContributionBuildContext&,
            lux::runtime::ContributionConfig)
            -> lux::cxx::expected<
                void,
                lux::runtime::SceneContributionBuildFailure>
        {
            const auto root = builder.add(
                std::make_unique<BoundedCloseRootSystem>(trace));
            if (!root)
                return lux::cxx::unexpected(root.error());
            return builder.add(
                std::make_unique<BoundedCloseLeafSystem>(trace));
        };
        return descriptor;
    }
}

int main()
{
    static_assert(!std::is_same_v<
        lux::scene::SceneFeatureId,
        lux::extensions::ContributionId>);

    std::vector<int> trace;
    lux::runtime::SceneContributionCatalog catalog;
    expect(catalog.add(makeRoot(trace)).has_value(),
           "catalog accepts the root contribution");
    expect(catalog.add(makeLeaf(trace)).has_value(),
           "catalog accepts the dependent contribution");
    expect(catalog.find(
               lux::scene::sceneFeatureId(
                   "org.test.scene.root")) != nullptr,
           "catalog lookup uses the Scene-owned feature identity");
    const auto duplicate = catalog.add(makeLeaf(trace));
    expect(!duplicate && duplicate.error() ==
               lux::runtime::ESceneContributionCatalogError::
                   DUPLICATE_FEATURE,
           "catalog rejects duplicate SceneFeatureId values");

    {
        lux::runtime::SceneContributionDescriptor invalid;
        invalid.id = lux::scene::SceneFeatureId{
            "Org.test.scene.invalid"};
        invalid.provider = lux::extensions::ExtensionId{
            "org.test.invalid"};
        invalid.build = [](
            lux::runtime::SceneContributionBatchBuilder&,
            const lux::runtime::SceneContributionBuildContext&,
            lux::runtime::ContributionConfig)
            -> lux::cxx::expected<
                void, lux::runtime::SceneContributionBuildFailure>
        {
            return {};
        };
        const auto rejected = catalog.add(std::move(invalid));
        expect(!rejected && rejected.error() ==
                   lux::runtime::ESceneContributionCatalogError::
                       INVALID_DESCRIPTOR,
               "catalog rejects non-canonical SceneFeatureId values");
    }

    // Cold-start assembly uses the same descriptors but writes into the
    // caller's single unpublished ScheduleBuilder transaction. Services and
    // systems become visible together only when that outer builder commits.
    {
        lux::ecs::World cold_world;
        lux::ecs::Schedule cold_schedule(cold_world);
        lux::ecs::SceneServices cold_services;
        lux::ecs::ScheduleBuilder cold_builder(cold_schedule, cold_services);
        const std::array selected{
            lux::runtime::SceneContributionSelection{
                lux::scene::SceneFeatureId{"org.test.scene.leaf"},
                {}}};
        auto assembled = catalog.stageBootstrap(
            cold_builder,
            selected);
        expect(assembled.has_value() && cold_builder.pendingCount() == 2u &&
                   cold_builder.services().contains<RootSystem>() &&
                   !cold_services.contains<RootSystem>(),
               "cold assembly stages the selected dependency closure");
        expect(cold_builder.commit().has_value() &&
                   cold_services.contains<RootSystem>() &&
                   cold_schedule.systemCount() == 2u,
                "outer schedule commit publishes cold systems and services");
        lux::runtime::SceneContributionHost cold_host(
            cold_schedule,
            cold_services,
            catalog);
        expect(assembled && cold_host.adoptBootstrap(std::move(*assembled)),
               "cold host adopts the committed closure ownership once");
        const auto cold_snapshot = cold_host.activationSnapshot();
        expect(cold_snapshot.size() == 2u &&
                   !cold_snapshot[0].root && cold_snapshot[1].root,
               "cold host records dependencies and the selected root in one active set");
        cold_schedule.tick(0.0f);
        expect(trace == std::vector<int>{1, 2},
               "cold contribution closure preserves dependency order");
        auto disable_leaf = cold_host.facade().requestDisable(
            lux::scene::sceneFeatureId("org.test.scene.leaf"));
        (void)cold_host.processSafePoint();
        auto disable_root = cold_host.facade().requestDisable(
            lux::scene::sceneFeatureId("org.test.scene.root"));
        (void)cold_host.processSafePoint();
        expect(disable_leaf.snapshot().terminal ==
                       lux::extensions::EOperationTerminalState::SUCCEEDED &&
                   disable_root.snapshot().terminal ==
                       lux::extensions::EOperationTerminalState::SUCCEEDED &&
                   cold_schedule.systemCount() == 0u &&
                   !cold_services.contains<RootSystem>() &&
                   trace == std::vector<int>{1, 2, -2, -1},
               "manifest-installed roots use the same disable/removal path");
    }
    trace.clear();

    // A descriptor may fail after earlier dependencies and its own systems
    // have been staged. The catalog owns that closure transaction and must
    // restore the caller's builder exactly to its entry savepoint.
    {
        std::vector<int> rollback_trace;
        lux::runtime::SceneContributionCatalog rollback_catalog;
        expect(rollback_catalog.add(makeRoot(rollback_trace)).has_value() &&
                   rollback_catalog.add(makeFailingAfterStage()).has_value(),
               "catalog accepts a closure which fails during construction");
        lux::ecs::World rollback_world;
        lux::ecs::Schedule rollback_schedule(rollback_world);
        lux::ecs::SceneServices rollback_services;
        lux::ecs::ScheduleBuilder rollback_builder(
            rollback_schedule,
            rollback_services);
        constexpr std::array selected{
            lux::scene::sceneFeatureId(
                "org.test.scene.failing_after_stage")};
        const auto assembled = rollback_catalog.assembleDefaults(
            rollback_builder,
            selected);
        expect(!assembled && rollback_builder.pendingCount() == 0u &&
                   !rollback_builder.services().contains<RootSystem>() &&
                   !rollback_services.contains<RootSystem>() &&
                   rollback_schedule.systemCount() == 0u,
               "failed cold closure rolls systems and services back to its savepoint");
    }


    // Cold bootstrap and dynamic enable must enforce the same declared service
    // surface. An extra service is rejected while the outer builder is still
    // unpublished, and the complete descriptor build is rolled back.
    {
        lux::runtime::SceneContributionCatalog undeclared_catalog;
        expect(
            undeclared_catalog.add(makeUndeclaredService()).has_value(),
            "catalog accepts an undeclared-service fixture for build validation");
        lux::ecs::World undeclared_world;
        lux::ecs::Schedule undeclared_schedule(undeclared_world);
        lux::ecs::SceneServices undeclared_services;
        lux::ecs::ScheduleBuilder undeclared_builder(
            undeclared_schedule,
            undeclared_services);
        const std::array selected{
            lux::runtime::SceneContributionSelection{
                lux::scene::SceneFeatureId{
                    "org.test.scene.undeclared_service"},
                {}}};
        const auto assembled = undeclared_catalog.stageBootstrap(
            undeclared_builder,
            selected);
        expect(
            !assembled && assembled.error().code ==
                    lux::runtime::ESceneContributionAssemblyError::
                        SERVICE_CONFLICT &&
                undeclared_builder.pendingCount() == 0u &&
                !undeclared_builder.committed() &&
                !undeclared_builder.services().contains<UndeclaredService>() &&
                !undeclared_services.contains<UndeclaredService>() &&
                undeclared_schedule.systemCount() == 0u,
            "cold bootstrap rejects undeclared services before builder commit");

        lux::ecs::World dynamic_world;
        lux::ecs::Schedule dynamic_schedule(dynamic_world);
        lux::ecs::SceneServices dynamic_services;
        lux::runtime::SceneContributionHost dynamic_host(
            dynamic_schedule,
            dynamic_services,
            undeclared_catalog);
        auto enabled = dynamic_host.facade().requestEnable(
            lux::scene::sceneFeatureId(
                "org.test.scene.undeclared_service"));
        (void)dynamic_host.processSafePoint();
        expect(
            enabled.snapshot().terminal ==
                    lux::extensions::EOperationTerminalState::FAILED &&
                enabled.snapshot().error ==
                    lux::runtime::ESceneContributionActivationError::
                        SERVICE_CONFLICT &&
                !dynamic_services.contains<UndeclaredService>() &&
                dynamic_schedule.systemCount() == 0u,
            "dynamic enable applies the same undeclared-service rejection");
    }

    lux::ecs::World world;
    lux::ecs::Schedule schedule(world);
    lux::ecs::SceneServices services;
    lux::runtime::SceneContributionHost host(
        schedule,
        services,
        catalog,
        nullptr,
        {.capacity = 8u, .byte_budget = 1024u});

    auto facade = host.facade();
    auto enabled = facade.requestEnable(
        lux::scene::sceneFeatureId("org.test.scene.leaf"));
    expect(enabled.snapshot().terminal ==
               lux::extensions::EOperationTerminalState::PENDING,
           "request returns a non-blocking pending ticket");
    expect(host.processSafePoint() == 1u,
           "owner safe point drains one typed command");
    expect(enabled.snapshot().terminal ==
               lux::extensions::EOperationTerminalState::SUCCEEDED &&
               host.active(lux::scene::sceneFeatureId(
                   "org.test.scene.root")) &&
               host.active(lux::scene::sceneFeatureId(
                   "org.test.scene.leaf")),
           "dependency closure activates atomically before the next tick");
    const auto root_service = host.services().find<RootSystem>();
    expect(root_service.get() != nullptr,
           "installed system is available through a generation-safe service ref");

    schedule.tick(0.0f);
    expect(trace == std::vector<int>{1, 2},
           "steady-state schedule remains one topological pointer walk");

    auto rejected = facade.requestDisable(
        lux::scene::sceneFeatureId("org.test.scene.root"));
    (void)host.processSafePoint();
    expect(rejected.snapshot().terminal ==
               lux::extensions::EOperationTerminalState::FAILED &&
               rejected.snapshot().error ==
                   lux::runtime::ESceneContributionActivationError::
                       REQUIRED_BY_OTHER_FEATURE,
           "disable refuses a contribution with active dependents");

    trace.clear();
    auto disabled = facade.requestDisable(
        lux::scene::sceneFeatureId("org.test.scene.root"),
        lux::runtime::EContributionDisableMode::CASCADE);
    (void)host.processSafePoint();
    expect(disabled.snapshot().terminal ==
               lux::extensions::EOperationTerminalState::SUCCEEDED &&
               !host.active(lux::scene::sceneFeatureId(
                   "org.test.scene.root")) &&
               !host.active(lux::scene::sceneFeatureId(
                   "org.test.scene.leaf")),
           "explicit cascade removes the dependency closure");
    expect(trace == std::vector<int>{-2, -1},
           "dependent contribution is removed before its prerequisite");
    expect(!host.services().find<RootSystem>() && !root_service,
           "borrowed system service generation retires after system removal");

    {
        std::vector<int> rejected_trace;
        lux::runtime::SceneContributionCatalog rejected_catalog;
        expect(rejected_catalog.add(makeRoot(rejected_trace)).has_value() &&
                   rejected_catalog.add(makeBrokenLeaf()).has_value(),
               "catalog accepts a dependency closure used for rollback testing");
        lux::ecs::World rejected_world;
        lux::ecs::Schedule rejected_schedule(rejected_world);
        lux::ecs::SceneServices rejected_services;
        lux::runtime::SceneContributionHost rejected_host(
            rejected_schedule,
            rejected_services,
            rejected_catalog);
        auto rejected_facade = rejected_host.facade();
        auto broken = rejected_facade.requestEnable(
            lux::scene::sceneFeatureId(
                "org.test.scene.broken_leaf"));
        (void)rejected_host.processSafePoint();
        expect(broken.snapshot().terminal ==
                   lux::extensions::EOperationTerminalState::FAILED &&
                   broken.snapshot().error ==
                       lux::runtime::ESceneContributionActivationError::
                           SCHEDULE_REJECTED,
               "invalid aggregate topology rejects the complete dependency closure");
        expect(!rejected_host.active(
                   lux::scene::sceneFeatureId("org.test.scene.root")) &&
                   !rejected_host.active(lux::scene::sceneFeatureId(
                       "org.test.scene.broken_leaf")) &&
                   !rejected_host.services().find<RootSystem>() &&
                   rejected_schedule.systemCount() == 0u,
               "failed closure exposes neither dependency systems nor services");
        expect(rejected_host.close().failed == 0u,
               "failed aggregate install still closes cleanly");
    }

    const auto report = host.close();
    expect(report.failed == 0u,
           "explicit host close reaches a clean terminal state");

    // Close admission and physical removal are distinct safe points. The
    // complete system batch remains installed while each owner consumes one
    // bounded close step per tick, then onRemoved runs in reverse topology.
    {
        std::vector<int> close_trace;
        lux::runtime::SceneContributionCatalog close_catalog;
        expect(close_catalog.add(makeBoundedClose(close_trace)).has_value(),
               "catalog accepts a bounded-close contribution");
        lux::ecs::World close_world;
        lux::ecs::Schedule close_schedule(close_world);
        lux::ecs::SceneServices close_services;
        lux::runtime::SceneContributionHost close_host(
            close_schedule, close_services, close_catalog);
        auto close_facade = close_host.facade();
        auto enabled_close = close_facade.requestEnable(
            lux::scene::sceneFeatureId(
                "org.test.scene.bounded_close"));
        (void)close_host.processSafePoint();
        expect(enabled_close.snapshot().terminal ==
                   lux::extensions::EOperationTerminalState::SUCCEEDED &&
                   close_schedule.systemCount() == 2u,
               "bounded-close systems publish as one contribution batch");

        close_host.requestClose();
        close_schedule.requestClose();
        const auto all_systems_closing = close_schedule.closeState();
        expect(all_systems_closing.valid &&
                   all_systems_closing.pending_systems == 2u &&
                   all_systems_closing.owner_work_pending,
               "Schedule exposes domain-neutral close progress for all systems");
        auto first_close = close_host.close();
        expect(first_close.removed == 0u &&
                   first_close.pending_systems == 2u &&
                   first_close.owner_work_pending &&
                   close_schedule.systemCount() == 2u,
               "close retains systems while both owners are pending");
        close_schedule.tick(0.0f);
        auto second_close = close_host.close();
        expect(second_close.removed == 0u &&
                   second_close.pending_systems == 1u &&
                   close_schedule.systemCount() == 2u,
               "one completed owner cannot remove a still-live peer batch");
        close_schedule.tick(0.0f);
        const auto final_close = close_host.close();
        expect(final_close.complete() && final_close.removed == 1u &&
                   close_schedule.systemCount() == 0u &&
                   close_schedule.closeState().complete,
               "quiescent contribution batch removes atomically");
        expect(close_trace == std::vector<int>{20, 10, -20, -10},
               "multi-system close requests and removes dependents before providers");
    }

    // A private AsyncScope-like owner must not advertise owner tick work while
    // it waits, and its terminal callback must wake the composition root.
    {
        lux::ecs::World external_world;
        lux::ecs::Schedule external_schedule(external_world);
        auto added = external_schedule.addSystem(
            std::make_unique<ExternalCloseSystem>());
        expect(added.has_value(),
               "external-close system installs");
        auto* owner = added ? external_schedule.get(*added) : nullptr;
        std::uint32_t wakeups = 0u;
        external_schedule.requestClose(
            lux::ecs::SystemCloseProgressSink{
                &wakeups,
                [](void* context) noexcept
                {
                    ++*static_cast<std::uint32_t*>(context);
                }});
        const auto waiting = external_schedule.closeState();
        expect(owner && waiting.valid && !waiting.complete &&
                   !waiting.owner_work_pending,
               "external close retains its system without requesting a busy owner tick");
        if (owner)
            owner->completeExternal();
        expect(wakeups == 1u && external_schedule.closeState().complete,
               "external close terminal wakes the composition root exactly once");
        if (added)
            expect(external_schedule.removeSystem(*added).has_value(),
                   "externally quiescent system can be removed at a safe point");
    }
    return failures == 0 ? 0 : 1;
}
