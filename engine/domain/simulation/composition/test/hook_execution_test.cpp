#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <cassert>
#include <memory>
#include <thread>

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;
    constexpr lux::system::SystemInstanceId A{1U}, B{2U}, D{3U}, C{4U};
    constexpr HookPointId Hook{1U};

    struct Probe final
    {
        std::barrier<> producers{2};
        std::atomic<unsigned> active{};
        std::atomic<unsigned> high_water{};
        std::atomic<unsigned> finished{};
        std::thread::id caller{std::this_thread::get_id()};
        unsigned workers{};
        unsigned script_calls{};
        bool consumer_saw_script{};
    };

    Probe* installing_probe{};

    struct Producer final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr SimulationSystemDescription Description{
            .type = {.canonical_name = "lux.test.execution.producer", .version = 1U}
        };
        Probe* probe;
        unsigned lane;
        void execute() noexcept
        {
            assert(probe->workers == 0U || std::this_thread::get_id() != probe->caller);
            const auto active = probe->active.fetch_add(1U) + 1U;
            auto high = probe->high_water.load();
            while (high < active && !probe->high_water.compare_exchange_weak(high, active))
            {}
            if (probe->workers >= 2U)
                probe->producers.arrive_and_wait();
            probe->finished.fetch_or(1U << lane);
            probe->active.fetch_sub(1U);
        }
    };

    struct Host final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr std::array Hooks{makeHookPointSpec<void()>(Hook, "gameplay", true, true)};
        inline static constexpr SimulationSystemDescription Description{
            .type = {.canonical_name = "lux.test.execution.host", .version = 1U}, .hooks = Hooks
        };
        explicit Host(Probe& value) noexcept : probe(&value), endpoint(D, Hook, hook)
        {
            assert(hook.prepare(1U) == EEndpointMutationError::NONE);
        }
        Probe* probe;
        HookPoint<void()> hook;
        ScriptHookEndpoint<void()> endpoint;
    };

    struct Consumer final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr SimulationSystemDescription Description{
            .type = {.canonical_name = "lux.test.execution.consumer", .version = 1U}
        };
        Probe* probe;
    };

    template <class Type, auto Install>
    SimulationSystemRegistration registration()
    {
        return {lux::system::systemTypeId(Type::Description.type.canonical_name), lux::cxx::typeToken<Type>(),
            &Type::Description, Type::Access.spec(), {}, Install};
    }

    auto installProducer(SimulationBuilder& builder, SimulationSystemView view) noexcept
        -> lux::cxx::expected<void, SimulationSystemBuildFailure>
    {
        auto value = builder.emplaceSystem<Producer>(view.instanceId(), installing_probe,
            static_cast<unsigned>(view.instanceId() == A ? 0U : 1U));
        if (!value)
            return lux::cxx::unexpected(value.error());
        return builder.addSystemTask<Producer>(view.instanceId(), [](Producer& producer) noexcept {
            producer.execute();
        });
    }

    auto installHost(SimulationBuilder& builder, SimulationSystemView view) noexcept
        -> lux::cxx::expected<void, SimulationSystemBuildFailure>
    {
        auto value = builder.emplaceSystem<Host>(view.instanceId(), *installing_probe);
        if (!value)
            return lux::cxx::unexpected(value.error());
        auto published = builder.publishScriptHook(view.instanceId(), (*value)->endpoint.descriptor());
        if (!published)
            return published;
        auto primary = builder.addSystemTask<Host>(view.instanceId(), [](Host& host) noexcept {
            host.probe->finished.fetch_or(4U);
        });
        if (!primary)
            return primary;
        return builder.addSystemHookTask<Host>(view.instanceId(), Hook,
            [](Host& host, const HookInvocation& invocation) noexcept {
            static_cast<void>(host.hook.dispatch(invocation));
        });
    }

    auto installConsumer(SimulationBuilder& builder, SimulationSystemView view) noexcept
        -> lux::cxx::expected<void, SimulationSystemBuildFailure>
    {
        auto value = builder.emplaceSystem<Consumer>(view.instanceId(), installing_probe);
        if (!value)
            return lux::cxx::unexpected(value.error());
        return builder.addSystemTask<Consumer>(view.instanceId(), [](Consumer& consumer) noexcept {
            assert(consumer.probe->script_calls == 1U);
            consumer.probe->consumer_saw_script = true;
        });
    }

    struct Backend final
    {
        Probe* probe;
        lux::script::ScriptArtifact artifact;
        static int invoke(lux_script_call_frame* frame) noexcept
        {
            auto& probe = *static_cast<Probe*>(frame->user_context);
            assert(std::this_thread::get_id() == probe.caller);
            assert(probe.active.load() == 0U && probe.finished.load() == 7U);
            ++probe.script_calls;
            return 0;
        }
        ScriptBackendDescriptor descriptor() noexcept
        {
            return {lux::rdesc::Script::Kind::CPP_STATIC, this,
                [](void* context, const ScriptInstanceCreateContext&, const lux::script::ScriptArtifact&,
                    ScriptBackendInstance& instance) noexcept {
                    instance.value = static_cast<Backend*>(context)->probe;
                    return EScriptBackendResult::SUCCESS;
                },
                [](void*, ScriptBackendInstance instance, const lux::rdesc::ScriptFunction&,
                    ScriptBackendPreparedMethod& method) noexcept {
                    method = {instance.value, {&invoke, instance.value}, {}};
                    return EScriptBackendResult::SUCCESS;
                },
                [](void*, ScriptBackendInstance, ScriptBackendPreparedMethod) noexcept {},
                [](void*, ScriptBackendInstance) noexcept {}};
        }
    };

    void run(unsigned workers, bool reverse, bool omit_provider_edge = false, bool cycle = false)
    {
        Probe probe;
        probe.workers = workers;
        installing_probe = &probe;
        SimulationSystemRegistry registrations;
        assert(registrations.add(registration<Producer, &installProducer>()));
        assert(registrations.add(registration<Host, &installHost>()));
        assert(registrations.add(registration<Consumer, &installConsumer>()));
        SimulationDescriptionBuilder builder;
        const auto add_producers = [&] {
            assert(builder.addSystem(A, "first", Producer::Description));
            assert(builder.addSystem(B, "second", Producer::Description));
        };
        if (!reverse)
            add_producers();
        assert(builder.addSystem(D, "host", Host::Description));
        assert(builder.addSystem(C, "consumer", Consumer::Description));
        if (reverse)
            add_producers();
        for (auto producer : {A, B, D})
        {
            if (producer == D && omit_provider_edge)
                continue;
            assert(builder.addExecutionDependency(SimulationExecutionPoint::task(producer),
                SimulationExecutionPoint::hook(D, Hook)));
        }
        assert(builder.addExecutionDependency(SimulationExecutionPoint::hook(D, Hook),
            SimulationExecutionPoint::task(C)));
        if (cycle)
            assert(builder.addExecutionDependency(
                SimulationExecutionPoint::task(C), SimulationExecutionPoint::task(A)));
        auto built = std::move(builder).build();
        assert(built);
        auto description = std::make_shared<SimulationDescription>(std::move(*built));
        ecs::Registry registry;
        auto simulation = Simulation::create(registry, description, registrations);
        if (omit_provider_edge || cycle)
        {
            assert(!simulation);
            assert(simulation.error().code == (cycle ? ESimulationSystemBuildError::DEPENDENCY_CYCLE :
                ESimulationSystemBuildError::AMBIGUOUS_HOOK_ORDER));
            assert(probe.finished.load() == 0U && probe.script_calls == 0U);
            return;
        }
        assert(simulation);
        // Initially no Script binding; late admission must still require a runtime hook.
        assert(simulation->seal());
        lux::rdesc::Script script_description;
        script_description.module_name = "lux.test.execution.script";
        script_description.body = lux::rdesc::CppStaticScript{"synthetic-owner-probe"};
        script_description.exports = {{"tick", 1U, {}, {}}};
        auto artifact = lux::script::ScriptArtifact::create(std::move(script_description), {});
        assert(artifact);
        Backend backend{&probe, std::move(*artifact)};
        const auto descriptor = backend.descriptor();
        ScriptSystemDescriptionBuilder scripts;
        std::array<std::uint8_t, 16U> asset_bytes{};
        asset_bytes.front() = 1U;
        assert(scripts.addMount({ScriptMountId{1U}, lux::asset::AssetId{asset_bytes}, SimulationScriptMount{}, true,
            {{1U, HookScriptTarget{D, Hook}}}}));
        auto mounted = std::move(scripts).build(*description);
        assert(mounted);
        auto runtime = ScriptSystem::create(*description, *mounted, registry, simulation->clock(),
            {4U, 1U, 1U, 1U, 1U, 1U, 32U, 1U, 1U, 1U, 1U, 1U},
            {&backend, [](void* context, const lux::asset::AssetId&, ResolvedScriptArtifact& result) noexcept {
                result.artifact = &static_cast<Backend*>(context)->artifact;
                return true;
            }}, {}, {}, {&descriptor, 1U}, simulation->scriptHookEndpoints(), {});
        assert(runtime && runtime->prepare());
        auto executor = lux::task::TaskExecutor::create({workers, 16U});
        assert(executor && !simulation->execute(*executor, SimulationDuration{1}));
        assert(probe.script_calls == 0U && probe.finished.load() == 0U);
        assert(simulation->clock().snapshot().step_index == 0U);
        auto connection = simulation->bindHookCallbacks({&*runtime,
            [](void* context, const SimulationClockSnapshot&, bool) noexcept {
                auto& runtime = *static_cast<ScriptSystem*>(context);
                runtime.beginStableAdmission();
                return static_cast<bool>(runtime.processLifecycle());
            },
            [](void* context, const SimulationClockSnapshot&, bool) noexcept {
                return static_cast<bool>(static_cast<ScriptSystem*>(context)->executeStablePoint());
            }, nullptr});
        assert(connection);
        assert(executor && simulation->execute(*executor, SimulationDuration{1}));
        assert(probe.script_calls == 1U && probe.consumer_saw_script);
        assert(probe.high_water.load() == (workers >= 2U ? 2U : 1U));
        assert(runtime->shutdown());
    }
}

int main()
{
    for (unsigned workers : {0U, 1U, 2U, 4U})
    {
        run(workers, false);
        run(workers, true);
    }
    std::jthread caller([] { run(4U, true); });
    caller.join();
    run(2U, false, true);
    run(2U, false, false, true);
}
