#include "ScriptBenchmarkAbility.hpp"
#include "ScriptBenchmarkAbility.ability.generated.hpp"

#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/abilities/DelayAbility.hpp>
#include "DelayAbility.ability.generated.hpp"
#include <lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>
#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptBridge.hpp>
#if LUX_BENCHMARK_HAS_LUA
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#endif
#if LUX_BENCHMARK_HAS_NATIVE
#include <lux/engine/simulation/scripting/native/NativeScriptBackend.hpp>
#endif
#include <lux/engine/task/TaskExecutor.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if LUX_BENCHMARK_HAS_NATIVE
#include <lux/engine/function/script/native/NativeModule.hpp>
#endif

#ifndef LUX_BENCHMARK_GIT_COMMIT
#define LUX_BENCHMARK_GIT_COMMIT "unknown"
#endif
#ifndef LUX_BENCHMARK_BUILD_TYPE
#define LUX_BENCHMARK_BUILD_TYPE "unknown"
#endif
#ifndef LUX_BENCHMARK_COMPILER
#define LUX_BENCHMARK_COMPILER "unknown"
#endif

namespace
{
    using Clock = std::chrono::steady_clock;
    using namespace lux::simulation;
    using namespace lux::simulation::script;
    namespace benchmark = lux::simulation::script::benchmark;

    std::atomic_size_t g_allocation_count{};
    std::atomic_bool g_count_allocations{};

    enum class EScenarioMode : std::uint8_t
    {
        SYNC,
        EXTERNAL_AWAIT,
        EAGER_AWAIT,
        NEXT_STEP,
        SIMULATION_DELAY,
        MIXED,
    };

    struct Options final
    {
        std::string group{"micro-sync"};
        std::string mode{"diagnostic"};
        std::size_t size{2500U};
        std::size_t frames{30U};
        std::size_t warmups{5U};
        std::size_t resume_budget{2000U};
        std::size_t ready_count{10000U};
        std::uint64_t seed{0x5EED2026ULL};
        std::filesystem::path output{"script_runtime_benchmark.csv"};
    };

    [[nodiscard]] bool parseSize(std::string_view text, std::size_t& output) noexcept
    {
        const auto result = std::from_chars(text.data(), text.data() + text.size(), output);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size() && output != 0U;
    }

    [[nodiscard]] bool parseU64(std::string_view text, std::uint64_t& output) noexcept
    {
        const auto result = std::from_chars(text.data(), text.data() + text.size(), output);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size();
    }

    [[nodiscard]] std::optional<Options> parseOptions(int argc, char** argv)
    {
        Options result;
        bool frames_supplied{};
        for (int index{1}; index < argc; ++index)
        {
            if (index + 1 >= argc)
                return std::nullopt;
            const std::string_view key{argv[index++]};
            const std::string_view value{argv[index]};
            if (key == "--group")
                result.group = value;
            else if (key == "--mode")
                result.mode = value;
            else if (key == "--size")
            {
                if (!parseSize(value, result.size))
                    return std::nullopt;
            }
            else if (key == "--frames")
            {
                if (!parseSize(value, result.frames))
                    return std::nullopt;
                frames_supplied = true;
            }
            else if (key == "--seed")
            {
                if (!parseU64(value, result.seed))
                    return std::nullopt;
            }
            else if (key == "--resume-budget")
            {
                if (!parseSize(value, result.resume_budget))
                    return std::nullopt;
            }
            else if (key == "--ready")
            {
                if (!parseSize(value, result.ready_count))
                    return std::nullopt;
            }
            else if (key == "--output")
                result.output = value;
            else
                return std::nullopt;
        }
        if (result.mode == "performance")
        {
            result.warmups = 300U;
            if (!frames_supplied)
                result.frames = 5000U;
        }
        else if (result.mode != "diagnostic")
            return std::nullopt;

        constexpr std::array groups{
            std::string_view{"micro-sync"},
            std::string_view{"micro-async"},
            std::string_view{"micro-lifecycle"},
            std::string_view{"scene-update-heavy"},
            std::string_view{"scene-gameplay-mixed"},
            std::string_view{"scene-suspended-idle"},
            std::string_view{"scene-resume-storm"},
            std::string_view{"scene-object-churn"},
            std::string_view{"scheduler-next-step"},
            std::string_view{"scheduler-simulation-delay"}
        };
        if (std::find(groups.begin(), groups.end(), result.group) == groups.end())
            return std::nullopt;
        result.ready_count = (std::min)(result.ready_count, result.size);
        return result;
    }

    struct Row final
    {
        std::string scenario;
        std::string backend;
        std::size_t size{};
        std::size_t sample{};
        std::uint64_t nanoseconds{};
        std::size_t allocations{};
        std::size_t active_instances{};
        std::size_t calls{};
        std::size_t ability_calls{};
        std::size_t suspensions{};
        std::size_t resumes{};
        std::size_t continuations{};
        std::size_t awaitables{};
        std::size_t queue_depth{};
        std::size_t queue_high_water{};
        std::size_t lifecycle_begins{};
        std::size_t lifecycle_ends{};
        std::uint64_t checksum{};
    };

    void writeCsv(const Options& options, std::span<const Row> rows)
    {
        if (!options.output.parent_path().empty())
            std::filesystem::create_directories(options.output.parent_path());
        auto temporary = options.output;
        temporary += ".tmp";
        std::ofstream output(temporary, std::ios::trunc);
        if (!output)
            throw std::runtime_error("cannot open benchmark output");
        output << "benchmark_schema_version,git_commit,build_type,compiler,os,logical_cpu_count,scenario,backend,"
                  "size,seed,sample,nanoseconds,"
                  "allocations,active_instances,calls,ability_calls,events,suspensions,resumes,continuations,"
                  "awaitables,queue_depth,queue_high_water,lifecycle_begins,lifecycle_ends,checksum\n";
        for (const auto& row : rows)
        {
            output << "1," << LUX_BENCHMARK_GIT_COMMIT << ',' << LUX_BENCHMARK_BUILD_TYPE << ','
                   << LUX_BENCHMARK_COMPILER << ",windows," << std::thread::hardware_concurrency() << ','
                   << row.scenario << ',' << row.backend << ',' << row.size << ',' << options.seed << ','
                   << row.sample << ','
                   << row.nanoseconds << ',' << row.allocations << ',' << row.active_instances << ',' << row.calls
                   << ',' << row.ability_calls << ",0," << row.suspensions << ',' << row.resumes << ','
                   << row.continuations << ',' << row.awaitables << ',' << row.queue_depth << ','
                   << row.queue_high_water << ',' << row.lifecycle_begins << ',' << row.lifecycle_ends << ','
                   << row.checksum << '\n';
        }
        output.close();
        std::error_code error;
        std::filesystem::remove(options.output, error);
        std::filesystem::rename(temporary, options.output);
    }

