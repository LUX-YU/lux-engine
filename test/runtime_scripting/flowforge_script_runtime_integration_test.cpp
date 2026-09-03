#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/ArithmeticNode.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>
#include <lux/engine/function/script/native/NativeModule.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/ScriptSystemDescription.hpp>
#include <lux/engine/simulation/abilities/DelayAbility.hpp>
#include "DelayAbility.ability.generated.hpp"
#include <lux/engine/simulation/scripting/native/NativeScriptBackend.hpp>
#include <lux/engine/task/TaskExecutor.hpp>

#include <array>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef LUX_BENCHMARK_GIT_COMMIT
#define LUX_BENCHMARK_GIT_COMMIT "unknown"
#endif
#ifndef LUX_BENCHMARK_BUILD_TYPE
#define LUX_BENCHMARK_BUILD_TYPE "unknown"
#endif

namespace
{
    using namespace lux;
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    inline constexpr system::SystemInstanceId kProbeSystem{0x8301U};
    inline constexpr HookPointId kTickHook{0x8302U};
    inline constexpr lux::script::ScriptSymbolId kTickSymbol{0x8303U};
    inline constexpr auto kTestContract = lux::script::ScriptApiContractIdView{"lux.test.flowforge.runtime"};
    inline constexpr auto kTestMethod = lux::script::ScriptApiMethodIdView{"lux.test.flowforge.runtime.write"};
    inline constexpr auto kReadMethod = lux::script::ScriptApiMethodIdView{"lux.test.flowforge.runtime.read"};
    inline constexpr auto kEagerMethod = lux::script::ScriptApiMethodIdView{"lux.test.flowforge.runtime.eager"};
    inline constexpr std::uint64_t kTestSchema{0x83048304ULL};
    std::size_t g_hook_capacity{1U};

