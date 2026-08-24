#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/events/DomainEvents.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/execution/detail/MainThreadMailbox.hpp>
#include <lux/engine/runtime/extensions/ExtensionModuleManager.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/runtime/extensions/EngineExtensions.hpp>
#include <lux/engine/runtime/extensions/EngineExtensionsCloseSender.hpp>
#include <lux/engine/runtime/render/scene/testing/AsyncTestServices.hpp>

#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

namespace
{
    int failures = 0;

    void check(bool value, const char* label)
    {
        std::printf("[%s] %s\n", value ? " ok " : "FAIL", label);
        if (!value)
            ++failures;
    }
}

int main(int argc, char** argv)
{
    check(argc == 5,
          "dependency, component and rollback extension DLL paths supplied");
    if (argc != 5)
        return 1;

    lux::exec::AsyncRuntimeBuilder builder;
    auto plan = std::move(builder).compile();
    if (!plan)
        return 1;
    lux::exec::AsyncRuntime async{std::move(*plan)};
    lux::extensions::ExtensionModuleManager modules;
    auto components = std::make_unique<lux::ecs::ComponentTypeCatalog>();
    check(
        lux::ecs::initializeGeneratedMetadata(*components).has_value(),
        "built-in generated schemas publish before dynamic module loading");
    lux::events::DomainEvents events;
    auto& pump = events.createPump("extensions-test");

    std::size_t loaded_facts = 0u;
    auto subscription = events.subscribe<
        lux::extensions::ExtensionModuleLoaded>(
        pump,
        [&loaded_facts](const auto&) noexcept
        {
            ++loaded_facts;
        });

    std::vector<lux::extensions::ExtensionModuleRequirement> requirements;
    requirements.push_back(
        lux::extensions::ExtensionModuleRequirement::fromPath(
        lux::extensions::ExtensionId{"org.lux.test.minimal"},
        std::filesystem::path{argv[1]},
        lux::extensions::EExtensionModuleTarget::RUNTIME,
        1u,
        1u));
    requirements.push_back(
        lux::extensions::ExtensionModuleRequirement::fromPath(
        lux::extensions::ExtensionId{"org.lux.test.dependent"},
        std::filesystem::path{argv[2]},
        lux::extensions::EExtensionModuleTarget::RUNTIME,
        1u,
        0u));
    requirements.push_back(
        lux::extensions::ExtensionModuleRequirement::fromPath(
        lux::extensions::ExtensionId{"org.lux.test.rollback"},
        std::filesystem::path{argv[4]},
        lux::extensions::EExtensionModuleTarget::RUNTIME,
        1u,
        0u));
    requirements.push_back(
        lux::extensions::ExtensionModuleRequirement::fromPath(
        lux::extensions::ExtensionId{"org.lux.test.component"},
        std::filesystem::path{argv[3]},
        lux::extensions::EExtensionModuleTarget::RUNTIME,
        1u,
        0u));

    auto extensions = std::make_unique<lux::extensions::EngineExtensions>(
        lux::extensions::EngineExtensionServices{
            modules,
            async,
            *components,
            &events,
            {}},
        std::move(requirements));

    const auto unknown = extensions->requestLoad(
        lux::extensions::extensionId("org.lux.test.unknown"));
    check(
        unknown.snapshot().terminal ==
            lux::extensions::EOperationTerminalState::FAILED &&
        unknown.snapshot().error ==
            lux::extensions::EExtensionLoadError::UNKNOWN_EXTENSION,
        "unknown id fails without entering the async runtime");

    const auto ticket = extensions->requestLoad(
        lux::extensions::extensionId("org.lux.test.dependent"));
    for (;;)
    {
        (void)extensions->processSafePoint();
        (void)async.drainMainThreadCompletions();
        const auto current = ticket.snapshot();
        if (current.terminal !=
            lux::extensions::EOperationTerminalState::PENDING)
            break;
        const auto epoch = async.mainThreadMailbox().workEpoch();
        if (async.mainThreadMailbox().emptyApprox())
            async.mainThreadMailbox().waitForWork(epoch);
    }

    const auto result = ticket.snapshot();
    check(
        result.terminal ==
            lux::extensions::EOperationTerminalState::SUCCEEDED,
        "dependency closure loads and registers without host orchestration");
    check(
        modules.requirementStatus(
            lux::extensions::extensionId("org.lux.test.minimal"),
            1u,
            1u) == lux::extensions::EExtensionRequirementStatus::READY,
        "module becomes READY only after registration commit");
    check(
        modules.requirementStatus(
            lux::extensions::extensionId("org.lux.test.dependent"),
            1u,
            0u) == lux::extensions::EExtensionRequirementStatus::READY,
        "root module becomes READY after its dependency");
    pump.drain();
    check(loaded_facts == 2u, "each committed module publishes one domain fact");

    const auto duplicate = extensions->requestLoad(
        lux::extensions::extensionId("org.lux.test.dependent"));
    (void)extensions->processSafePoint();
    check(
        duplicate.snapshot().terminal ==
            lux::extensions::EOperationTerminalState::SUCCEEDED,
        "repeated request reuses the ready module without loading the DLL");

    const auto component_ticket = extensions->requestLoad(
        lux::extensions::extensionId("org.lux.test.component"));
    for (;;)
    {
        (void)extensions->processSafePoint();
        (void)async.drainMainThreadCompletions();
        if (component_ticket.snapshot().terminal !=
            lux::extensions::EOperationTerminalState::PENDING)
            break;
        const auto epoch = async.mainThreadMailbox().workEpoch();
        if (async.mainThreadMailbox().emptyApprox())
            async.mainThreadMailbox().waitForWork(epoch);
    }
    check(
        component_ticket.snapshot().terminal ==
            lux::extensions::EOperationTerminalState::SUCCEEDED,
        "dynamic component module registration succeeds");
    const auto* dynamic_schema = components->findBySchema(
        "org.lux.test.component.dynamic_test_component");
    check(
        dynamic_schema &&
            dynamic_schema->provider == "org.lux.test.component" &&
            static_cast<bool>(dynamic_schema->lifetime),
        "generated component schema commits with provider and module lease");
    pump.drain();
    check(loaded_facts == 3u, "component module publishes one domain fact");

    const auto component_count = components->all().size();
    const auto reflected_count =
        lux::meta::ReflectionRegistry::instance().classes().size();
    const auto rollback_ticket = extensions->requestLoad(
        lux::extensions::extensionId("org.lux.test.rollback"));
    for (;;)
    {
        (void)extensions->processSafePoint();
        (void)async.drainMainThreadCompletions();
        if (rollback_ticket.snapshot().terminal !=
            lux::extensions::EOperationTerminalState::PENDING)
            break;
        const auto epoch = async.mainThreadMailbox().workEpoch();
        if (async.mainThreadMailbox().emptyApprox())
            async.mainThreadMailbox().waitForWork(epoch);
    }
    check(
        rollback_ticket.snapshot().terminal ==
                lux::extensions::EOperationTerminalState::FAILED &&
            rollback_ticket.snapshot().error ==
                lux::extensions::EExtensionLoadError::
                    CATALOG_VALIDATION_FAILED,
        "real DLL with duplicate schema rejects the registration transaction");
    check(
        lux::meta::ReflectionRegistry::instance().findClass(
            "lux::test::RollbackOnlyType") == nullptr &&
            lux::meta::ReflectionRegistry::instance().classes().size() ==
                reflected_count,
        "failed DLL publishes no reflected metadata");
    check(
        components->all().size() == component_count,
        "failed DLL leaves the component catalog unchanged");
    pump.drain();
    check(loaded_facts == 3u,
          "failed DLL publishes no successful module fact");

    bool close_complete = false;
    lux::extensions::EngineExtensionsCloseReport close_report;
    ::experimental::execution::start_detached(
        extensions->closeAsync() |
        stdexec::then(
            [&close_complete, &close_report](auto report) noexcept
            {
                close_report = std::move(report);
                close_complete = true;
            }));
    while (!close_complete)
    {
        (void)async.drainMainThreadCompletions();
        if (close_complete)
            break;
        const auto epoch = async.mainThreadMailbox().workEpoch();
        if (async.mainThreadMailbox().emptyApprox())
            async.mainThreadMailbox().waitForWork(epoch);
    }
    check(close_report.terminal(), "extension facade closes with no pending load");
    extensions.reset();
    (void)async.drainMainThreadCompletions();
    check(
        !modules.close().has_value(),
        "component catalogue lease prevents premature DLL unload");
    components.reset();
    lux::meta::meta_module_deinit();
    check(
        modules.close().has_value(),
        "module unload occurs after catalog and reflection teardown");
    lux::runtime::testing::detail::closeRuntime(async);

    return failures == 0 ? 0 : 1;
}