    [[nodiscard]] lux::asset::AssetId assetId() noexcept
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.front() = 0xB0U;
        return lux::asset::AssetId{bytes};
    }

    [[nodiscard]] lux::world::WorldObjectId objectId(std::size_t index) noexcept
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.front() = 0xB1U;
        for (std::size_t byte{}; byte < sizeof(std::uint64_t); ++byte)
            bytes[8U + byte] = static_cast<std::uint8_t>((index >> (byte * 8U)) & 0xFFU);
        return lux::world::WorldObjectId{uuids::uuid{bytes}};
    }

    inline constexpr lux::system::SystemInstanceId kSystem{0xB001U};
    inline constexpr HookPointId kHook{0xB002U};
    inline constexpr lux::script::ScriptSymbolId kBegin{0xB003U};
    inline constexpr lux::script::ScriptSymbolId kTick{0xB004U};
    inline constexpr lux::script::ScriptSymbolId kEnd{0xB005U};

    [[nodiscard]] SimulationDescription scriptDescription()
    {
        constexpr std::array hooks{makeHookPointSpec<void()>(kHook, "benchmark-update")};
        const SimulationSystemDescription system{
            .type = {.canonical_name = "lux.benchmark.ScriptRuntime", .version = 1U},
            .hooks = hooks
        };
        SimulationDescriptionBuilder builder;
        if (!builder.addSystem(kSystem, "script-runtime-benchmark", system))
            throw std::runtime_error("benchmark simulation description rejected");
        auto result = std::move(builder).build();
        if (!result)
            throw std::runtime_error("benchmark simulation description build failed");
        return std::move(*result);
    }

    [[nodiscard]] std::shared_ptr<const SimulationDescription> emptyDescription()
    {
        SimulationDescriptionBuilder builder;
        auto result = std::move(builder).build();
        if (!result)
            throw std::runtime_error("empty clock simulation description rejected");
        return std::make_shared<SimulationDescription>(std::move(*result));
    }

    struct ValueProvider final
    {
        std::size_t calls{};
        std::uint64_t checksum{};
        std::int32_t value{7};

        std::int32_t read(std::int32_t input) noexcept
        {
            ++calls;
            checksum += static_cast<std::uint32_t>(input + value);
            return input + value;
        }

        void write(std::int32_t input) noexcept
        {
            ++calls;
            value = input;
            checksum += static_cast<std::uint32_t>(input);
        }
    };

    struct BackendState;

    struct RuntimeObject final
    {
        BackendState* owner{};
        std::size_t serial{};
        std::uint64_t value{};
        std::optional<lux::script::ScriptAbilityCpp<benchmark::ValueAbility>> ability;
        std::optional<lux::script::ScriptAbilityStarter<DelayAbility>> delay;
    };

    struct PreparedCall final
    {
        RuntimeObject* object{};
        lux::script::ScriptSymbolId symbol{};
    };

    struct Continuation final
    {
        RuntimeObject* object{};
    };

    struct BackendState final
    {
        EScenarioMode mode{EScenarioMode::SYNC};
        double delay_seconds{1.0};
        std::size_t creates{};
        std::size_t destroys{};
        std::size_t prepares{};
        std::size_t releases{};
        std::size_t begins{};
        std::size_t ends{};
        std::size_t calls{};
        std::size_t suspensions{};
        std::size_t resumes{};
        std::size_t continuation_destroys{};
        std::uint64_t checksum{};
        std::vector<ScriptAwaitableCompletion> completions;
    };

    int invokePrepared(lux_script_call_frame* frame) noexcept
    {
        auto& prepared = *static_cast<PreparedCall*>(frame->user_context);
        auto& object = *prepared.object;
        auto& state = *object.owner;
        if (prepared.symbol == kBegin)
        {
            object.value = object.serial + 1U;
            ++state.begins;
            return 0;
        }
        if (prepared.symbol == kEnd)
        {
            ++state.ends;
            state.checksum += object.value;
            return 0;
        }
        ++state.calls;
        ++object.value;
        state.checksum += object.value;
        return 0;
    }

    EScriptBackendResult createInstance(
        void* opaque,
        const ScriptInstanceCreateContext& context,
        const lux::script::ScriptArtifact&,
        ScriptBackendInstance& output
    ) noexcept
    {
        auto& state = *static_cast<BackendState*>(opaque);
        auto* object = new (std::nothrow) RuntimeObject{&state, state.creates++, 0U};
        if (!object)
            return EScriptBackendResult::ALLOCATION_FAILURE;
        for (const auto& capability : context.capabilities)
        {
            const lux::script::ScriptAbilityBinding value_binding{
                &lux::script::ScriptAbilityTraits<benchmark::ValueAbility>::Description,
                capability.context,
                capability.dispatch
            };
            const bool is_value_capability = capability.contract.hash() == value_binding.description->id.hash() &&
                capability.contract.name() == value_binding.description->id.name();
            if (is_value_capability)
            {
                auto created = lux::script::ScriptAbilityCpp<benchmark::ValueAbility>::create(value_binding);
                if (!created)
                {
                    delete object;
                    return EScriptBackendResult::CONSTRUCTION_FAILURE;
                }
                object->ability.emplace(std::move(*created));
                continue;
            }
            const lux::script::ScriptAbilityBinding delay_binding{
                &lux::script::ScriptAbilityTraits<DelayAbility>::Description,
                capability.context,
                capability.dispatch
            };
            const bool is_delay_capability = capability.contract.hash() == delay_binding.description->id.hash() &&
                capability.contract.name() == delay_binding.description->id.name();
            if (is_delay_capability)
            {
                auto created = lux::script::ScriptAbilityStarter<DelayAbility>::create(delay_binding);
                if (!created)
                {
                    delete object;
                    return EScriptBackendResult::CONSTRUCTION_FAILURE;
                }
                object->delay.emplace(std::move(*created));
            }
        }
        output.value = object;
        return EScriptBackendResult::SUCCESS;
    }

    EScriptBackendResult prepareMethod(
        void* opaque,
        ScriptBackendInstance instance,
        const lux::rdesc::ScriptFunction& function,
        lux::script::BoundScriptCall& output
    ) noexcept
    {
        auto* prepared = new (std::nothrow) PreparedCall{
            static_cast<RuntimeObject*>(instance.value), function.symbol_id};
        if (!prepared)
            return EScriptBackendResult::ALLOCATION_FAILURE;
        ++static_cast<BackendState*>(opaque)->prepares;
        output = {&invokePrepared, prepared};
        return EScriptBackendResult::SUCCESS;
    }

    void releaseMethod(void* opaque, ScriptBackendInstance, lux::script::BoundScriptCall call) noexcept
    {
        ++static_cast<BackendState*>(opaque)->releases;
        delete static_cast<PreparedCall*>(call.context);
    }

    void destroyInstance(void* opaque, ScriptBackendInstance instance) noexcept
    {
        ++static_cast<BackendState*>(opaque)->destroys;
        delete static_cast<RuntimeObject*>(instance.value);
    }

    ScriptStepResult resumeContinuation(
        void* opaque,
        ScriptStepContext&,
        const ScriptResumePacket& packet
    ) noexcept
    {
        auto& continuation = *static_cast<Continuation*>(opaque);
        auto& object = *continuation.object;
        ++object.owner->resumes;
        ++object.value;
        object.owner->checksum += object.value + static_cast<std::uint8_t>(packet.state);
        return ScriptStepResult::completed();
    }

    void destroyContinuation(void* opaque) noexcept
    {
        auto* continuation = static_cast<Continuation*>(opaque);
        ++continuation->object->owner->continuation_destroys;
        delete continuation;
    }

    ScriptStepResult invokeStep(
        void* opaque,
        lux_script_call_frame&,
        ScriptStepContext& step,
        ScriptBackendContinuation& output
    ) noexcept
    {
        auto& object = *static_cast<RuntimeObject*>(opaque);
        auto& state = *object.owner;
        ++state.calls;
        ScriptStepResult result{ScriptStepResult::completed()};
        bool suspend{};
        if (state.mode == EScenarioMode::EXTERNAL_AWAIT || state.mode == EScenarioMode::EAGER_AWAIT)
        {
            auto awaiting = step.awaitables.create();
            if (!awaiting)
                return ScriptStepResult::failed(1);
            if (state.mode == EScenarioMode::EAGER_AWAIT)
            {
                if (!awaiting->completion.ready())
                    return ScriptStepResult::failed(6);
            }
            else
                state.completions.push_back(awaiting->completion);
            result = ScriptStepResult::suspended(awaiting->id);
            suspend = true;
        }
        else if (state.mode == EScenarioMode::NEXT_STEP)
        {
            if (!object.delay)
                return ScriptStepResult::failed(2);
            result = invokeScriptAbilityAsync<void>(
                step,
                [&object](lux::script::ScriptAbilityCompletion<void> completion) noexcept {
                    return object.delay->nextStep(std::move(completion));
                }
            );
            suspend = result.state == EScriptStepState::SUSPENDED;
        }
        else if (state.mode == EScenarioMode::SIMULATION_DELAY)
        {
            if (!object.delay)
                return ScriptStepResult::failed(3);
            result = invokeScriptAbilityAsync<void>(
                step,
                [&object, &state](lux::script::ScriptAbilityCompletion<void> completion) noexcept {
                    return object.delay->simulationSeconds(state.delay_seconds, std::move(completion));
                }
            );
            suspend = result.state == EScriptStepState::SUSPENDED;
        }
        else if (state.mode == EScenarioMode::MIXED)
        {
            const auto lane = object.serial % 20U;
            if (lane < 10U)
                ++object.value;
            else if (lane < 14U && object.ability)
                object.value += static_cast<std::uint32_t>(object.ability->read(static_cast<std::int32_t>(lane)));
            else if (lane < 16U && object.ability)
                object.ability->write(static_cast<std::int32_t>(lane));
            else if (lane == 18U)
            {
                auto awaiting = step.awaitables.create();
                if (!awaiting)
                    return ScriptStepResult::failed(4);
                state.completions.push_back(awaiting->completion);
                result = ScriptStepResult::suspended(awaiting->id);
                suspend = true;
            }
            else if (lane == 19U && object.delay)
            {
                result = invokeScriptAbilityAsync<void>(
                    step,
                    [&object](lux::script::ScriptAbilityCompletion<void> completion) noexcept {
                        return object.delay->nextStep(std::move(completion));
                    }
                );
                suspend = result.state == EScriptStepState::SUSPENDED;
            }
            state.checksum += object.value;
        }
        if (!suspend)
            return result;
        auto* continuation = new (std::nothrow) Continuation{&object};
        if (!continuation)
            return ScriptStepResult::failed(5);
        ++state.suspensions;
        output = {continuation, &resumeContinuation, &destroyContinuation};
        return result;
    }

    EScriptBackendResult prepareStepMethod(
        void* opaque,
        ScriptBackendInstance instance,
        const lux::rdesc::ScriptFunction& function,
        BoundScriptStepCall& output
    ) noexcept
    {
        const auto mode = static_cast<BackendState*>(opaque)->mode;
        if (function.symbol_id == kTick && mode != EScenarioMode::SYNC)
            output = {instance.value, &invokeStep};
        return EScriptBackendResult::SUCCESS;
    }

    void releaseStepMethod(void*, ScriptBackendInstance, BoundScriptStepCall) noexcept
    {
    }

    struct RuntimeHarness final
    {
        RuntimeHarness(
            std::size_t count,
            EScenarioMode mode,
            std::size_t resume_budget,
            bool entity_scope = false,
            bool prepare_now = true
        )
            : simulation_description(scriptDescription()), backend_state{.mode = mode}, entity_scope(entity_scope)
        {
            auto clock_simulation = Simulation::create(registry, emptyDescription(), empty_system_types);
            if (!clock_simulation)
                throw std::runtime_error("clock simulation create failed");
            clock_owner.emplace(std::move(*clock_simulation));
            auto created_executor = lux::task::TaskExecutor::create({0U, 1U});
            if (!created_executor)
                throw std::runtime_error("clock executor create failed");
            executor.emplace(std::move(*created_executor));

            objects.reserve(count);
            entities.reserve(count);
            ScriptSystemDescriptionBuilder description_builder;
            for (std::size_t index{}; index < count; ++index)
            {
                ScriptMountScope scope{SimulationScriptMount{}};
                if (entity_scope)
                {
                    const auto object = objectId(index);
                    const auto entity = registry.create();
                    objects.push_back(object);
                    entities.push_back(entity);
                    object_index.emplace(object, index);
                    scope = EntityScriptMount{object};
                }
                if (!description_builder.addMount({
                        ScriptMountId{index + 1U},
                        assetId(),
                        scope,
                        true,
                        {{kTick, HookScriptTarget{kSystem, kHook}}}
                    }))
                {
                    throw std::runtime_error("benchmark mount rejected");
                }
            }
            auto built = std::move(description_builder).build(simulation_description);
            if (!built)
                throw std::runtime_error("benchmark ScriptSystem description rejected");
            system_description.emplace(std::move(*built));

            lux::rdesc::Script description;
            description.module_name = "lux.benchmark.runtime";
            description.exports = {
                {"admit", kBegin, {}, {}},
                {"update", kTick, {}, {}},
                {"retire", kEnd, {lux::rdesc::makeScriptValueType<EScriptEndPlayReason>()}, {}}
            };
            description.lifecycle = {kBegin, kEnd};
            if (mode == EScenarioMode::NEXT_STEP || mode == EScenarioMode::SIMULATION_DELAY ||
                mode == EScenarioMode::MIXED)
            {
                const auto& delay = lux::script::ScriptAbilityTraits<DelayAbility>::Description;
                description.api_requirements.push_back({lux::script::ScriptApiContractId{delay.id.name()},
                                                        delay.schema_hash});
            }
            if (mode == EScenarioMode::MIXED)
            {
                const auto& value = lux::script::ScriptAbilityTraits<benchmark::ValueAbility>::Description;
                description.api_requirements.push_back({lux::script::ScriptApiContractId{value.id.name()},
                                                        value.schema_hash});
            }
            description.body = lux::rdesc::CppStaticScript{"benchmark-synthetic"};
            auto created_artifact = lux::script::ScriptArtifact::create(std::move(description), {});
            if (!created_artifact)
                throw std::runtime_error("benchmark ScriptArtifact rejected");
            artifact.emplace(std::move(*created_artifact));

            if (hook.prepare(1U) != EEndpointMutationError::NONE)
                throw std::runtime_error("benchmark HookPoint prepare failed");
            hook_bridge = std::make_unique<ScriptHookEndpoint<void()>>(kSystem, kHook, hook);
            hook_descriptor = hook_bridge->descriptor();
            backend_state.completions.reserve(count);
            backend = {
                lux::rdesc::Script::Kind::CPP_STATIC,
                &backend_state,
                &createInstance,
                &prepareMethod,
                &releaseMethod,
                &destroyInstance,
                &prepareStepMethod,
                &releaseStepMethod
            };

            std::array<ScriptApiCapabilityPublication, 1U> publications{};
            std::span<const ScriptApiCapabilityPublication> capability_span;
            if (mode == EScenarioMode::MIXED)
            {
                value_binding = lux::script::bindScriptAbility<benchmark::ValueAbility>(value_provider);
                publications[0] = publishScriptAbility(value_binding);
                capability_span = publications;
            }
            const std::size_t bounded_count = (std::max)(count, std::size_t{1U});
            auto created = ScriptSystem::create(
                simulation_description,
                *system_description,
                registry,
                clock_owner->clock(),
                ScriptRuntimeLimits{
                    bounded_count,
                    bounded_count,
                    bounded_count,
                    1U,
                    bounded_count,
                    bounded_count,
                    64U,
                    (std::max)(resume_budget, std::size_t{1U}),
                    bounded_count,
                    bounded_count
                },
                {this, &resolveArtifact},
                entity_scope ? WorldObjectResolver{this, &resolveWorld} : WorldObjectResolver{},
                capability_span,
                std::span{&backend, 1U},
                std::span{&hook_descriptor, 1U},
                {}
            );
            if (!created)
                throw std::runtime_error("benchmark ScriptSystem create failed");
            system.emplace(std::move(*created));
            if (prepare_now && !system->prepare())
                throw std::runtime_error("benchmark ScriptSystem prepare failed");
        }

        ~RuntimeHarness()
        {
            if (system)
                static_cast<void>(system->shutdown());
        }

        RuntimeHarness(const RuntimeHarness&) = delete;
        RuntimeHarness& operator=(const RuntimeHarness&) = delete;

        static bool resolveArtifact(
            void* opaque,
            const lux::asset::AssetId& requested,
            ResolvedScriptArtifact& output
        ) noexcept
        {
            auto& self = *static_cast<RuntimeHarness*>(opaque);
            if (requested != assetId())
                return false;
            output.artifact = std::addressof(*self.artifact);
            return true;
        }

        static bool resolveWorld(
            void* opaque,
            const lux::world::WorldObjectId& object,
            ecs::Entity& output
        ) noexcept
        {
            auto& self = *static_cast<RuntimeHarness*>(opaque);
            const auto found = self.object_index.find(object);
            if (found == self.object_index.end() || found->second >= self.entities.size())
                return false;
            output = self.entities[found->second];
            return self.registry.valid(output);
        }

        void advance(SimulationDuration delta)
        {
            if (!clock_owner->execute(*executor, delta))
                throw std::runtime_error("benchmark clock advance failed");
        }

        void stablePoint()
        {
            if (!system->executeStablePoint())
                throw std::runtime_error("benchmark stable point failed");
        }

        void completePending(std::size_t maximum)
        {
            const auto count = (std::min)(maximum, backend_state.completions.size());
            for (std::size_t index{}; index < count; ++index)
            {
                if (!backend_state.completions[index].ready())
                    throw std::runtime_error("benchmark completion failed");
            }
            backend_state.completions.erase(
                backend_state.completions.begin(),
                backend_state.completions.begin() + static_cast<std::ptrdiff_t>(count)
            );
        }

        void churn(std::size_t first, std::size_t count)
        {
            if (!entity_scope || entities.empty())
                throw std::runtime_error("benchmark churn requires entity scope");
            for (std::size_t offset{}; offset < count; ++offset)
            {
                const auto index = (first + offset) % entities.size();
                registry.destroy(entities[index]);
                entities[index] = registry.create();
            }
            stablePoint();
        }

        SimulationDescription simulation_description;
        ecs::Registry registry;
        SimulationSystemRegistry empty_system_types;
        std::optional<Simulation> clock_owner;
        std::optional<lux::task::TaskExecutor> executor;
        std::optional<ScriptSystemDescription> system_description;
        std::optional<lux::script::ScriptArtifact> artifact;
        HookPoint<void()> hook;
        std::unique_ptr<ScriptHookEndpoint<void()>> hook_bridge;
        ScriptHookEndpointDescriptor hook_descriptor;
        BackendState backend_state;
        ScriptBackendDescriptor backend;
        ValueProvider value_provider;
        lux::script::ScriptAbilityBinding value_binding;
        std::optional<ScriptSystem> system;
        std::vector<lux::world::WorldObjectId> objects;
        std::vector<ecs::Entity> entities;
        std::unordered_map<lux::world::WorldObjectId, std::size_t, lux::world::WorldObjectIdHash> object_index;
        bool entity_scope{};
    };

    template <class Operation>
    Row measureRow(
        std::string scenario,
        std::string backend,
        std::size_t size,
        std::size_t sample,
        Operation&& operation
    );

    void runBackendLifecycle(
        const Options& options,
        std::vector<Row>& rows,
        std::string backend_name,
        ScriptBackendDescriptor descriptor,
        const lux::script::ScriptArtifact& artifact,
        bool entity_scope,
        std::size_t begin_index = 0U,
        std::size_t tick_index = 1U,
        std::size_t end_index = 2U
    )
    {
        struct Calls final
        {
            lux::script::BoundScriptCall begin;
            lux::script::BoundScriptCall tick;
            lux::script::BoundScriptCall end;
        };
        std::vector<ScriptBackendInstance> instances(options.size);
        std::vector<Calls> calls(options.size);
        rows.push_back(measureRow("micro-lifecycle-create-initialize", backend_name, options.size, 0U, [&] {
            for (std::size_t index{}; index < options.size; ++index)
            {
                const ScriptInstanceCreateContext context{
                    assetId(),
                    entity_scope ? ScriptInstanceScope{EntityScriptScope{ecs::Entity{static_cast<std::uint32_t>(index + 1U)}}}
                                 : ScriptInstanceScope{SimulationScriptScope{}},
                    nullptr
                };
                if (descriptor.createInstance(descriptor.context, context, artifact, instances[index]) !=
                    EScriptBackendResult::SUCCESS)
                {
                    throw std::runtime_error("lifecycle benchmark instance creation failed");
                }
                const auto& exports = artifact.description().exports;
                if (descriptor.prepareMethod(
                        descriptor.context, instances[index], exports[begin_index], calls[index].begin) !=
                        EScriptBackendResult::SUCCESS ||
                    descriptor.prepareMethod(
                        descriptor.context, instances[index], exports[tick_index], calls[index].tick) !=
                        EScriptBackendResult::SUCCESS ||
                    descriptor.prepareMethod(
                        descriptor.context, instances[index], exports[end_index], calls[index].end) !=
                        EScriptBackendResult::SUCCESS)
                {
                    throw std::runtime_error("lifecycle benchmark method preparation failed");
                }
            }
            return Row{.active_instances = options.size};
        }));
        rows.push_back(measureRow("micro-lifecycle-begin", backend_name, options.size, 0U, [&] {
            std::size_t successful{};
            for (auto& value : calls)
            {
                lux_script_call_frame frame{
                    nullptr, 0U, 0U, nullptr, 0U, 0U, nullptr, value.begin.context};
                if (value.begin.invoke(&frame) != 0)
                    throw std::runtime_error(backend_name + " lifecycle benchmark BeginPlay failed");
                ++successful;
            }
            return Row{.active_instances = options.size, .calls = successful, .lifecycle_begins = successful,
                       .checksum = successful};
        }));
        rows.push_back(measureRow("micro-lifecycle-steady-call", backend_name, options.size, 0U, [&] {
            std::size_t successful{};
            for (auto& value : calls)
            {
                lux_script_call_frame frame{
                    nullptr, 0U, 0U, nullptr, 0U, 0U, nullptr, value.tick.context};
                if (value.tick.invoke(&frame) != 0)
                    throw std::runtime_error(backend_name + " lifecycle benchmark steady call failed");
                ++successful;
            }
            return Row{.active_instances = options.size, .calls = successful, .checksum = successful};
        }));
        rows.push_back(measureRow("micro-lifecycle-end-destroy", backend_name, options.size, 0U, [&] {
            const EScriptEndPlayReason reason{EScriptEndPlayReason::RUNTIME_STOPPED};
            lux_script_value_slot reason_slot{
                LUX_SCRIPT_VK_UINT32,
                {},
                sizeof(reason),
                lux::semantic::typeId("lux.simulation.ScriptEndPlayReason"),
                const_cast<EScriptEndPlayReason*>(std::addressof(reason))
            };
            std::size_t successful{};
            for (std::size_t index{}; index < calls.size(); ++index)
            {
                auto& value = calls[index];
                lux_script_call_frame frame{
                    &reason_slot, 1U, 0U, nullptr, 0U, 0U, nullptr, value.end.context};
                if (value.end.invoke(&frame) != 0)
                    throw std::runtime_error(backend_name + " lifecycle benchmark EndPlay failed");
                descriptor.releaseMethod(descriptor.context, instances[index], value.end);
                descriptor.releaseMethod(descriptor.context, instances[index], value.tick);
                descriptor.releaseMethod(descriptor.context, instances[index], value.begin);
                descriptor.destroyInstance(descriptor.context, instances[index]);
                ++successful;
            }
            return Row{.calls = successful, .lifecycle_ends = successful, .checksum = successful};
        }));
    }

    struct CppLifecycleObject final
    {
        std::uint64_t value{};
    };

    struct CppLifecycleFixture final
    {
        CppLifecycleFixture(std::size_t capacity)
        {
            reflected_class.name = "CppLifecycleObject";
            reflected_class.full_name = "lux.benchmark.CppLifecycleObject";
            reflected_class.type = lux::meta::ref_type_of_v<CppLifecycleObject>;
            reflected_class.construct = [](void* memory) { std::construct_at(static_cast<CppLifecycleObject*>(memory)); };
            reflected_class.destruct = [](void* object) { std::destroy_at(static_cast<CppLifecycleObject*>(object)); };

            initializeMethod(begin, "admit", [](void* object, void**, void*) {
                static_cast<CppLifecycleObject*>(object)->value = 10U;
            });
            initializeMethod(tick, "update", [](void* object, void**, void*) {
                ++static_cast<CppLifecycleObject*>(object)->value;
            });
            initializeMethod(end, "retire", [](void* object, void** arguments, void*) {
                const auto reason = *static_cast<const EScriptEndPlayReason*>(arguments[0]);
                if (reason != EScriptEndPlayReason::RUNTIME_STOPPED ||
                    static_cast<CppLifecycleObject*>(object)->value != 11U)
                {
                    std::terminate();
                }
            });
            begin.owner_class = std::addressof(reflected_class);
            tick.owner_class = std::addressof(reflected_class);
            end.owner_class = std::addressof(reflected_class);
            end.invokable.parameters.push_back({
                "reason",
                lux::meta::ref_type_of_v<EScriptEndPlayReason>,
                lux::cxx::type_name<EScriptEndPlayReason>(),
                lux::cxx::type_hash<EScriptEndPlayReason>()
            });
            const std::array methods{std::addressof(begin), std::addressof(tick), std::addressof(end)};
            const std::array symbols{kBegin, kTick, kEnd};
            auto projected = projectCppStaticEntityScript(
                "lux.benchmark.cpp-lifecycle",
                "cpp-lifecycle-v1",
                reflected_class,
                methods,
                symbols,
                {nullptr, &resolveSemantic},
                nullptr,
                {kBegin, kEnd}
            );
            if (!projected)
                throw std::runtime_error("C++ lifecycle descriptor projection failed");
            script.emplace(std::move(*projected));
            auto created_artifact = lux::script::ScriptArtifact::create(script->description(), {});
            if (!created_artifact)
                throw std::runtime_error("C++ lifecycle artifact creation failed");
            artifact.emplace(std::move(*created_artifact));
            const std::array pools{CppStaticScriptPoolDescription{std::addressof(*script), capacity}};
            auto created_backend = CppStaticScriptBackend::create(pools);
            if (!created_backend)
                throw std::runtime_error("C++ lifecycle backend creation failed");
            backend.emplace(std::move(*created_backend));
        }

        static void initializeMethod(
            lux::meta::RefMethod& method,
            std::string_view name,
            lux::meta::MethodInvoker invoker
        )
        {
            method.invokable.name = name;
            method.invokable.full_name = name;
            method.invokable.return_type = lux::meta::ref_type_of_v<void>;
            method.invokable.invoker = invoker;
            method.is_noexcept = true;
        }

        static bool resolveSemantic(void*, const lux::meta::RefType& type, lux::semantic::Layout& output) noexcept
        {
            if (type.hash != lux::cxx::type_hash<EScriptEndPlayReason>())
                return false;
            using Traits = lux::semantic::TypeTraits<EScriptEndPlayReason>;
            output = {
                lux::semantic::typeId(Traits::CanonicalName),
                Traits::CanonicalName,
                Traits::AbiKind,
                Traits::Size,
                Traits::Alignment
            };
            return true;
        }

        lux::meta::RefClass reflected_class;
        lux::meta::RefMethod begin;
        lux::meta::RefMethod tick;
        lux::meta::RefMethod end;
        std::optional<CppStaticScriptDescriptor> script;
        std::optional<lux::script::ScriptArtifact> artifact;
        std::optional<CppStaticScriptBackend> backend;
    };