    [[nodiscard]] asset::AssetId assetId()
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes[0] = 0x83U;
        return asset::AssetId{bytes};
    }

    struct ProbeSystem final
    {
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr std::array Hooks{makeHookPointSpec<void()>(kTickHook, "tick")};
        inline static constexpr SimulationSystemDescription Description{
            .type = {.canonical_name = "lux.test.flowforge.runtime.probe", .version = 1U},
            .hooks = Hooks
        };

        ProbeSystem() noexcept : endpoint(kProbeSystem, kTickHook, hook)
        {
            ready = hook.prepare(g_hook_capacity) == EEndpointMutationError::NONE;
        }

        void execute() noexcept
        {
            static_cast<void>(hook.dispatch());
        }

        HookPoint<void()> hook;
        ScriptHookEndpoint<void()> endpoint;
        bool ready{};
    };

    [[nodiscard]] lux::cxx::expected<void, SimulationSystemBuildFailure> installProbe(
        SimulationBuilder& builder,
        SimulationSystemView description
    ) noexcept
    {
        auto probe = builder.emplaceSystem<ProbeSystem>(description.instanceId());
        if (!probe)
            return lux::cxx::unexpected(probe.error());
        if (!(*probe)->ready)
        {
            return lux::cxx::unexpected(SimulationSystemBuildFailure{
                ESimulationSystemBuildError::CONSTRUCTION_FAILURE,
                description.instanceId()
            });
        }
        auto published = builder.publishScriptHook(description.instanceId(), (*probe)->endpoint.descriptor());
        if (!published)
            return published;
        return builder.addSystemTask<ProbeSystem>(
            description.instanceId(),
            [](ProbeSystem& value) noexcept { value.execute(); }
        );
    }

    [[nodiscard]] SimulationSystemRegistration probeRegistration() noexcept
    {
        return {
            .type = system::systemTypeId(ProbeSystem::Description.type.canonical_name),
            .cpp_type = cxx::typeToken<ProbeSystem>(),
            .description = &ProbeSystem::Description,
            .access = ProbeSystem::Access.spec(),
            .configuration = {},
            .install = &installProbe
        };
    }

    struct TestProvider final
    {
        std::int32_t value{};
        std::size_t calls{};
        std::size_t eager_starts{};
        std::uint64_t checksum{};

        static lux::script::ScriptAbilityErasedCallResult write(
            void* opaque,
            const void*,
            std::span<const lux::script::ScriptAbilityInputSlot> arguments,
            std::span<lux::script::ScriptAbilityOutputSlot> results
        ) noexcept
        {
            if (arguments.size() != 1U || arguments.front().data == nullptr || !results.empty())
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{91});
            auto& self = *static_cast<TestProvider*>(opaque);
            self.value = *static_cast<const std::int32_t*>(arguments.front().data);
            self.checksum += static_cast<std::uint32_t>(self.value);
            ++self.calls;
            return {};
        }

        static lux::script::ScriptAbilityErasedCallResult read(
            void* opaque,
            const void*,
            std::span<const lux::script::ScriptAbilityInputSlot> arguments,
            std::span<lux::script::ScriptAbilityOutputSlot> results
        ) noexcept
        {
            if (arguments.size() != 1U || arguments.front().data == nullptr || results.size() != 1U ||
                results.front().data == nullptr)
            {
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{94});
            }
            auto& self = *static_cast<TestProvider*>(opaque);
            *static_cast<std::int32_t*>(results.front().data) =
                self.value + *static_cast<const std::int32_t*>(arguments.front().data);
            ++self.calls;
            return {};
        }

        static lux::script::ScriptAbilityStartResult eager(
            void* opaque,
            const void*,
            std::span<const lux::script::ScriptAbilityInputSlot> arguments,
            lux::script::ScriptAbilityErasedCompletion completion
        ) noexcept
        {
            if (!arguments.empty())
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{92});
            auto& self = *static_cast<TestProvider*>(opaque);
            ++self.eager_starts;
            const auto completed = completion.success();
            if (!completed)
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{93});
            return {};
        }
    };

    static constexpr auto kI32 = lux::script::makeScriptAbilityValue<std::int32_t>(
        lux::script::EScriptAbilityValueLifetime::OWNED_VALUE
    );
    static constexpr std::array kWriteParameters{
        lux::script::ScriptAbilityParameterDescription{"value", kI32}
    };
    static constexpr std::array kTestMethods{
        lux::script::ScriptAbilityMethodDescription{
            kTestMethod,
            "write",
            "Write",
            lux::script::EScriptApiMethodKind::COMMAND,
            kWriteParameters,
            {}
        },
        lux::script::ScriptAbilityMethodDescription{
            kReadMethod,
            "read",
            "Read",
            lux::script::EScriptApiMethodKind::QUERY,
            kWriteParameters,
            std::span{&kI32, 1U}
        },
        lux::script::ScriptAbilityMethodDescription{
            kEagerMethod,
            "eager",
            "Eager",
            lux::script::EScriptApiMethodKind::ASYNC_OPERATION,
            {},
            {}
        }
    };
    static constexpr lux::script::ScriptAbilityDescription kTestAbility{
        kTestContract,
        "Runtime Test",
        1U,
        kTestSchema,
        lux::script::EScriptAbilityReceiverKind::PROVIDER_INSTANCE,
        kTestMethods
    };
    static constexpr std::array kTestErasedMethods{
        lux::script::ScriptAbilityErasedMethodBinding{
            kTestMethod,
            lux::script::EScriptApiMethodKind::COMMAND,
            kWriteParameters,
            {},
            &TestProvider::write,
            nullptr
        },
        lux::script::ScriptAbilityErasedMethodBinding{
            kReadMethod,
            lux::script::EScriptApiMethodKind::QUERY,
            kWriteParameters,
            std::span{&kI32, 1U},
            &TestProvider::read,
            nullptr
        },
        lux::script::ScriptAbilityErasedMethodBinding{
            kEagerMethod,
            lux::script::EScriptApiMethodKind::ASYNC_OPERATION,
            {},
            {},
            nullptr,
            &TestProvider::eager
        }
    };
    static constexpr std::array kTestNodes{
        lux::flowforge::ScriptAbilityNodeDescription{
            kTestContract,
            kTestMethod,
            "Runtime Test",
            "Write",
            1U,
            kTestSchema,
            lux::script::EScriptAbilityReceiverKind::PROVIDER_INSTANCE,
            lux::script::EScriptApiMethodKind::COMMAND,
            kWriteParameters,
            {}
        },
        lux::flowforge::ScriptAbilityNodeDescription{
            kTestContract,
            kReadMethod,
            "Runtime Test",
            "Read",
            1U,
            kTestSchema,
            lux::script::EScriptAbilityReceiverKind::PROVIDER_INSTANCE,
            lux::script::EScriptApiMethodKind::QUERY,
            kWriteParameters,
            std::span{&kI32, 1U}
        },
        lux::flowforge::ScriptAbilityNodeDescription{
            kTestContract,
            kEagerMethod,
            "Runtime Test",
            "Eager",
            1U,
            kTestSchema,
            lux::script::EScriptAbilityReceiverKind::PROVIDER_INSTANCE,
            lux::script::EScriptApiMethodKind::ASYNC_OPERATION,
            {},
            {}
        }
    };

    [[nodiscard]] flowforge::FlowGraph makeGraph(
        const flowforge::ScriptAbilityNodeDescription& next_step
    )
    {
        flowforge::FlowGraph graph;
        const auto* i32 = &meta::ref_type_of_v<std::int32_t>;
        const auto counter = graph.addVariable("counter", i32, meta::RuntimeObject(std::int32_t{}));
        const flowforge::DataPinInfo counter_info{"counter", i32};
        auto event = std::make_unique<flowforge::OnEventNode>("tick");
        auto eager = std::make_unique<flowforge::ScriptAbilityNode>(kTestNodes[2]);
        auto next = std::make_unique<flowforge::ScriptAbilityNode>(next_step);
        auto get = std::make_unique<flowforge::GetVariableNode>(counter, counter_info);
        auto add = std::make_unique<flowforge::BinaryOpNode>(flowforge::ENodeOperation::ADD, i32);
        auto set = std::make_unique<flowforge::SetVariableNode>(counter, counter_info);
        auto write = std::make_unique<flowforge::ScriptAbilityNode>(kTestNodes.front());
        assert(const_cast<flowforge::DataInPin&>(add->rhs()).setConstantData(meta::RuntimeObject(std::int32_t{111})));
        auto* event_ptr = event.get();
        auto* eager_ptr = eager.get();
        auto* next_ptr = next.get();
        auto* get_ptr = get.get();
        auto* add_ptr = add.get();
        auto* set_ptr = set.get();
        auto* write_ptr = write.get();
        const auto event_slot = graph.addNodes(std::move(event));
        graph.addNodes(std::move(eager));
        graph.addNodes(std::move(next));
        graph.addNodes(std::move(get));
        graph.addNodes(std::move(add));
        graph.addNodes(std::move(set));
        graph.addNodes(std::move(write));
        flowforge::LastLink previous;
        assert(event_ptr->execOutPin().linkTo(&eager_ptr->execInPin(), previous) == flowforge::ELinkError::SUCCESS);
        assert(eager_ptr->execOutPin().linkTo(&next_ptr->execInPin(), previous) == flowforge::ELinkError::SUCCESS);
        assert(next_ptr->execOutPin().linkTo(&set_ptr->execInPin(), previous) == flowforge::ELinkError::SUCCESS);
        assert(set_ptr->execOutPin().linkTo(&write_ptr->execInPin(), previous) == flowforge::ELinkError::SUCCESS);
        assert(const_cast<flowforge::DataOutPin&>(get_ptr->valuePin()).linkTo(
            const_cast<flowforge::DataInPin*>(&add_ptr->lhs()), previous) == flowforge::ELinkError::SUCCESS);
        assert(const_cast<flowforge::DataOutPin&>(add_ptr->result()).linkTo(
            const_cast<flowforge::DataInPin*>(&set_ptr->valueIn()), previous) == flowforge::ELinkError::SUCCESS);
        assert(const_cast<flowforge::DataOutPin&>(set_ptr->valueOut()).linkTo(
            write_ptr->parameterPins().front().get(), previous) == flowforge::ELinkError::SUCCESS);
        assert(graph.addExport({
            flowforge::FlowForgeExportNodeId{1U},
            graph.getNode(event_slot).node->id(),
            kTickSymbol
        }));
        return graph;
    }

    [[nodiscard]] flowforge::FlowGraph makeSyncGraph()
    {
        flowforge::FlowGraph graph;
        const auto* i32 = &meta::ref_type_of_v<std::int32_t>;
        const auto counter = graph.addVariable("counter", i32, meta::RuntimeObject(std::int32_t{}));
        const flowforge::DataPinInfo counter_info{"counter", i32};
        auto event = std::make_unique<flowforge::OnEventNode>("tick");
        auto get = std::make_unique<flowforge::GetVariableNode>(counter, counter_info);
        auto add = std::make_unique<flowforge::BinaryOpNode>(flowforge::ENodeOperation::ADD, i32);
        auto set = std::make_unique<flowforge::SetVariableNode>(counter, counter_info);
        auto write = std::make_unique<flowforge::ScriptAbilityNode>(kTestNodes.front());
        auto read = std::make_unique<flowforge::ScriptAbilityNode>(kTestNodes[1]);
        assert(const_cast<flowforge::DataInPin&>(add->rhs()).setConstantData(meta::RuntimeObject(std::int32_t{17})));
        assert(read->parameterPins().front()->setConstantData(meta::RuntimeObject(std::int32_t{1})));
        auto* event_ptr = event.get();
        auto* write_ptr = write.get();
        auto* read_ptr = read.get();
        auto* get_ptr = get.get();
        auto* add_ptr = add.get();
        auto* set_ptr = set.get();
        const auto event_slot = graph.addNodes(std::move(event));
        graph.addNodes(std::move(write));
        graph.addNodes(std::move(read));
        graph.addNodes(std::move(get));
        graph.addNodes(std::move(add));
        graph.addNodes(std::move(set));
        flowforge::LastLink previous;
        assert(event_ptr->execOutPin().linkTo(&set_ptr->execInPin(), previous) == flowforge::ELinkError::SUCCESS);
        assert(set_ptr->execOutPin().linkTo(&write_ptr->execInPin(), previous) == flowforge::ELinkError::SUCCESS);
        assert(write_ptr->execOutPin().linkTo(&read_ptr->execInPin(), previous) == flowforge::ELinkError::SUCCESS);
        assert(const_cast<flowforge::DataOutPin&>(get_ptr->valuePin()).linkTo(
            const_cast<flowforge::DataInPin*>(&add_ptr->lhs()), previous) == flowforge::ELinkError::SUCCESS);
        assert(const_cast<flowforge::DataOutPin&>(add_ptr->result()).linkTo(
            const_cast<flowforge::DataInPin*>(&set_ptr->valueIn()), previous) == flowforge::ELinkError::SUCCESS);
        assert(const_cast<flowforge::DataOutPin&>(set_ptr->valueOut()).linkTo(
            write_ptr->parameterPins().front().get(), previous) == flowforge::ELinkError::SUCCESS);
        assert(graph.addExport({
            flowforge::FlowForgeExportNodeId{2U},
            graph.getNode(event_slot).node->id(),
            kTickSymbol
        }));
        return graph;
    }

    struct ArtifactSource final
    {
        const lux::script::ScriptArtifact* artifact{};
        const lux::script::NativeModule* module{};

        static bool resolveArtifact(
            void* opaque,
            const asset::AssetId& requested,
            ResolvedScriptArtifact& result
        ) noexcept
        {
            const auto& self = *static_cast<ArtifactSource*>(opaque);
            if (requested != assetId())
                return false;
            result.artifact = self.artifact;
            return true;
        }

        static bool resolveModule(
            void* opaque,
            const asset::AssetId& requested,
            const lux::script::ScriptArtifact&,
            ResolvedNativeModule& result
        ) noexcept
        {
            const auto& self = *static_cast<ArtifactSource*>(opaque);
            if (requested != assetId())
                return false;
            result.module = self.module;
            return true;
        }
    };

    struct BenchmarkOptions final
    {
        std::string group;
        std::size_t size{2500U};
        std::size_t frames{30U};
        std::size_t warmups{5U};
        std::size_t resume_budget{2000U};
        std::uint64_t seed{0x5EED2026ULL};
        std::filesystem::path output{"flowforge_script_runtime_benchmark.csv"};
    };

    [[nodiscard]] bool parseSize(std::string_view text, std::size_t& value) noexcept
    {
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value != 0U;
    }

    [[nodiscard]] std::optional<BenchmarkOptions> parseBenchmarkOptions(int argc, char** argv)
    {
        if (argc == 1)
            return std::nullopt;
        BenchmarkOptions result;
        for (int index{1}; index < argc; ++index)
        {
            if (index + 1 >= argc)
                return std::nullopt;
            const std::string_view key{argv[index++]};
            const std::string_view value{argv[index]};
            if (key == "--group")
                result.group = value;
            else if (key == "--size")
            {
                if (!parseSize(value, result.size))
                    return std::nullopt;
            }
            else if (key == "--frames")
            {
                if (!parseSize(value, result.frames))
                    return std::nullopt;
            }
            else if (key == "--warmups")
            {
                if (!parseSize(value, result.warmups))
                    return std::nullopt;
            }
            else if (key == "--resume-budget")
            {
                if (!parseSize(value, result.resume_budget))
                    return std::nullopt;
            }
            else if (key == "--seed")
            {
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result.seed);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                    return std::nullopt;
            }
            else if (key == "--output")
                result.output = value;
            else if (key == "--mode")
            {
                if (value == "performance" && result.warmups == 5U && result.frames == 30U)
                {
                    result.warmups = 300U;
                    result.frames = 5000U;
                }
                else if (value != "diagnostic" && value != "performance")
                    return std::nullopt;
            }
            else
                return std::nullopt;
        }
        constexpr std::array groups{
            std::string_view{"micro-flowforge-sync"},
            std::string_view{"micro-flowforge-ability-query"},
            std::string_view{"micro-flowforge-suspend"},
            std::string_view{"micro-flowforge-resume"},
            std::string_view{"scene-flowforge-update-heavy"},
            std::string_view{"scene-flowforge-gameplay-mixed"},
            std::string_view{"scene-flowforge-suspended-idle"},
            std::string_view{"scene-flowforge-resume-storm"}
        };
        return std::ranges::find(groups, result.group) == groups.end()
            ? std::nullopt
            : std::optional<BenchmarkOptions>{std::move(result)};
    }
}