#if LUX_BENCHMARK_HAS_LUA
    struct LuaLifecycleFixture final
    {
        explicit LuaLifecycleFixture(std::size_t capacity)
        {
            lux::rdesc::Script description;
            description.module_name = "lux.benchmark.lua-lifecycle";
            description.body = lux::rdesc::LuaSourceScript{"benchmark"};
            description.exports = {
                {"admit", kBegin, {}, {}},
                {"update", kTick, {}, {}},
                {"retire", kEnd, {lux::rdesc::makeScriptValueType<EScriptEndPlayReason>()}, {}}
            };
            description.lifecycle = {kBegin, kEnd};
            constexpr std::string_view source = R"lua(
                return {
                    admit = function(self) self.value = 10 end,
                    update = function(self) self.value = self.value + 1 end,
                    retire = function(self, reason)
                        if self.value ~= 11 or reason ~= 2 then error("lifecycle mismatch") end
                    end
                }
            )lua";
            std::vector<std::byte> payload;
            payload.reserve(source.size());
            for (const auto value : source)
                payload.push_back(static_cast<std::byte>(value));
            auto created_artifact = lux::script::ScriptArtifact::create(std::move(description), std::move(payload));
            if (!created_artifact)
                throw std::runtime_error("Lua lifecycle artifact creation failed");
            artifact.emplace(std::move(*created_artifact));
            auto created_backend = LuaScriptBackend::create(capacity, capacity * 3U);
            if (!created_backend)
                throw std::runtime_error("Lua lifecycle backend creation failed");
            backend.emplace(std::move(*created_backend));
        }

        std::optional<lux::script::ScriptArtifact> artifact;
        std::optional<LuaScriptBackend> backend;
    };