namespace
{
    int runBenchmark(const BenchmarkOptions& options)
    {
        using Clock = std::chrono::steady_clock;
        using namespace lux;
        using namespace lux::simulation;
        using namespace lux::simulation::script;
        const bool sync = options.group == "micro-flowforge-sync" ||
            options.group == "micro-flowforge-ability-query" ||
            options.group == "scene-flowforge-update-heavy";
        const bool idle = options.group == "scene-flowforge-suspended-idle";
        const bool storm = options.group == "scene-flowforge-resume-storm" ||
            options.group == "micro-flowforge-resume";
        const bool suspend_only = options.group == "micro-flowforge-suspend";
        g_hook_capacity = options.size;

        flowforge::ScriptAbilityNodeCatalog catalog;
        if (!catalog.add(flowforge::makeScriptAbilityCatalogContribution<DelayAbility>()) ||
            !catalog.add({kTestNodes}))
        {
            return 10;
        }
        const auto* next_step = catalog.view().find(
            lux::script::ScriptApiContractIdView{"lux.simulation.delay"},
            lux::script::ScriptApiMethodIdView{"lux.simulation.delay.next_step"}
        );
        if (next_step == nullptr)
            return 11;
        auto graph = sync ? makeSyncGraph() : makeGraph(*next_step);
        auto artifact = flowforge::compileFlowForgeScript(
            graph,
            flowforge::FlowForgeCompileOptions{
                .module_name = sync ? "lux.benchmark.flowforge.sync" : "lux.benchmark.flowforge.async",
                .script_abilities = catalog.view()
            }
        );
        if (!artifact)
        {
            std::fprintf(
                stderr,
                "FlowForge benchmark compile failed: %u %s\n",
                static_cast<unsigned>(artifact.error().code),
                artifact.error().message.c_str()
            );
            return 12;
        }
        auto native_module = lux::script::loadNativeModule(
            artifact->payload(),
            sync ? "lux.benchmark.flowforge.sync" : "lux.benchmark.flowforge.async"
        );
        if (!native_module)
            return 13;

        SimulationDescriptionBuilder simulation_builder;
        if (!simulation_builder.addSystem(kProbeSystem, "probe", ProbeSystem::Description))
            return 14;
        auto simulation_description = std::move(simulation_builder).build();
        if (!simulation_description)
            return 15;
        ecs::Registry registry;
        SimulationSystemRegistry system_types;
        if (!system_types.add(probeRegistration()))
            return 16;
        auto simulation = Simulation::create(
            registry,
            std::make_shared<SimulationDescription>(std::move(*simulation_description)),
            system_types
        );
        if (!simulation)
            return 17;

        ScriptSystemDescriptionBuilder script_builder;
        for (std::size_t index{}; index < options.size; ++index)
        {
            if (!script_builder.addMount({
                ScriptMountId{index + 1U},
                assetId(),
                SimulationScriptMount{},
                true,
                {{kTickSymbol, HookScriptTarget{kProbeSystem, kTickHook}}}
            }))
            {
                return 18;
            }
        }
        auto script_description = std::move(script_builder).build(simulation->description());
        if (!script_description)
            return 19;

        ArtifactSource source{std::addressof(*artifact), std::addressof(*native_module)};
        NativeScriptBackend backend{
            {std::addressof(source), &ArtifactSource::resolveModule},
            NativeScriptBackendConfig{
                .module_capacity = 1U,
                .instance_capacity = options.size,
                .prepared_call_capacity = options.size * (sync ? 1U : 2U),
                .continuation_capacity = options.size,
                .max_ability_imports_per_module = 4U,
                .max_continuation_frame_bytes = 8192U
            }
        };
        if (!backend)
            return 20;
        TestProvider provider;
        const lux::script::ScriptAbilityBinding test_binding{
            &kTestAbility,
            std::addressof(provider),
            std::addressof(provider),
            kTestErasedMethods
        };
        const std::array capabilities{publishScriptAbility(test_binding)};
        const auto backend_descriptor = backend.descriptor();
        auto system = ScriptSystem::create(
            simulation->description(),
            *script_description,
            registry,
            simulation->clock(),
            ScriptRuntimeLimits{
                16U,
                options.size,
                options.size,
                1U,
                options.size,
                options.size,
                64U,
                (std::min)(options.resume_budget, options.size),
                options.size,
                options.size
            },
            {std::addressof(source), &ArtifactSource::resolveArtifact},
            {},
            capabilities,
            std::span{&backend_descriptor, 1U},
            simulation->scriptHookEndpoints(),
            simulation->scriptEventEndpoints()
        );
        if (!system)
            return 21;
        const auto prepared = system->prepare();
        if (!prepared)
        {
            std::fprintf(stderr, "FlowForge benchmark prepare failed: %u\n", static_cast<unsigned>(prepared.error()));
            return 21;
        }
        auto executor = task::TaskExecutor::create({0U, 1U});
        if (!executor)
            return 22;
        const auto normal_frame = [&] {
            return simulation->execute(*executor, SimulationDuration{1}) && system->executeStablePoint();
        };
        if (idle || storm)
        {
            if (!simulation->execute(*executor, SimulationDuration{1}))
                return 23;
            while (system->stats().resume_queue_depth != 0U)
            {
                if (!system->executeStablePoint())
                    return 23;
            }
            if (storm && !simulation->execute(*executor, SimulationDuration{1}))
                return 24;
        }
        else if (!suspend_only)
        {
            for (std::size_t frame{}; frame < options.warmups; ++frame)
            {
                if (!normal_frame())
                    return 25;
            }
        }

        std::error_code directory_error;
        if (!options.output.parent_path().empty())
            std::filesystem::create_directories(options.output.parent_path(), directory_error);
        std::ofstream output(options.output, std::ios::trunc);
        if (!output)
            return 26;
        output << "git_commit,build_type,scenario,backend,size,seed,frame,nanoseconds,active_instances,calls,"
                  "continuations,awaitables,queue_depth,queue_high_water,continuation_frame_bytes,artifact_bytes,checksum\n";
        const auto* exported = native_module->findFunction(kTickSymbol);
        const auto frame_bytes = exported != nullptr && exported->step != nullptr ? exported->step->frame_size : 0U;
        const auto record = [&](std::size_t frame, std::uint64_t nanoseconds) {
            const auto stats = system->stats();
            output << LUX_BENCHMARK_GIT_COMMIT << ',' << LUX_BENCHMARK_BUILD_TYPE << ',' << options.group
                   << ",flowforge-aot," << options.size << ',' << options.seed << ',' << frame << ',' << nanoseconds
                   << ',' << stats.active_instances << ',' << provider.calls << ',' << stats.active_continuations << ','
                   << stats.active_awaitables << ',' << stats.resume_queue_depth << ',' << stats.resume_queue_high_water
                   << ',' << frame_bytes << ',' << artifact->payload().size() << ','
                   << (provider.checksum ^ provider.calls) << '\n';
        };

        if (suspend_only)
        {
            const auto begin = Clock::now();
            if (!simulation->execute(*executor, SimulationDuration{1}))
                return 27;
            record(0U, std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
        }
        else if (storm)
        {
            std::size_t frame{};
            do
            {
                const auto begin = Clock::now();
                if (!system->executeStablePoint())
                    return 28;
                record(frame++, std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
            } while (system->activeContinuationCount() != 0U && frame < options.frames);
        }
        else
        {
            for (std::size_t frame{}; frame < options.frames; ++frame)
            {
                const auto begin = Clock::now();
                const bool success = idle ? static_cast<bool>(system->executeStablePoint()) : normal_frame();
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
                if (!success)
                    return 29;
                record(frame, elapsed);
            }
        }
        return system->shutdown() ? 0 : 30;
    }
}

int main(int argc, char** argv)
{
    if (argc > 1)
    {
        const auto options = parseBenchmarkOptions(argc, argv);
        return options ? runBenchmark(*options) : 2;
    }
    using namespace lux;
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    flowforge::ScriptAbilityNodeCatalog catalog;
    assert(catalog.add(flowforge::makeScriptAbilityCatalogContribution<DelayAbility>()));
    assert(catalog.add({kTestNodes}));
    const auto* next_step = catalog.view().find(
        lux::script::ScriptApiContractIdView{"lux.simulation.delay"},
        lux::script::ScriptApiMethodIdView{"lux.simulation.delay.next_step"}
    );
    assert(next_step != nullptr);
    auto graph = makeGraph(*next_step);
    auto artifact = flowforge::compileFlowForgeScript(
        graph,
        flowforge::FlowForgeCompileOptions{
            .module_name = "lux.test.flowforge.runtime",
            .script_abilities = catalog.view()
        }
    );
    if (!artifact)
    {
        std::fprintf(
            stderr,
            "FlowForge runtime compile failed: %u %s\n",
            static_cast<unsigned>(artifact.error().code),
            artifact.error().message.c_str()
        );
    }
    assert(artifact);
    auto native_module = lux::script::loadNativeModule(artifact->payload(), "lux.test.flowforge.runtime");
    assert(native_module);

    SimulationDescriptionBuilder simulation_builder;
    assert(simulation_builder.addSystem(kProbeSystem, "probe", ProbeSystem::Description));
    auto simulation_description = std::move(simulation_builder).build();
    assert(simulation_description);
    ecs::Registry registry;
    SimulationSystemRegistry system_types;
    assert(system_types.add(probeRegistration()));
    auto simulation = Simulation::create(
        registry,
        std::make_shared<SimulationDescription>(std::move(*simulation_description)),
        system_types
    );
    assert(simulation);

    ScriptSystemDescriptionBuilder script_builder;
    assert(script_builder.addMount({
        ScriptMountId{1U},
        assetId(),
        SimulationScriptMount{},
        true,
        {{kTickSymbol, HookScriptTarget{kProbeSystem, kTickHook}}}
    }));
    auto script_description = std::move(script_builder).build(simulation->description());
    assert(script_description);

    ArtifactSource source{std::addressof(*artifact), std::addressof(*native_module)};
    NativeScriptBackend backend{
        {std::addressof(source), &ArtifactSource::resolveModule},
        NativeScriptBackendConfig{
            .module_capacity = 1U,
            .instance_capacity = 1U,
            .prepared_call_capacity = 4U,
            .continuation_capacity = 2U,
            .max_ability_imports_per_module = 4U,
            .max_continuation_frame_bytes = 8192U
        }
    };
    assert(backend);
    TestProvider provider;
    const lux::script::ScriptAbilityBinding test_binding{
        &kTestAbility,
        std::addressof(provider),
        std::addressof(provider),
        kTestErasedMethods
    };
    const std::array capabilities{publishScriptAbility(test_binding)};
    const auto backend_descriptor = backend.descriptor();
    auto missing = ScriptSystem::create(
        simulation->description(),
        *script_description,
        registry,
        simulation->clock(),
        ScriptRuntimeLimits{8U, 1U, 4U, 4U, 4U, 4U, 64U, 1U, 4U, 4U},
        {std::addressof(source), &ArtifactSource::resolveArtifact},
        {},
        {},
        std::span{&backend_descriptor, 1U},
        simulation->scriptHookEndpoints(),
        simulation->scriptEventEndpoints()
    );
    assert(missing);
    const auto missing_prepared = missing->prepare();
    assert(!missing_prepared && missing_prepared.error() == EScriptSystemError::SCRIPT_CAPABILITY_NOT_FOUND);
    assert(missing->shutdown());

    auto mismatch_capability = capabilities.front();
    ++mismatch_capability.schema_hash;
    const std::array mismatch_capabilities{mismatch_capability};
    auto mismatch = ScriptSystem::create(
        simulation->description(),
        *script_description,
        registry,
        simulation->clock(),
        ScriptRuntimeLimits{8U, 1U, 4U, 4U, 4U, 4U, 64U, 1U, 4U, 4U},
        {std::addressof(source), &ArtifactSource::resolveArtifact},
        {},
        mismatch_capabilities,
        std::span{&backend_descriptor, 1U},
        simulation->scriptHookEndpoints(),
        simulation->scriptEventEndpoints()
    );
    assert(mismatch);
    const auto mismatch_prepared = mismatch->prepare();
    assert(!mismatch_prepared && mismatch_prepared.error() == EScriptSystemError::SCRIPT_CAPABILITY_SCHEMA_MISMATCH);
    assert(mismatch->shutdown());

    auto system = ScriptSystem::create(
        simulation->description(),
        *script_description,
        registry,
        simulation->clock(),
        ScriptRuntimeLimits{8U, 1U, 4U, 4U, 4U, 4U, 64U, 1U, 4U, 4U},
        {std::addressof(source), &ArtifactSource::resolveArtifact},
        {},
        capabilities,
        std::span{&backend_descriptor, 1U},
        simulation->scriptHookEndpoints(),
        simulation->scriptEventEndpoints()
    );
    assert(system);
    assert(system->prepare());

    auto executor = task::TaskExecutor::create({0U, 1U});
    assert(executor);
    assert(simulation->execute(*executor, SimulationDuration{1}));
    assert(system->activeContinuationCount() == 1U);
    assert(provider.eager_starts == 1U);
    assert(provider.calls == 0U);
    assert(system->executeStablePoint());
    assert(system->activeContinuationCount() == 1U);
    assert(provider.calls == 0U);
    assert(simulation->execute(*executor, SimulationDuration{1}));
    assert(system->executeStablePoint());
    assert(system->activeContinuationCount() == 0U);
    assert(system->activeAwaitableCount() == 0U);
    assert(provider.calls == 1U && provider.value == 111);
    assert(system->shutdown());

    auto retiring = ScriptSystem::create(
        simulation->description(),
        *script_description,
        registry,
        simulation->clock(),
        ScriptRuntimeLimits{8U, 1U, 4U, 4U, 4U, 4U, 64U, 1U, 4U, 4U},
        {std::addressof(source), &ArtifactSource::resolveArtifact},
        {},
        capabilities,
        std::span{&backend_descriptor, 1U},
        simulation->scriptHookEndpoints(),
        simulation->scriptEventEndpoints()
    );
    assert(retiring);
    assert(retiring->prepare());
    const auto calls_before_retirement = provider.calls;
    assert(simulation->execute(*executor, SimulationDuration{1}));
    assert(retiring->activeContinuationCount() == 1U);
    assert(retiring->shutdown());
    assert(retiring->activeContinuationCount() == 0U);
    assert(retiring->activeAwaitableCount() == 0U);
    assert(simulation->execute(*executor, SimulationDuration{1}));
    assert(provider.calls == calls_before_retirement);
    return 0;
}