#endif

#if LUX_BENCHMARK_HAS_NATIVE
    struct NativeLifecycleFixture final
    {
        explicit NativeLifecycleFixture(std::size_t capacity)
        {
            auto loaded = lux::script::loadNativeModule(std::filesystem::path{LUX_BENCHMARK_NATIVE_FIXTURE});
            if (!loaded)
                throw std::runtime_error("Native lifecycle module load failed");
            module.emplace(std::move(*loaded));
            lux::rdesc::Script description;
            description.module_name = "native_fixture";
            description.body = lux::rdesc::NativeModuleScript{
                LUX_SCRIPT_ABI_VERSION,
                module->stateLayoutHash(),
                module->stateSize(),
                module->stateAlignment(),
                {}
            };
            description.exports = {
                {"Increment", 1U, {}, {}},
                {"OnUpdate", 2U, {lux::rdesc::makeScriptValueType<float>()}, {}},
                {"OnUpdate", 3U,
                    {lux::rdesc::makeScriptValueType<float>(), lux::rdesc::makeScriptValueType<std::uint32_t>()}, {}},
                {"AdmitToGameplay", 4U, {}, {}},
                {"LeaveGameplay", 5U, {lux::rdesc::makeScriptValueType<EScriptEndPlayReason>()}, {}}
            };
            description.lifecycle = {4U, 5U};
            auto created_artifact = lux::script::ScriptArtifact::create(std::move(description), {});
            if (!created_artifact)
                throw std::runtime_error("Native lifecycle artifact creation failed");
            artifact.emplace(std::move(*created_artifact));
            backend = std::make_unique<NativeScriptBackend>(
                NativeModuleResolver{this, &resolve},
                1U,
                capacity
            );
            if (!*backend)
                throw std::runtime_error("Native lifecycle backend creation failed");
        }

        static bool resolve(
            void* opaque,
            const lux::asset::AssetId&,
            const lux::script::ScriptArtifact&,
            ResolvedNativeModule& output
        ) noexcept
        {
            auto& self = *static_cast<NativeLifecycleFixture*>(opaque);
            output.module = std::addressof(*self.module);
            return true;
        }

        std::optional<lux::script::NativeModule> module;
        std::optional<lux::script::ScriptArtifact> artifact;
        std::unique_ptr<NativeScriptBackend> backend;
    };
#endif

    template <class Operation>
    Row measureRow(
        std::string scenario,
        std::string backend,
        std::size_t size,
        std::size_t sample,
        Operation&& operation
    )
    {
        g_allocation_count.store(0U, std::memory_order_relaxed);
        g_count_allocations.store(true, std::memory_order_release);
        const auto begin = Clock::now();
        Row result = operation();
        const auto end = Clock::now();
        g_count_allocations.store(false, std::memory_order_release);
        result.scenario = std::move(scenario);
        result.backend = std::move(backend);
        result.size = size;
        result.sample = sample;
        result.nanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
        result.allocations = g_allocation_count.load(std::memory_order_relaxed);
        return result;
    }

    void appendRuntimeStats(Row& row, RuntimeHarness& harness)
    {
        const auto stats = harness.system->stats();
        row.active_instances = stats.active_instances;
        row.continuations = stats.active_continuations;
        row.awaitables = stats.active_awaitables;
        row.queue_depth = stats.resume_queue_depth;
        row.queue_high_water = stats.resume_queue_high_water;
        row.calls = harness.backend_state.calls;
        row.ability_calls = harness.value_provider.calls;
        row.suspensions = harness.backend_state.suspensions;
        row.resumes = harness.backend_state.resumes;
        row.lifecycle_begins = harness.backend_state.begins;
        row.lifecycle_ends = harness.backend_state.ends;
        row.checksum = harness.backend_state.checksum + harness.value_provider.checksum;
    }

    void runMicroSync(const Options& options, std::vector<Row>& rows)
    {
        ValueProvider provider;
        const auto binding = lux::script::bindScriptAbility<benchmark::ValueAbility>(provider);
        auto api_result = lux::script::ScriptAbilityCpp<benchmark::ValueAbility>::create(binding);
        if (!api_result)
            throw std::runtime_error("benchmark Ability facade rejected");
        auto api = std::move(*api_result);
        std::uint64_t direct_checksum{};
        std::uint64_t bound_checksum{};
        const lux::script::BoundScriptCall bound{
            [](lux_script_call_frame* frame) noexcept {
                ++*static_cast<std::uint64_t*>(frame->user_context);
                return 0;
            },
            &bound_checksum
        };
        HookPoint<void()> hook;
        if (hook.prepare(1U) != EEndpointMutationError::NONE ||
            !hook.connect(&direct_checksum, [](void* value) noexcept { ++*static_cast<std::uint64_t*>(value); }))
        {
            throw std::runtime_error("benchmark direct HookPoint rejected");
        }
#if LUX_BENCHMARK_HAS_LUA
        LuaLifecycleFixture lua{1U};
        auto lua_descriptor = lua.backend->descriptor();
        ScriptBackendInstance lua_instance;
        if (lua_descriptor.createInstance(
                lua_descriptor.context,
                ScriptInstanceCreateContext{assetId(), EntityScriptScope{ecs::Entity{1U}}, nullptr},
                *lua.artifact,
                lua_instance
            ) != EScriptBackendResult::SUCCESS)
        {
            throw std::runtime_error("Lua micro instance creation failed");
        }
        lux::script::BoundScriptCall lua_tick;
        lux::script::BoundScriptCall lua_begin;
        if (lua_descriptor.prepareMethod(
                lua_descriptor.context,
                lua_instance,
                lua.artifact->description().exports[0],
                lua_begin
            ) != EScriptBackendResult::SUCCESS ||
            lua_descriptor.prepareMethod(
                lua_descriptor.context,
                lua_instance,
                lua.artifact->description().exports[1],
                lua_tick
            ) != EScriptBackendResult::SUCCESS)
        {
            throw std::runtime_error("Lua micro call preparation failed");
        }
        lux_script_call_frame lua_begin_frame{
            nullptr, 0U, 0U, nullptr, 0U, 0U, nullptr, lua_begin.context};
        if (lua_begin.invoke(&lua_begin_frame) != 0)
            throw std::runtime_error("Lua micro BeginPlay failed");
        std::uint64_t lua_checksum{};
#endif
        for (std::size_t sample{}; sample < 30U; ++sample)
        {
            rows.push_back(measureRow("micro-sync", "direct-cpp", options.size, sample, [&] {
                for (std::size_t index{}; index < options.size; ++index)
                    direct_checksum += static_cast<std::uint32_t>(provider.read(static_cast<std::int32_t>(index)));
                return Row{.calls = options.size, .checksum = direct_checksum};
            }));
            rows.push_back(measureRow("micro-sync", "ability-dynamic-thunk", options.size, sample, [&] {
                for (std::size_t index{}; index < options.size; ++index)
                    direct_checksum += static_cast<std::uint32_t>(api.read(static_cast<std::int32_t>(index)));
                return Row{.calls = options.size, .ability_calls = options.size, .checksum = direct_checksum};
            }));
            rows.push_back(measureRow("micro-sync", "bound-script-call", options.size, sample, [&] {
                lux_script_call_frame frame{nullptr, 0U, 0U, nullptr, 0U, 0U, nullptr, bound.context};
                for (std::size_t index{}; index < options.size; ++index)
                    static_cast<void>(bound.invoke(&frame));
                return Row{.calls = options.size, .checksum = bound_checksum};
            }));
            rows.push_back(measureRow("micro-sync", "hook-point", options.size, sample, [&] {
                for (std::size_t index{}; index < options.size; ++index)
                    static_cast<void>(hook.dispatch());
                return Row{.calls = options.size, .checksum = direct_checksum};
            }));
#if LUX_BENCHMARK_HAS_LUA
            rows.push_back(measureRow("micro-sync", "lua-prepared-call", options.size, sample, [&] {
                lux_script_call_frame frame{
                    nullptr, 0U, 0U, nullptr, 0U, 0U, nullptr, lua_tick.context};
                for (std::size_t index{}; index < options.size; ++index)
                {
                    if (lua_tick.invoke(&frame) != 0)
                        throw std::runtime_error("Lua micro invocation failed");
                    ++lua_checksum;
                }
                return Row{.calls = options.size, .checksum = lua_checksum};
            }));
#endif
        }
#if LUX_BENCHMARK_HAS_LUA
        lua_descriptor.releaseMethod(lua_descriptor.context, lua_instance, lua_tick);
        lua_descriptor.releaseMethod(lua_descriptor.context, lua_instance, lua_begin);
        lua_descriptor.destroyInstance(lua_descriptor.context, lua_instance);
#endif
        if (bound_checksum != options.size * 30U || provider.calls != options.size * 60U)
            throw std::runtime_error("micro-sync benchmark observation mismatch");
    }

    void runObjectFrames(
        const Options& options,
        std::vector<Row>& rows,
        EScenarioMode mode,
        std::string_view scenario
    )
    {
        RuntimeHarness harness{options.size, mode, options.resume_budget};
        for (std::size_t frame{}; frame < options.warmups; ++frame)
        {
            static_cast<void>(harness.hook.dispatch());
            if (mode == EScenarioMode::MIXED)
                harness.completePending(harness.backend_state.completions.size());
            harness.advance(std::chrono::milliseconds{16});
            harness.stablePoint();
        }
        rows.reserve(rows.size() + options.frames);
        for (std::size_t frame{}; frame < options.frames; ++frame)
        {
            rows.push_back(measureRow(std::string{scenario}, "synthetic-object", options.size, frame, [&] {
                static_cast<void>(harness.hook.dispatch());
                if (mode == EScenarioMode::MIXED)
                    harness.completePending(harness.backend_state.completions.size());
                harness.advance(std::chrono::milliseconds{16});
                harness.stablePoint();
                Row row;
                appendRuntimeStats(row, harness);
                return row;
            }));
        }
        const auto expected_frames = options.warmups + options.frames;
        const bool invalid_sync_result = mode == EScenarioMode::SYNC &&
            harness.backend_state.calls != options.size * expected_frames;
        const bool invalid_common_result = harness.system->activeInstanceCount() != options.size ||
            harness.backend_state.begins != options.size || harness.backend_state.checksum == 0U;
        if (invalid_sync_result || invalid_common_result)
            throw std::runtime_error("object-frame benchmark observation mismatch");
    }

    void runAsyncPhases(const Options& options, std::vector<Row>& rows)
    {
        RuntimeHarness harness{options.size, EScenarioMode::EXTERNAL_AWAIT, options.resume_budget};
        rows.push_back(measureRow("micro-async-suspend", "synthetic-continuation", options.size, 0U, [&] {
            static_cast<void>(harness.hook.dispatch());
            Row row;
            appendRuntimeStats(row, harness);
            return row;
        }));
        auto pending = std::move(harness.backend_state.completions);
        const auto split = pending.size() / 2U;
        rows.push_back(measureRow("micro-async-complete", "awaitable-ingress", split, 0U, [&] {
            for (std::size_t index{}; index < split; ++index)
            {
                if (!pending[index].ready())
                    throw std::runtime_error("same-thread benchmark completion failed");
            }
            Row row;
            appendRuntimeStats(row, harness);
            return row;
        }));
        rows.push_back(measureRow("micro-async-cross-thread-complete", "awaitable-ingress", pending.size() - split,
                                  0U, [&] {
            std::atomic_bool failed{};
            std::jthread worker([&] {
                for (std::size_t index{split}; index < pending.size(); ++index)
                {
                    if (!pending[index].ready())
                        failed.store(true, std::memory_order_relaxed);
                }
            });
            worker.join();
            if (failed.load(std::memory_order_relaxed) || harness.backend_state.resumes != 0U)
                throw std::runtime_error("cross-thread benchmark completion violated ingress contract");
            Row row;
            appendRuntimeStats(row, harness);
            return row;
        }));
        rows.push_back(measureRow("micro-async-resume", "stable-point", options.size, 0U, [&] {
            harness.stablePoint();
            Row row;
            appendRuntimeStats(row, harness);
            return row;
        }));

        RuntimeHarness eager{options.size, EScenarioMode::EAGER_AWAIT, options.resume_budget};
        rows.push_back(measureRow("micro-async-eager-complete", "stable-point-tail-queue", options.size, 0U, [&] {
            static_cast<void>(eager.hook.dispatch());
            if (eager.backend_state.resumes != 0U)
                throw std::runtime_error("eager benchmark completion resumed recursively");
            eager.stablePoint();
            Row row;
            appendRuntimeStats(row, eager);
            return row;
        }));
    }

    void runLifecycle(const Options& options, std::vector<Row>& rows)
    {
        {
            RuntimeHarness harness{options.size, EScenarioMode::SYNC, options.resume_budget, false, false};
            rows.push_back(measureRow("micro-lifecycle-admit", "script-system", options.size, 0U, [&] {
                if (!harness.system->prepare())
                    throw std::runtime_error("benchmark lifecycle prepare failed");
                Row row;
                appendRuntimeStats(row, harness);
                return row;
            }));
            rows.push_back(measureRow("micro-lifecycle-retire", "script-system", options.size, 0U, [&] {
                if (!harness.system->shutdown())
                    throw std::runtime_error("benchmark lifecycle shutdown failed");
                Row row;
                appendRuntimeStats(row, harness);
                return row;
            }));
        }

        CppLifecycleFixture cpp{options.size};
        runBackendLifecycle(
            options,
            rows,
            "cpp-static",
            cpp.backend->descriptor(),
            *cpp.artifact,
            true
        );
#if LUX_BENCHMARK_HAS_LUA
        LuaLifecycleFixture lua{options.size};
        runBackendLifecycle(
            options,
            rows,
            "lua",
            lua.backend->descriptor(),
            *lua.artifact,
            true
        );
#endif
#if LUX_BENCHMARK_HAS_NATIVE
        NativeLifecycleFixture native{options.size};
        runBackendLifecycle(
            options,
            rows,
            "native",
            native.backend->descriptor(),
            *native.artifact,
            false,
            3U,
            0U,
            4U
        );
#endif
    }

    void runSuspendedIdle(const Options& options, std::vector<Row>& rows)
    {
        RuntimeHarness harness{options.size, EScenarioMode::EXTERNAL_AWAIT, options.resume_budget};
        static_cast<void>(harness.hook.dispatch());
        for (std::size_t frame{}; frame < options.warmups; ++frame)
            harness.stablePoint();
        for (std::size_t frame{}; frame < options.frames; ++frame)
        {
            rows.push_back(measureRow("scene-suspended-idle", "synthetic-continuation", options.size, frame, [&] {
                harness.stablePoint();
                Row row;
                appendRuntimeStats(row, harness);
                return row;
            }));
        }
        const auto stats = harness.system->stats();
        if (stats.active_continuations != options.size || stats.active_awaitables != options.size ||
            harness.backend_state.resumes != 0U)
        {
            throw std::runtime_error("suspended-idle benchmark observation mismatch");
        }
    }

    void runResumeStorm(const Options& options, std::vector<Row>& rows)
    {
        RuntimeHarness harness{options.size, EScenarioMode::EXTERNAL_AWAIT, options.resume_budget};
        static_cast<void>(harness.hook.dispatch());
        harness.completePending(options.ready_count);
        std::size_t frame{};
        while (harness.system->stats().resume_queue_depth != 0U)
        {
            rows.push_back(measureRow("scene-resume-storm", "synthetic-continuation", options.size, frame++, [&] {
                harness.stablePoint();
                Row row;
                appendRuntimeStats(row, harness);
                return row;
            }));
        }
        const auto expected_frames = (options.ready_count + options.resume_budget - 1U) / options.resume_budget;
        if (harness.backend_state.resumes != options.ready_count || frame != expected_frames)
            throw std::runtime_error("resume-storm benchmark observation mismatch");
    }

    void runChurn(const Options& options, std::vector<Row>& rows)
    {
        RuntimeHarness harness{options.size, EScenarioMode::SYNC, options.resume_budget, true};
        const auto churn_count = (std::max)(std::size_t{1U}, (std::min)(std::size_t{100U}, options.size / 10U));
        for (std::size_t frame{}; frame < options.warmups; ++frame)
            harness.churn(frame * churn_count, churn_count);
        for (std::size_t frame{}; frame < options.frames; ++frame)
        {
            rows.push_back(measureRow("scene-object-churn", "synthetic-object", options.size, frame, [&] {
                harness.churn((frame + options.warmups) * churn_count, churn_count);
                Row row;
                appendRuntimeStats(row, harness);
                return row;
            }));
        }
        const auto churn_total = churn_count * (options.warmups + options.frames);
        if (harness.backend_state.creates != options.size + churn_total ||
            harness.backend_state.destroys != churn_total || harness.backend_state.begins != options.size + churn_total ||
            harness.backend_state.ends != churn_total || harness.system->activeInstanceCount() != options.size)
        {
            throw std::runtime_error("object-churn benchmark observation mismatch");
        }
    }

    void runScheduler(const Options& options, std::vector<Row>& rows, EScenarioMode mode)
    {
        const bool next_step = mode == EScenarioMode::NEXT_STEP;
        RuntimeHarness harness{options.size, mode, options.resume_budget};
        static_cast<void>(harness.hook.dispatch());
        rows.push_back(measureRow(
            next_step ? "scheduler-next-step-idle" : "scheduler-simulation-delay-idle",
            "bounded-heap",
            options.size,
            0U,
            [&] {
                if (!next_step)
                    harness.advance(SimulationDuration{});
                harness.stablePoint();
                Row row;
                appendRuntimeStats(row, harness);
                return row;
            }
        ));
        const auto idle_stats = harness.system->stats();
        const auto idle_waits = next_step ? idle_stats.next_step_waits : idle_stats.simulation_delay_waits;
        if (idle_waits != options.size || harness.backend_state.resumes != 0U)
            throw std::runtime_error("delay scheduler idle observation mismatch");
        rows.push_back(measureRow(
            next_step ? "scheduler-next-step-expire" : "scheduler-simulation-delay-expire",
            "bounded-heap",
            options.size,
            0U,
            [&] {
                harness.advance(next_step ? SimulationDuration{} : std::chrono::seconds{1});
                harness.stablePoint();
                Row row;
                appendRuntimeStats(row, harness);
                return row;
            }
        ));
        const auto expected_resumes = (std::min)(options.size, options.resume_budget);
        if (harness.backend_state.resumes != expected_resumes)
            throw std::runtime_error("delay scheduler expiry observation mismatch");
    }
} // namespace

int main(int argc, char** argv)
{
    const auto options = parseOptions(argc, argv);
    if (!options)
        return 2;
    try
    {
        std::vector<Row> rows;
        if (options->group == "micro-sync")
            runMicroSync(*options, rows);
        else if (options->group == "micro-async")
            runAsyncPhases(*options, rows);
        else if (options->group == "micro-lifecycle")
            runLifecycle(*options, rows);
        else if (options->group == "scene-update-heavy")
            runObjectFrames(*options, rows, EScenarioMode::SYNC, options->group);
        else if (options->group == "scene-gameplay-mixed")
            runObjectFrames(*options, rows, EScenarioMode::MIXED, options->group);
        else if (options->group == "scene-suspended-idle")
            runSuspendedIdle(*options, rows);
        else if (options->group == "scene-resume-storm")
            runResumeStorm(*options, rows);
        else if (options->group == "scene-object-churn")
            runChurn(*options, rows);
        else if (options->group == "scheduler-next-step")
            runScheduler(*options, rows, EScenarioMode::NEXT_STEP);
        else
            runScheduler(*options, rows, EScenarioMode::SIMULATION_DELAY);
        writeCsv(*options, rows);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "script runtime benchmark failed: %s\n", error.what());
        return 3;
    }
}

void* operator new(std::size_t size)
{
    if (g_count_allocations.load(std::memory_order_relaxed))
        g_allocation_count.fetch_add(1U, std::memory_order_relaxed);
    if (void* result = std::malloc(size == 0U ? 1U : size))
        return result;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* value) noexcept
{
    std::free(value);
}

void operator delete[](void* value) noexcept
{
    ::operator delete(value);
}

void operator delete(void* value, std::size_t) noexcept
{
    std::free(value);
}

void operator delete[](void* value, std::size_t) noexcept
{
    ::operator delete(value);
}
