#include "CppBenchmarkScripts.hpp"
#include "CppBenchmarkScripts.CppLifecycle.script.generated.hpp"
#include "CppBenchmarkScripts.CppCoroutineBenchmark.script.generated.hpp"
#include "../../../system/test/HookInvocationTestAccess.hpp"
using lux::simulation::test::dispatchHookForTest;
#include "../../../scripting/core/test/ScriptEndpointTestAccess.hpp"
using lux::simulation::script::test::deliverEndpoint;
#include "ScriptBenchmarkAbility.hpp"
#include "ScriptBenchmarkAbility.ability.generated.hpp"
#if LUX_BENCHMARK_HAS_LUA
#include "ScriptBenchmarkAbility.ability.lua.generated.hpp"
#endif

#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/abilities/DelayAbility.hpp>
#include "DelayAbility.ability.generated.hpp"
#if LUX_BENCHMARK_HAS_LUA
#include "DelayAbility.ability.lua.generated.hpp"
#endif
#include <lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp>
#include <lux/engine/simulation/scripting/ScriptEventSource.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>
#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>
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
        REAL_DELAY,
        EVENT_WAIT,
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
        std::filesystem::path lua_artifact;
        bool vm_accounting{};
#if LUX_BENCHMARK_HAS_LUA
        lux::script::lua::ELuaExecutionPolicy lua_policy{
            lux::script::lua::ELuaExecutionPolicy::DEFAULT
        };
#endif
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
        bool warmups_supplied{};
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
            else if (key == "--warmups")
            {
                if (!parseSize(value, result.warmups))
                    return std::nullopt;
                warmups_supplied = true;
            }
            else if (key == "--vm-accounting")
            {
                if (value != "on" && value != "off")
                    return std::nullopt;
                result.vm_accounting = value == "on";
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
            else if (key == "--lua-artifact")
                result.lua_artifact = value;
#if LUX_BENCHMARK_HAS_LUA
            else if (key == "--lua-policy")
            {
                if (value == "default")
                    result.lua_policy = lux::script::lua::ELuaExecutionPolicy::DEFAULT;
                else if (value == "interpreter-only")
                    result.lua_policy = lux::script::lua::ELuaExecutionPolicy::INTERPRETER_ONLY;
                else
                    return std::nullopt;
            }
#endif
            else
                return std::nullopt;
        }
        if (result.mode == "performance")
        {
            if (!warmups_supplied)
                result.warmups = 300U;
            if (!frames_supplied)
                result.frames = 5000U;
        }
        else if (result.mode != "diagnostic")
            return std::nullopt;

        constexpr std::array groups{
            std::string_view{"micro-sync"},
            std::string_view{"micro-hook-channel"},
            std::string_view{"micro-async"},
            std::string_view{"micro-lifecycle"},
            std::string_view{"scene-update-heavy"},
            std::string_view{"scene-gameplay-mixed"},
            std::string_view{"scene-suspended-idle"},
            std::string_view{"scene-resume-storm"},
            std::string_view{"scene-object-churn"},
            std::string_view{"scheduler-next-step"},
            std::string_view{"scheduler-simulation-delay"},
            std::string_view{"integration-real-delay"},
            std::string_view{"micro-event-wait"},
            std::string_view{"scene-event-idle"},
            std::string_view{"scene-event-fanout"},
            std::string_view{"scene-event-sparse"},
            std::string_view{"micro-cpp-coroutine"}
#if LUX_BENCHMARK_HAS_LUA
            , std::string_view{"micro-lua-sync"}
            , std::string_view{"micro-lua-ability-query"}
            , std::string_view{"micro-lua-coroutine"}
            , std::string_view{"micro-lua-event"}
            , std::string_view{"scene-lua-update-heavy"}
            , std::string_view{"scene-lua-ability"}
            , std::string_view{"scene-lua-coroutine"}
            , std::string_view{"scene-lua-event"}
            , std::string_view{"scene-lua-suspended-idle"}
            , std::string_view{"scene-lua-resume-storm"}
            , std::string_view{"scene-lua-object-churn"}
#endif
        };
        if (std::find(groups.begin(), groups.end(), result.group) == groups.end())
            return std::nullopt;
#if LUX_BENCHMARK_HAS_LUA
        if (result.group.find("lua") != std::string::npos && result.lua_artifact.empty())
            return std::nullopt;
#endif
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
        std::size_t events{};
        std::size_t suspensions{};
        std::size_t resumes{};
        std::size_t continuations{};
        std::size_t awaitables{};
        std::size_t event_waiters{};
        std::size_t event_dispatch_visits{};
        std::size_t payload_bytes{};
        std::size_t queue_depth{};
        std::size_t queue_high_water{};
        std::size_t external_queue_depth{};
        std::size_t external_queue_high_water{};
        std::size_t external_queue_capacity_failures{};
        std::size_t lifecycle_begins{};
        std::size_t lifecycle_ends{};
        std::uint64_t checksum{};
        std::uint64_t vm_allocations{};
        std::uint64_t vm_reallocations{};
        std::uint64_t vm_frees{};
        std::uint64_t vm_requested_bytes{};
        std::uint64_t vm_released_bytes{};
        std::uint64_t vm_coroutine_creations{};
        std::uint64_t vm_coroutine_resumes{};
        std::uint64_t vm_coroutine_releases{};
    };

#if LUX_BENCHMARK_HAS_LUA
    lux::script::lua::LuaRuntimeInfo g_lua_runtime_info;
#endif

    void writeCsv(const Options& options, std::span<const Row> rows)
    {
        if (!options.output.parent_path().empty())
            std::filesystem::create_directories(options.output.parent_path());
        auto temporary = options.output;
        temporary += ".tmp";
        std::ofstream output(temporary, std::ios::trunc);
        if (!output)
            throw std::runtime_error("cannot open benchmark output");
        output << "benchmark_schema_version,git_commit,build_type,compiler,os,logical_cpu_count,"
                  "lua_vm,lua_version,jit_available,jit_enabled,scenario,backend,"
                  "size,seed,sample,nanoseconds,"
                  "allocations,active_instances,calls,ability_calls,events,suspensions,resumes,continuations,"
                  "awaitables,event_waiters,event_dispatch_visits,payload_bytes,queue_depth,queue_high_water,"
                  "external_queue_depth,external_queue_high_water,external_queue_capacity_failures,"
                  "lifecycle_begins,lifecycle_ends,checksum,vm_accounting,vm_allocations,vm_reallocations,vm_frees,"
                  "vm_requested_bytes,vm_released_bytes,vm_coroutine_creations,"
                  "vm_coroutine_resumes,vm_coroutine_releases\n";
        for (const auto& row : rows)
        {
            output << "5," << LUX_BENCHMARK_GIT_COMMIT << ',' << LUX_BENCHMARK_BUILD_TYPE << ','
                   << LUX_BENCHMARK_COMPILER << ",windows," << std::thread::hardware_concurrency() << ','
#if LUX_BENCHMARK_HAS_LUA
                   << g_lua_runtime_info.vm << ',' << g_lua_runtime_info.version << ','
                   << (g_lua_runtime_info.jit_available ? 1 : 0) << ','
                   << (g_lua_runtime_info.jit_enabled ? 1 : 0) << ','
#else
                   << ",,0,0,"
#endif
                   << row.scenario << ',' << row.backend << ',' << row.size << ',' << options.seed << ','
                   << row.sample << ','
                   << row.nanoseconds << ',' << row.allocations << ',' << row.active_instances << ',' << row.calls
                   << ',' << row.ability_calls << ',' << row.events << ',' << row.suspensions << ',' << row.resumes
                   << ','
                   << row.continuations << ',' << row.awaitables << ',' << row.event_waiters << ','
                   << row.event_dispatch_visits << ',' << row.payload_bytes << ',' << row.queue_depth << ','
                   << row.queue_high_water << ',' << row.external_queue_depth << ','
                   << row.external_queue_high_water << ',' << row.external_queue_capacity_failures << ','
                   << row.lifecycle_begins << ',' << row.lifecycle_ends << ',' << row.checksum << ','
                   << options.vm_accounting << ',' << row.vm_allocations << ',' << row.vm_reallocations << ','
                   << row.vm_frees << ',' << row.vm_requested_bytes << ',' << row.vm_released_bytes << ','
                   << row.vm_coroutine_creations << ',' << row.vm_coroutine_resumes << ','
                   << row.vm_coroutine_releases << '\n';
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
    inline constexpr lux::script::ScriptSymbolId kLuaAsync{0xB006U};
    inline constexpr lux::script::ScriptSymbolId kLuaPlain{0xB007U};
    inline constexpr lux::script::ScriptSymbolId kLuaQuery{0xB008U};
    inline constexpr lux::script::ScriptSymbolId kLuaEventWait{0xB009U};
    inline constexpr EventPointId kEvent{0xB009U};
    inline constexpr EventPointId kTargetEvent{0xB00AU};

    [[nodiscard]] SimulationDescription scriptDescription()
    {
        constexpr std::array hooks{makeHookPointSpec<void()>(kHook, "benchmark-update")};
        constexpr std::array events{
            makeEventPointSpec<std::int32_t>(
                kEvent,
                "benchmark-event",
                kHook,
                EEventRoute::SIMULATION_BROADCAST,
                "lux.i32",
                1U
            ),
            makeEventPointSpec<std::int32_t>(
                kTargetEvent,
                "benchmark-target-event",
                kHook,
                EEventRoute::ENTITY_TARGETED,
                "lux.i32",
                1U
            )
        };
        const SimulationSystemDescription system{
            .type = {.canonical_name = "lux.benchmark.ScriptRuntime", .version = 1U},
            .hooks = hooks,
            .events = events
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
        bool entity_scope{};
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
        std::size_t real_delay_starts{};
        std::uint64_t checksum{};
        std::vector<ScriptAwaitableCompletion> completions;
        std::vector<lux::script::ScriptAbilityCompletion<void>> real_delay_completions;
    };

    lux::script::ScriptAbilityStartResult startFakeRealDelay(
        void* opaque,
        std::chrono::nanoseconds,
        lux::script::ScriptAbilityCompletion<void> completion
    ) noexcept
    {
        auto& state = *static_cast<BackendState*>(opaque);
        try
        {
            ++state.real_delay_starts;
            state.real_delay_completions.push_back(std::move(completion));
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{5});
        }
    }

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
        auto* object = new (std::nothrow) RuntimeObject{
            &state,
            state.creates++,
            0U,
            std::holds_alternative<EntityScriptScope>(context.scope)
        };
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

    ScriptStepResult invokeStep(
        void* opaque,
        lux_script_call_frame&,
        ScriptStepContext& step,
        ScriptBackendContinuation& output
    ) noexcept;

    EScriptBackendResult prepareMethod(
        void* opaque,
        ScriptBackendInstance instance,
        const lux::rdesc::ScriptFunction& function,
        ScriptBackendPreparedMethod& output
    ) noexcept
    {
        auto* prepared = new (std::nothrow) PreparedCall{
            static_cast<RuntimeObject*>(instance.value), function.symbol_id};
        if (!prepared)
            return EScriptBackendResult::ALLOCATION_FAILURE;
        ++static_cast<BackendState*>(opaque)->prepares;
        const auto mode = static_cast<BackendState*>(opaque)->mode;
        output = {
            prepared,
            lux::script::BoundScriptCall{&invokePrepared, prepared},
            function.symbol_id == kTick && mode != EScenarioMode::SYNC
                ? BoundScriptStepCall{instance.value, &invokeStep}
                : BoundScriptStepCall{}
        };
        return EScriptBackendResult::SUCCESS;
    }

    void releaseMethod(void* opaque, ScriptBackendInstance, ScriptBackendPreparedMethod method) noexcept
    {
        ++static_cast<BackendState*>(opaque)->releases;
        delete static_cast<PreparedCall*>(method.token);
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
        if (packet.value != nullptr && packet.value->type.valid() &&
            packet.value->type.type_id == lux::semantic::typeId("lux.i32") &&
            packet.value->bytes.size() == sizeof(std::int32_t))
        {
            std::int32_t payload{};
            std::memcpy(std::addressof(payload), packet.value->bytes.data(), sizeof(payload));
            object.value += static_cast<std::uint32_t>(payload);
        }
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
        else if (state.mode == EScenarioMode::REAL_DELAY)
        {
            if (!object.delay)
                return ScriptStepResult::failed(7);
            result = invokeScriptAbilityAsync<void>(
                step,
                [&object](lux::script::ScriptAbilityCompletion<void> completion) noexcept {
                    return object.delay->realSeconds(1.0, std::move(completion));
                }
            );
            suspend = result.state == EScriptStepState::SUSPENDED;
        }
        else if (state.mode == EScenarioMode::EVENT_WAIT)
        {
            const auto route = object.entity_scope
                ? EEventRoute::ENTITY_TARGETED
                : EEventRoute::SIMULATION_BROADCAST;
            const auto waiting = step.event_waits.wait({
                kSystem,
                route == EEventRoute::SIMULATION_BROADCAST ? kEvent : kTargetEvent,
                route
            });
            if (!waiting)
                return ScriptStepResult::failed(8);
            result = ScriptStepResult::suspended(*waiting);
            suspend = true;
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
                mode == EScenarioMode::REAL_DELAY ||
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
            if (mode == EScenarioMode::EVENT_WAIT)
            {
                description.event_requirements.push_back({
                    "Benchmark",
                    entity_scope ? "target_event" : "event",
                    kSystem.value,
                    entity_scope ? kTargetEvent.value : kEvent.value,
                    entity_scope ? lux::script::EScriptEventRoute::ENTITY_TARGETED
                                 : lux::script::EScriptEventRoute::SIMULATION_BROADCAST,
                    {"lux.i32", lux::semantic::typeId("lux.i32"), LUX_SCRIPT_VK_INT32, 4U, 4U},
                    lux::semantic::typeId("lux.i32"),
                    1U, kHook.value, simulation_description.findHookPoint(kSystem, kHook).contractHash(), 1U
                });
            }
            description.body = lux::rdesc::CppStaticScript{"benchmark-synthetic"};
            auto created_artifact = lux::script::ScriptArtifact::create(std::move(description), {});
            if (!created_artifact)
                throw std::runtime_error("benchmark ScriptArtifact rejected");
            artifact.emplace(std::move(*created_artifact));

            if (hook.prepare(1U) != EEndpointMutationError::NONE)
                throw std::runtime_error("benchmark HookPoint prepare failed");
            if (event.prepare({1U, (std::max)(count, std::size_t{1U})}) != EEndpointMutationError::NONE)
                throw std::runtime_error("benchmark EventPoint prepare failed");
            if (target_event.prepare({1U, (std::max)(count, std::size_t{1U})}) != EEndpointMutationError::NONE)
                throw std::runtime_error("benchmark targeted EventPoint prepare failed");
            hook_bridge = std::make_unique<ScriptHookEndpoint<void()>>(kSystem, kHook, hook);
            hook_descriptor = hook_bridge->descriptor();
            event_bridge = std::make_unique<ScriptEventEndpoint<SimulationBroadcastRoute, std::int32_t>>(
                kSystem,
                kEvent,
                event
            );
            event_descriptor = event_bridge->descriptor();
            target_event_bridge = std::make_unique<
                ScriptEventEndpoint<EntityTargetedRoute<ecs::Entity>, std::int32_t>>(
                    kSystem,
                    kTargetEvent,
                    target_event
                );
            event_descriptors = {event_descriptor, target_event_bridge->descriptor()};
            backend_state.completions.reserve(count);
            backend_state.real_delay_completions.reserve(count);
            backend = {
                lux::rdesc::Script::Kind::CPP_STATIC,
                &backend_state,
                &createInstance,
                &prepareMethod,
                &releaseMethod,
                &destroyInstance
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
                    bounded_count,
                    bounded_count,
                    bounded_count
                },
                {this, &resolveArtifact},
                entity_scope ? WorldObjectResolver{this, &resolveWorld} : WorldObjectResolver{},
                capability_span,
                std::span{&backend, 1U},
                std::span{&hook_descriptor, 1U},
                event_descriptors,
                {},
                mode == EScenarioMode::REAL_DELAY
                    ? ScriptRealDelayEndpoint{&backend_state, &startFakeRealDelay}
                    : ScriptRealDelayEndpoint{}
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
            const auto bytes = object.value.as_bytes();
            if (std::to_integer<std::uint8_t>(bytes.front()) != 0xB1U)
                return false;
            std::uint64_t encoded_index{};
            for (std::size_t byte{}; byte < sizeof(std::uint64_t); ++byte)
            {
                encoded_index |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[8U + byte])) <<
                    (byte * 8U);
            }
            if (encoded_index > (std::numeric_limits<std::size_t>::max)())
                return false;
            const auto index = static_cast<std::size_t>(encoded_index);
            if (index >= self.entities.size())
                return false;
            output = self.entities[index];
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

        void deliverEvent(std::int32_t payload, std::optional<std::size_t> target = std::nullopt)
        {
            if (!target)
            {
                {
                    auto writer = event.begin(0U);
                    if (!writer.record(payload))
                        throw std::runtime_error("benchmark Event record failed");
                }
                if (deliverEndpoint(event_bridge) == 0U)
                    throw std::runtime_error("benchmark Event delivery failed");
                return;
            }
            if (*target >= entities.size())
                throw std::runtime_error("benchmark Event target is invalid");
            {
                auto writer = target_event.begin(0U);
                if (!writer.record(entities[*target], payload))
                    throw std::runtime_error("benchmark targeted Event record failed");
            }
            if (deliverEndpoint(target_event_bridge) == 0U)
                throw std::runtime_error("benchmark targeted Event delivery failed");
        }

        void deliverTargetedBatch(std::size_t count, std::int32_t payload)
        {
            if (count > entities.size())
                throw std::runtime_error("benchmark targeted Event batch is invalid");
            auto writer = target_event.begin(0U);
            for (std::size_t index{}; index < count; ++index)
            {
                if (!writer.record(entities[index], payload))
                    throw std::runtime_error("benchmark targeted Event batch record failed");
            }
            writer = {};
            if (deliverEndpoint(target_event_bridge) != count)
                throw std::runtime_error("benchmark targeted Event batch delivery failed");
        }

        void rematerialize(std::size_t first, std::size_t count)
        {
            if (!entity_scope || entities.empty())
                throw std::runtime_error("benchmark churn requires entity scope");
            for (std::size_t offset{}; offset < count; ++offset)
            {
                const auto index = (first + offset) % entities.size();
                registry.destroy(entities[index]);
                entities[index] = registry.create();
            }
        }

        SimulationDescription simulation_description;
        ecs::Registry registry;
        SimulationSystemRegistry empty_system_types;
        std::optional<Simulation> clock_owner;
        std::optional<lux::task::TaskExecutor> executor;
        std::optional<ScriptSystemDescription> system_description;
        std::optional<lux::script::ScriptArtifact> artifact;
        HookPoint<void()> hook;
        HookChannel<SimulationBroadcastRoute, std::int32_t> event;
        HookChannel<EntityTargetedRoute<ecs::Entity>, std::int32_t> target_event;
        std::unique_ptr<ScriptHookEndpoint<void()>> hook_bridge;
        std::unique_ptr<ScriptEventEndpoint<SimulationBroadcastRoute, std::int32_t>> event_bridge;
        std::unique_ptr<ScriptEventEndpoint<EntityTargetedRoute<ecs::Entity>, std::int32_t>> target_event_bridge;
        ScriptHookEndpointDescriptor hook_descriptor;
        ScriptEventEndpointDescriptor event_descriptor;
        std::array<ScriptEventEndpointDescriptor, 2U> event_descriptors;
        BackendState backend_state;
        ScriptBackendDescriptor backend;
        ValueProvider value_provider;
        lux::script::ScriptAbilityBinding value_binding;
        std::optional<ScriptSystem> system;
        std::vector<lux::world::WorldObjectId> objects;
        std::vector<ecs::Entity> entities;
        bool entity_scope{};
    };

#if LUX_BENCHMARK_HAS_LUA
    [[nodiscard]] std::shared_ptr<const lux::script::ScriptArtifactAsset> loadLuaArtifact(
        const std::filesystem::path& path
    )
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot open packaged Lua benchmark artifact");
        input.seekg(0, std::ios::end);
        const auto size = input.tellg();
        if (size <= 0)
            throw std::runtime_error("packaged Lua benchmark artifact is empty");
        input.seekg(0, std::ios::beg);
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (!input || bytes.size() < 56U)
            throw std::runtime_error("cannot read packaged Lua benchmark artifact");
        std::array<std::uint8_t, 16U> id_bytes{};
        for (std::size_t index{}; index < id_bytes.size(); ++index)
            id_bytes[index] = std::to_integer<std::uint8_t>(bytes[40U + index]);
        const lux::asset::AssetId requested{id_bytes};
        auto decoded = lux::asset::TAssetSerDeser<lux::script::ScriptArtifactAsset>::decode(
            requested,
            lux::cxx::SharedBytes<>::copyOf(bytes),
            {bytes.size(), (std::numeric_limits<std::size_t>::max)(), 0U}
        );
        if (!decoded || (*decoded)->data().description().kind() != lux::rdesc::Script::Kind::LUA_SOURCE)
            throw std::runtime_error("packaged Lua benchmark artifact decode failed");
        return std::move(*decoded);
    }

    struct LuaRuntimeHarness final
    {
        LuaRuntimeHarness(
            const std::filesystem::path& artifact_path,
            std::size_t count,
            lux::script::ScriptSymbolId symbol,
            std::size_t resume_budget,
            lux::script::lua::ELuaExecutionPolicy execution_policy, bool vm_accounting = false
        )
            : simulation_description(scriptDescription()), artifact_asset(loadLuaArtifact(artifact_path))
        {
            auto clock_simulation = Simulation::create(registry, emptyDescription(), empty_system_types);
            if (!clock_simulation)
                throw std::runtime_error("Lua benchmark clock simulation create failed");
            clock_owner.emplace(std::move(*clock_simulation));
            auto created_executor = lux::task::TaskExecutor::create({0U, 1U});
            if (!created_executor)
                throw std::runtime_error("Lua benchmark clock executor create failed");
            executor.emplace(std::move(*created_executor));

            objects.reserve(count);
            entities.reserve(count);
            ScriptSystemDescriptionBuilder description_builder;
            for (std::size_t index{}; index < count; ++index)
            {
                const auto object = objectId(index);
                const auto entity = registry.create();
                objects.push_back(object);
                entities.push_back(entity);
                if (!description_builder.addMount({
                        ScriptMountId{index + 1U},
                        artifact_asset->id(),
                        EntityScriptMount{object},
                        true,
                        {{symbol, HookScriptTarget{kSystem, kHook}}}
                    }))
                {
                    throw std::runtime_error("Lua benchmark mount rejected");
                }
            }
            auto built = std::move(description_builder).build(simulation_description);
            if (!built)
                throw std::runtime_error("Lua benchmark ScriptSystem description rejected");
            system_description.emplace(std::move(*built));

            const std::size_t bounded_count = (std::max)(count, std::size_t{1U});
            if (hook.prepare(1U) != EEndpointMutationError::NONE)
                throw std::runtime_error("Lua benchmark HookPoint prepare failed");
            if (event.prepare({1U, bounded_count}) != EEndpointMutationError::NONE)
                throw std::runtime_error("Lua benchmark EventPoint prepare failed");
            hook_bridge = std::make_unique<ScriptHookEndpoint<void()>>(kSystem, kHook, hook);
            hook_descriptor = hook_bridge->descriptor();
            event_bridge = std::make_unique<ScriptEventEndpoint<SimulationBroadcastRoute, std::int32_t>>(
                kSystem,
                kEvent,
                event
            );
            event_descriptor = event_bridge->descriptor();
            auto projected_event = projectScriptEventSource(
                simulation_description.findEvent(kSystem, kEvent),
                event_descriptor,
                "Benchmark",
                "event"
            );
            if (!projected_event)
                throw std::runtime_error("Lua benchmark Event source projection failed");
            event_source = std::move(*projected_event);

            contributions = {
                lux::script::lua::makeScriptAbilityLuaContribution<benchmark::ValueAbility>(),
                lux::script::lua::makeScriptAbilityLuaContribution<DelayAbility>()
            };
            if (bounded_count > (std::numeric_limits<std::size_t>::max)() / 4U)
                throw std::runtime_error("Lua benchmark capacity overflow");
            const auto requirements =
                describeLuaPreparedRequirements(artifact_asset->data().description(), contributions);
            if (!requirements) throw std::runtime_error("Lua benchmark requirements are incompatible");
            auto created_backend = LuaScriptBackend::create({
                .instance_capacity = bounded_count,
                .prepared_call_capacity = bounded_count * 4U,
                .continuation_capacity = bounded_count,
                .execution_depth_capacity = 16U,
                .ability_catalog_method_capacity = 6U,
                .prepared_ability_capacity = bounded_count * requirements->ability_methods,
                .abilities = contributions,
                .execution_policy = execution_policy,
                .event_catalog_capacity = 1U,
                .prepared_event_capacity = bounded_count * requirements->event_sources,
                .events = std::span{&event_source, 1U},
                .track_vm_allocations = vm_accounting,
                .prepared_ability_blocks = std::array{
                    lux::simulation::script::LuaPreparedBlockClass{
                        requirements->ability_methods,
                        bounded_count
                    }
                },
                .prepared_ability_storage_bytes =
                    64U * 1024U * 1024U,
                .prepared_event_blocks = std::array{
                    lux::simulation::script::LuaPreparedBlockClass{
                        requirements->event_sources,
                        bounded_count
                    }
                },
                .prepared_event_storage_bytes =
                    64U * 1024U * 1024U
            });
            if (!created_backend)
                throw std::runtime_error("Lua benchmark backend creation failed");
            backend.emplace(std::move(*created_backend));
            g_lua_runtime_info = backend->runtimeInfo();
            backend_descriptor = backend->descriptor();

            value_binding = lux::script::bindScriptAbility<benchmark::ValueAbility>(value_provider);
            const std::array publications{publishScriptAbility(value_binding)};
            auto created = ScriptSystem::create(
                simulation_description,
                *system_description,
                registry,
                clock_owner->clock(),
                {
                    bounded_count,
                    bounded_count,
                    bounded_count,
                    1U,
                    bounded_count,
                    bounded_count,
                    64U,
                    (std::max)(resume_budget, std::size_t{1U}),
                    bounded_count,
                    bounded_count,
                    bounded_count,
                    bounded_count
                },
                {this, &resolveArtifact},
                {this, &resolveWorld},
                publications,
                std::span{&backend_descriptor, 1U},
                std::span{&hook_descriptor, 1U},
                std::span{&event_descriptor, 1U}
            );
            if (!created)
                throw std::runtime_error("Lua benchmark ScriptSystem creation failed");
            system.emplace(std::move(*created));
            if (!system->prepare())
                throw std::runtime_error("Lua benchmark ScriptSystem prepare failed");
            lifecycle_begins = count;
        }

        ~LuaRuntimeHarness()
        {
            if (system)
                static_cast<void>(system->shutdown());
        }

        static bool resolveArtifact(
            void* opaque,
            const lux::asset::AssetId& requested,
            ResolvedScriptArtifact& output
        ) noexcept
        {
            auto& self = *static_cast<LuaRuntimeHarness*>(opaque);
            if (requested != self.artifact_asset->id())
                return false;
            output.artifact = std::addressof(self.artifact_asset->data());
            return true;
        }

        static bool resolveWorld(
            void* opaque,
            const lux::world::WorldObjectId& object,
            ecs::Entity& output
        ) noexcept
        {
            auto& self = *static_cast<LuaRuntimeHarness*>(opaque);
            const auto bytes = object.value.as_bytes();
            if (std::to_integer<std::uint8_t>(bytes.front()) != 0xB1U)
                return false;
            std::uint64_t encoded_index{};
            for (std::size_t byte{}; byte < sizeof(std::uint64_t); ++byte)
            {
                encoded_index |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[8U + byte])) <<
                    (byte * 8U);
            }
            if (encoded_index >= self.entities.size())
                return false;
            const auto index = static_cast<std::size_t>(encoded_index);
            output = self.entities[index];
            return self.registry.valid(output);
        }

        void dispatch()
        {
            static_cast<void>(dispatchHookForTest(hook));
            ++dispatches;
        }

        void advance(SimulationDuration delta)
        {
            if (!clock_owner->execute(*executor, delta))
                throw std::runtime_error("Lua benchmark clock advance failed");
        }

        void stablePoint()
        {
            if (!system->executeStablePoint())
                throw std::runtime_error("Lua benchmark stable point failed");
        }

        void deliverEvent(std::int32_t payload)
        {
            {
                auto writer = event.begin(0U);
                if (!writer.record(payload))
                    throw std::runtime_error("Lua benchmark Event record failed");
            }
            if (deliverEndpoint(event_bridge) == 0U)
                throw std::runtime_error("Lua benchmark Event delivery failed");
        }

        void rematerialize(std::size_t first, std::size_t count)
        {
            for (std::size_t offset{}; offset < count; ++offset)
            {
                const auto index = (first + offset) % entities.size();
                registry.destroy(entities[index]);
                entities[index] = registry.create();
            }
            lifecycle_begins += count;
            lifecycle_ends += count;
        }

        SimulationDescription simulation_description;
        std::shared_ptr<const lux::script::ScriptArtifactAsset> artifact_asset;
        ecs::Registry registry;
        SimulationSystemRegistry empty_system_types;
        std::optional<Simulation> clock_owner;
        std::optional<lux::task::TaskExecutor> executor;
        std::optional<ScriptSystemDescription> system_description;
        HookPoint<void()> hook;
        HookChannel<SimulationBroadcastRoute, std::int32_t> event;
        std::unique_ptr<ScriptHookEndpoint<void()>> hook_bridge;
        std::unique_ptr<ScriptEventEndpoint<SimulationBroadcastRoute, std::int32_t>> event_bridge;
        ScriptHookEndpointDescriptor hook_descriptor;
        ScriptEventEndpointDescriptor event_descriptor;
        lux::script::ScriptEventSourceDescription event_source;
        std::array<lux::script::lua::ScriptAbilityLuaContribution, 2U> contributions;
        std::optional<LuaScriptBackend> backend;
        ScriptBackendDescriptor backend_descriptor;
        ValueProvider value_provider;
        lux::script::ScriptAbilityBinding value_binding;
        std::optional<ScriptSystem> system;
        std::vector<lux::world::WorldObjectId> objects;
        std::vector<ecs::Entity> entities;
        std::size_t dispatches{};
        std::size_t lifecycle_begins{};
        std::size_t lifecycle_ends{};
    };
#endif

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
            ScriptBackendPreparedMethod begin;
            ScriptBackendPreparedMethod tick;
            ScriptBackendPreparedMethod end;
        };
        std::vector<ScriptBackendInstance> instances(options.size);
        std::vector<Calls> calls(options.size);
        rows.push_back(measureRow("micro-lifecycle-create-initialize", backend_name, options.size, 0U, [&] {
            for (std::size_t index{}; index < options.size; ++index)
            {
                const ScriptInstanceCreateContext context{
                    assetId(),
                    entity_scope
                        ? ScriptInstanceScope{EntityScriptScope{ecs::Entity{static_cast<std::uint32_t>(index + 1U)}}}
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
                    nullptr, 0U, 0U, nullptr, 0U, 0U, nullptr, value.begin.synchronous.context};
                if (value.begin.synchronous.invoke(&frame) != 0)
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
                    nullptr, 0U, 0U, nullptr, 0U, 0U, nullptr, value.tick.synchronous.context};
                if (value.tick.synchronous.invoke(&frame) != 0)
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
                    &reason_slot, 1U, 0U, nullptr, 0U, 0U, nullptr, value.end.synchronous.context};
                if (value.end.synchronous.invoke(&frame) != 0)
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

    struct CppLifecycleFixture final
    {
        CppLifecycleFixture(std::size_t capacity)
        {
            auto description = materializeCppStaticScript(generated::CppLifecycle);
            if (!description) throw std::runtime_error("C++ lifecycle generated contract invalid");
            auto created_artifact = lux::script::ScriptArtifact::create(std::move(*description), {});
            if (!created_artifact)
                throw std::runtime_error("C++ lifecycle artifact creation failed");
            artifact.emplace(std::move(*created_artifact));
            const std::array pools{CppStaticScriptPoolDescription{
                &generated::CppLifecycle, capacity, 0U, 0U, alignof(std::max_align_t), capacity * 3U
            }};
            auto created_backend = CppStaticScriptBackend::create(pools);
            if (!created_backend)
                throw std::runtime_error("C++ lifecycle backend creation failed");
            backend.emplace(std::move(*created_backend));
        }

        std::optional<lux::script::ScriptArtifact> artifact;
        std::optional<CppStaticScriptBackend> backend;
    };

#if LUX_BENCHMARK_HAS_LUA
    struct LuaLifecycleFixture final
    {
        LuaLifecycleFixture(
            std::size_t capacity,
            lux::script::lua::ELuaExecutionPolicy execution_policy
        )
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
            auto created_backend = LuaScriptBackend::create({
                .instance_capacity = capacity,
                .prepared_call_capacity = capacity * 3U,
                .continuation_capacity = capacity,
                .execution_depth_capacity = 8U,
                .ability_catalog_method_capacity = 1U,
                .execution_policy = execution_policy
            });
            if (!created_backend)
                throw std::runtime_error("Lua lifecycle backend creation failed");
            backend.emplace(std::move(*created_backend));
            g_lua_runtime_info = backend->runtimeInfo();
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
                NativeScriptBackendConfig{
                    .module_capacity = 1U,
                    .instance_capacity = capacity,
                    .prepared_call_capacity = capacity * 5U,
                    .continuation_capacity = capacity,
                    .max_ability_imports_per_module = 8U,
                    .max_continuation_frame_bytes = 4096U,
                    .continuation_frame_storage_bytes =
                        2U * ((std::max)(std::size_t{4096U}, capacity * 256U)) + 4096U,
                    .storage_populations = std::array{
                        lux::simulation::script::NativeScriptStoragePopulation{
                            std::addressof(*module), capacity, capacity
                        }
                    },
                    .state_storage_bytes = 64U * 1024U * 1024U
                }
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
        row.event_waiters = stats.active_event_waiters;
        row.event_dispatch_visits = stats.event_waiter_dispatch_visits;
        row.queue_depth = stats.resume_queue_depth;
        row.queue_high_water = stats.resume_queue_high_water;
        row.external_queue_depth = stats.external_completion_queue_depth;
        row.external_queue_high_water = stats.external_completion_queue_high_water;
        row.external_queue_capacity_failures = stats.external_completion_capacity_failures;
        row.calls = harness.backend_state.calls;
        row.ability_calls = harness.value_provider.calls;
        row.suspensions = harness.backend_state.suspensions;
        row.resumes = harness.backend_state.resumes;
        row.lifecycle_begins = harness.backend_state.begins;
        row.lifecycle_ends = harness.backend_state.ends;
        row.checksum = harness.backend_state.checksum + harness.value_provider.checksum;
    }

#if LUX_BENCHMARK_HAS_LUA
    void appendLuaStats(Row& row, LuaRuntimeHarness& harness)
    {
        const auto vm = harness.backend->stats();
        row.vm_allocations = vm.vm_allocations.allocations;
        row.vm_reallocations = vm.vm_allocations.reallocations;
        row.vm_frees = vm.vm_allocations.frees;
        row.vm_requested_bytes = vm.vm_allocations.requested_bytes;
        row.vm_released_bytes = vm.vm_allocations.released_bytes;
        row.vm_coroutine_creations = vm.vm_coroutine_creations;
        row.vm_coroutine_resumes = vm.vm_coroutine_resumes;
        row.vm_coroutine_releases = vm.vm_coroutine_releases;
        const auto stats = harness.system->stats();
        row.active_instances = stats.active_instances;
        row.continuations = stats.active_continuations;
        row.awaitables = stats.active_awaitables;
        row.event_waiters = stats.active_event_waiters;
        row.event_dispatch_visits = stats.event_waiter_dispatch_visits;
        row.queue_depth = stats.resume_queue_depth;
        row.queue_high_water = stats.resume_queue_high_water;
        row.external_queue_depth = stats.external_completion_queue_depth;
        row.external_queue_high_water = stats.external_completion_queue_high_water;
        row.external_queue_capacity_failures = stats.external_completion_capacity_failures;
        row.calls = stats.sync_invocations + stats.step_invocations;
        row.ability_calls = harness.value_provider.calls;
        row.suspensions = stats.suspensions_admitted;
        row.resumes = stats.backend_resume_calls;
        row.lifecycle_begins = harness.lifecycle_begins;
        row.lifecycle_ends = harness.lifecycle_ends;
        row.checksum = harness.dispatches + harness.value_provider.checksum + harness.lifecycle_begins;
    }
#endif

    using lux::simulation::benchmark::cpp_coroutine_checksum;

    struct CppCoroutineBenchmarkHarness final
    {
        explicit CppCoroutineBenchmarkHarness(std::size_t capacity)
        {
            auto description = materializeCppStaticScript(generated::CppCoroutineBenchmark);
            if (!description) throw std::runtime_error("C++ coroutine generated contract invalid");
            auto created_artifact = lux::script::ScriptArtifact::create(std::move(*description), {});
            if (!created_artifact)
                throw std::runtime_error("C++ coroutine benchmark artifact failed");
            artifact.emplace(std::move(*created_artifact));
            const auto storage_bytes = (std::max)(std::size_t{1024U}, capacity * 512U);
            // This benchmark has no owned-reference argument block; the plan provisions one frame per flight.
            const std::array pools{CppStaticScriptPoolDescription{
                &generated::CppCoroutineBenchmark,
                1U,
                capacity,
                storage_bytes * 2U + 4096U,
                alignof(std::max_align_t),
                1U,
                512U
            }};
            auto created_backend = CppStaticScriptBackend::create(pools);
            if (!created_backend)
                throw std::runtime_error("C++ coroutine benchmark backend failed");
            backend.emplace(std::move(*created_backend));
            runtime = backend->descriptor();
            if (runtime.createInstance(
                    runtime.context,
                    {{}, EntityScriptScope{ecs::Entity{1U}}, &behavior, {1U, 1U}},
                    *artifact,
                    instance
                ) != EScriptBackendResult::SUCCESS ||
                runtime.prepareMethod(
                    runtime.context,
                    instance,
                    artifact->description().exports.front(),
                    prepared
                ) != EScriptBackendResult::SUCCESS)
            {
                throw std::runtime_error("C++ coroutine benchmark preparation failed");
            }
            call = prepared.resumable;
        }

        ~CppCoroutineBenchmarkHarness()
        {
            runtime.releaseMethod(runtime.context, instance, prepared);
            runtime.destroyInstance(runtime.context, instance);
        }

        static lux::cxx::expected<ScriptAwaitableRegistration, EScriptAwaitableCreateError> createAwaitable(
            void* opaque,
            ScriptInstanceId,
            std::optional<lux::simulation::script::PreparedResumeType>
        ) noexcept
        {
            auto& self = *static_cast<CppCoroutineBenchmarkHarness*>(opaque);
            return ScriptAwaitableRegistration{{self.next_awaitable++, 1U}, {}};
        }

        static void discardAwaitable(void*, ScriptInstanceId, ScriptAwaitableId) noexcept
        {
        }

        std::optional<lux::script::ScriptArtifact> artifact;
        std::optional<CppStaticScriptBackend> backend;
        ScriptBackendDescriptor runtime;
        ScriptBehavior behavior;
        ScriptBackendInstance instance;
        ScriptBackendPreparedMethod prepared;
        BoundScriptStepCall call;
        std::uint32_t next_awaitable{1U};
    };

    void runCppCoroutineMicro(const Options& options, std::vector<Row>& rows)
    {
        CppCoroutineBenchmarkHarness harness{options.size};
        std::vector<ScriptBackendContinuation> continuations(options.size);
        std::vector<ScriptAwaitableId> awaitables(options.size);
        lux_script_call_frame frame{};
        ScriptStepContext step{
            {1U, 1U},
            std::addressof(harness),
            &CppCoroutineBenchmarkHarness::createAwaitable,
            &CppCoroutineBenchmarkHarness::discardAwaitable
        };
        rows.push_back(measureRow("micro-cpp-coroutine-start", "cpp-static", options.size, 0U, [&] {
            for (std::size_t index{}; index < options.size; ++index)
            {
                const auto result = harness.call.invoke(
                    harness.call.context,
                    frame,
                    step,
                    continuations[index]
                );
                if (result.state != EScriptStepState::SUSPENDED || !continuations[index])
                    throw std::runtime_error("C++ coroutine benchmark start failed");
                awaitables[index] = result.waiting_on;
            }
            const auto stats = harness.backend->stats();
            return Row{
                .suspensions = options.size,
                .continuations = stats.active_frames,
                .payload_bytes = stats.frame_storage_bytes,
                .queue_high_water = stats.frame_high_water,
                .checksum = cpp_coroutine_checksum
            };
        }));
        rows.push_back(measureRow("micro-cpp-coroutine-resume", "cpp-static", options.size, 0U, [&] {
            for (std::size_t index{}; index < options.size; ++index)
            {
                const ScriptResumePacket packet{
                    awaitables[index],
                    EScriptAwaitableState::READY,
                    nullptr,
                    {}
                };
                const auto result = continuations[index].resume(continuations[index].state, step, packet);
                if (result.state != EScriptStepState::COMPLETED)
                    throw std::runtime_error("C++ coroutine benchmark resume failed");
                continuations[index].destroy(continuations[index].state);
            }
            const auto stats = harness.backend->stats();
            return Row{
                .resumes = options.size,
                .continuations = stats.active_frames,
                .payload_bytes = stats.frame_storage_bytes,
                .queue_high_water = stats.frame_high_water,
                .checksum = cpp_coroutine_checksum
            };
        }));
        if (cpp_coroutine_checksum < options.size || harness.backend->stats().heap_frame_allocations != 0U)
            throw std::runtime_error("C++ coroutine benchmark observation mismatch");
    }

    void runHookChannelMicro(const Options& options, std::vector<Row>& rows)
    {
        HookChannel<SimulationBroadcastRoute, std::uint32_t> channel;
        const auto per_lane = options.size / 4U + (options.size % 4U != 0U ? 1U : 0U);
        if (channel.prepare({4U, per_lane}) != EEndpointMutationError::NONE)
            throw std::runtime_error("channel benchmark capacity failure");
        std::uint64_t expected{};
        for (std::size_t index{}; index < options.size; ++index)
            expected += static_cast<std::uint32_t>((index ^ options.seed) & 0xffffU);
        for (std::size_t sample{}; sample < options.warmups + options.frames; ++sample)
        {
            auto append = measureRow("channel-append", "typed-channel-primitive", options.size, sample, [&] {
                for (std::size_t lane{}; lane < 4U; ++lane)
                {
                    auto writer = channel.begin(lane);
                    for (std::size_t index = lane; index < options.size; index += 4U)
                        if (!writer.record(static_cast<std::uint32_t>((index ^ options.seed) & 0xffffU)))
                            throw std::runtime_error("channel append failed");
                }
                return Row{.events = options.size, .payload_bytes = options.size * sizeof(std::uint32_t)};
            });
            auto consume = measureRow("channel-seal-scan", "typed-channel-primitive", options.size, sample, [&] {
                if (!channel.seal())
                    throw std::runtime_error("channel seal failed");
                std::uint64_t checksum{};
                for (std::size_t lane{}; lane < channel.laneCount(); ++lane)
                    for (const auto& occurrence : channel.lane(lane))
                        checksum += static_cast<volatile const std::uint32_t&>(occurrence.payload);
                if (checksum != expected)
                    throw std::runtime_error("channel checksum mismatch");
                return Row{.events = options.size, .checksum = checksum};
            });
            auto reset = measureRow("channel-reset", "typed-channel-primitive", options.size, sample, [&] {
                channel.reset();
                return Row{.checksum = channel.pendingOccurrenceCount()};
            });
            if (sample >= options.warmups)
            {
                append.sample -= options.warmups;
                consume.sample -= options.warmups;
                reset.sample -= options.warmups;
                rows.push_back(std::move(append));
                rows.push_back(std::move(consume));
                rows.push_back(std::move(reset));
            }
        }
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
        LuaLifecycleFixture lua{1U, options.lua_policy};
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
        ScriptBackendPreparedMethod lua_tick_method;
        ScriptBackendPreparedMethod lua_begin_method;
        if (lua_descriptor.prepareMethod(
                lua_descriptor.context,
                lua_instance,
                lua.artifact->description().exports[0],
                lua_begin_method
            ) != EScriptBackendResult::SUCCESS ||
            lua_descriptor.prepareMethod(
                lua_descriptor.context,
                lua_instance,
                lua.artifact->description().exports[1],
                lua_tick_method
            ) != EScriptBackendResult::SUCCESS)
        {
            throw std::runtime_error("Lua micro call preparation failed");
        }
        const auto lua_begin = lua_begin_method.synchronous;
        const auto lua_tick = lua_tick_method.synchronous;
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
                    static_cast<void>(dispatchHookForTest(hook));
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
        lua_descriptor.releaseMethod(lua_descriptor.context, lua_instance, lua_tick_method);
        lua_descriptor.releaseMethod(lua_descriptor.context, lua_instance, lua_begin_method);
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
            static_cast<void>(dispatchHookForTest(harness.hook));
            if (mode == EScenarioMode::MIXED)
                harness.completePending(harness.backend_state.completions.size());
            harness.advance(std::chrono::milliseconds{16});
            harness.stablePoint();
        }
        rows.reserve(rows.size() + options.frames);
        for (std::size_t frame{}; frame < options.frames; ++frame)
        {
            rows.push_back(measureRow(std::string{scenario}, "synthetic-object", options.size, frame, [&] {
                static_cast<void>(dispatchHookForTest(harness.hook));
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
            static_cast<void>(dispatchHookForTest(harness.hook));
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
            static_cast<void>(dispatchHookForTest(eager.hook));
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
        LuaLifecycleFixture lua{options.size, options.lua_policy};
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
        static_cast<void>(dispatchHookForTest(harness.hook));
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

    void runEventMicro(const Options& options, std::vector<Row>& rows)
    {
        RuntimeHarness harness{options.size, EScenarioMode::EVENT_WAIT, options.size};
        rows.push_back(measureRow("micro-event-register", "script-event-waiter", options.size, 0U, [&] {
            static_cast<void>(dispatchHookForTest(harness.hook));
            Row row;
            appendRuntimeStats(row, harness);
            return row;
        }));
        rows.push_back(measureRow("micro-event-deliver-copy", "script-event-waiter", options.size, 0U, [&] {
            harness.deliverEvent(17);
            Row row;
            appendRuntimeStats(row, harness);
            row.events = 1U;
            row.payload_bytes = options.size * sizeof(std::int32_t);
            return row;
        }));
        rows.push_back(measureRow("micro-event-resume", "stable-point", options.size, 0U, [&] {
            harness.stablePoint();
            Row row;
            appendRuntimeStats(row, harness);
            return row;
        }));
        if (harness.backend_state.resumes != options.size)
            throw std::runtime_error("Event micro benchmark resume count mismatch");

        RuntimeHarness cancellation{options.size, EScenarioMode::EVENT_WAIT, options.size};
        static_cast<void>(dispatchHookForTest(cancellation.hook));
        rows.push_back(measureRow("micro-event-cancel", "instance-retirement", options.size, 0U, [&] {
            if (!cancellation.system->shutdown())
                throw std::runtime_error("Event cancellation benchmark shutdown failed");
            Row row;
            appendRuntimeStats(row, cancellation);
            return row;
        }));
    }

    void runEventIdle(const Options& options, std::vector<Row>& rows)
    {
        RuntimeHarness harness{options.size, EScenarioMode::EVENT_WAIT, options.resume_budget};
        static_cast<void>(dispatchHookForTest(harness.hook));
        const auto before = harness.system->stats();
        for (std::size_t frame{}; frame < options.warmups; ++frame)
            harness.stablePoint();
        for (std::size_t frame{}; frame < options.frames; ++frame)
        {
            rows.push_back(measureRow("scene-event-idle", "script-event-waiter", options.size, frame, [&] {
                harness.stablePoint();
                Row row;
                appendRuntimeStats(row, harness);
                return row;
            }));
        }
        const auto after = harness.system->stats();
        if (after.active_event_waiters != options.size ||
            after.event_waiter_dispatch_visits != before.event_waiter_dispatch_visits ||
            harness.backend_state.resumes != 0U)
        {
            throw std::runtime_error("Event idle benchmark detected waiter scanning or resume");
        }
    }

    void runEventFanout(const Options& options, std::vector<Row>& rows)
    {
        RuntimeHarness harness{options.size, EScenarioMode::EVENT_WAIT, options.resume_budget};
        static_cast<void>(dispatchHookForTest(harness.hook));
        rows.push_back(measureRow("scene-event-fanout-delivery", "script-event-waiter", options.size, 0U, [&] {
            harness.deliverEvent(23);
            Row row;
            appendRuntimeStats(row, harness);
            row.events = 1U;
            row.payload_bytes = options.size * sizeof(std::int32_t);
            return row;
        }));
        std::size_t frame{};
        while (harness.system->stats().resume_queue_depth != 0U ||
               harness.system->stats().external_completion_queue_depth != 0U)
        {
            rows.push_back(measureRow("scene-event-fanout-resume", "stable-point", options.size, frame++, [&] {
                harness.stablePoint();
                Row row;
                appendRuntimeStats(row, harness);
                return row;
            }));
        }
        const auto expected_frames = (options.size + options.resume_budget - 1U) / options.resume_budget;
        if (frame != expected_frames || harness.backend_state.resumes != options.size)
            throw std::runtime_error("Event fan-out benchmark violated the resume budget");
    }

    void runEventSparse(const Options& options, std::vector<Row>& rows)
    {
        RuntimeHarness harness{options.size, EScenarioMode::EVENT_WAIT, options.resume_budget, true};
        static_cast<void>(dispatchHookForTest(harness.hook));
        rows.push_back(measureRow("scene-event-sparse-delivery", "targeted-event-waiter", options.size, 0U, [&] {
            harness.deliverTargetedBatch(options.ready_count, 29);
            Row row;
            appendRuntimeStats(row, harness);
            row.events = options.ready_count;
            row.payload_bytes = options.ready_count * sizeof(std::int32_t);
            return row;
        }));
        const auto stats = harness.system->stats();
        if (stats.event_waiter_dispatch_visits != options.ready_count ||
            stats.active_event_waiters != options.size - options.ready_count)
        {
            throw std::runtime_error("sparse Event delivery was not output-sensitive");
        }
        while (harness.system->stats().resume_queue_depth != 0U)
            harness.stablePoint();
    }

    void runResumeStorm(const Options& options, std::vector<Row>& rows)
    {
        RuntimeHarness harness{options.size, EScenarioMode::EXTERNAL_AWAIT, options.resume_budget};
        static_cast<void>(dispatchHookForTest(harness.hook));
        harness.completePending(options.ready_count);
        std::size_t frame{};
        while (harness.system->stats().resume_queue_depth != 0U ||
               harness.system->stats().external_completion_queue_depth != 0U)
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
        {
            harness.rematerialize(frame * churn_count, churn_count);
            harness.stablePoint();
        }
        for (std::size_t frame{}; frame < options.frames; ++frame)
        {
            const auto first = (frame + options.warmups) * churn_count;
            rows.push_back(measureRow("scene-object-churn-signal", "synthetic-object", options.size, frame, [&] {
                harness.rematerialize(first, churn_count);
                Row row;
                appendRuntimeStats(row, harness);
                return row;
            }));
            rows.push_back(measureRow("scene-object-churn-stable-point", "synthetic-object", options.size, frame, [&] {
                harness.stablePoint();
                Row row;
                appendRuntimeStats(row, harness);
                return row;
            }));
        }
        const auto churn_total = churn_count * (options.warmups + options.frames);
        if (harness.backend_state.creates != options.size + churn_total ||
            harness.backend_state.destroys != churn_total ||
            harness.backend_state.begins != options.size + churn_total || harness.backend_state.ends != churn_total ||
            harness.system->activeInstanceCount() != options.size)
        {
            throw std::runtime_error("object-churn benchmark observation mismatch");
        }
    }

    void runScheduler(const Options& options, std::vector<Row>& rows, EScenarioMode mode)
    {
        const bool next_step = mode == EScenarioMode::NEXT_STEP;
        RuntimeHarness harness{options.size, mode, options.resume_budget};
        static_cast<void>(dispatchHookForTest(harness.hook));
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

    void runRealDelay(const Options& options, std::vector<Row>& rows)
    {
        RuntimeHarness harness{options.size, EScenarioMode::REAL_DELAY, options.resume_budget};
        rows.push_back(measureRow("integration-real-delay-start", "fake-monotonic-provider", options.size, 0U, [&] {
            static_cast<void>(dispatchHookForTest(harness.hook));
            if (harness.backend_state.real_delay_starts != options.size)
                throw std::runtime_error("real-delay benchmark start count mismatch");
            Row row;
            appendRuntimeStats(row, harness);
            return row;
        }));
        rows.push_back(measureRow(
            "integration-real-delay-complete",
            "cross-thread-ingress",
            options.size,
            0U,
            [&] {
                std::atomic_bool failed{};
                std::jthread worker([&] {
                    for (auto& completion : harness.backend_state.real_delay_completions)
                    {
                        if (!completion.success())
                            failed.store(true, std::memory_order_relaxed);
                    }
                });
                worker.join();
                if (failed.load(std::memory_order_relaxed) || harness.backend_state.resumes != 0U)
                    throw std::runtime_error("real-delay completion bypassed stable point");
                Row row;
                appendRuntimeStats(row, harness);
                return row;
            }
        ));
        rows.push_back(measureRow("integration-real-delay-resume", "stable-point", options.size, 0U, [&] {
            harness.stablePoint();
            if (harness.backend_state.resumes != (std::min)(options.size, options.resume_budget))
                throw std::runtime_error("real-delay benchmark resume budget mismatch");
            Row row;
            appendRuntimeStats(row, harness);
            return row;
        }));
    }

#if LUX_BENCHMARK_HAS_LUA
    void runLuaSync(
        const Options& options,
        std::vector<Row>& rows,
        lux::script::ScriptSymbolId symbol,
        std::string_view scenario
    )
    {
        LuaRuntimeHarness harness{
            options.lua_artifact,
            options.size,
            symbol,
            options.resume_budget,
            options.lua_policy, options.vm_accounting
        };
        for (std::size_t frame{}; frame < options.warmups; ++frame)
        {
            harness.dispatch();
            harness.stablePoint();
        }
        for (std::size_t frame{}; frame < options.frames; ++frame)
        {
            rows.push_back(measureRow(std::string{scenario}, "lua-object", options.size, frame, [&] {
                harness.dispatch();
                harness.stablePoint();
                Row row;
                appendLuaStats(row, harness);
                return row;
            }));
        }
        if (harness.system->activeInstanceCount() != options.size || harness.dispatches == 0U)
            throw std::runtime_error("Lua synchronous benchmark observation mismatch");
        if ((symbol == kTick || symbol == kLuaQuery) && harness.value_provider.calls == 0U)
            throw std::runtime_error("Lua Ability benchmark did not call the prepared provider");
    }

    void runLuaCoroutineFrames(const Options& options, std::vector<Row>& rows)
    {
        LuaRuntimeHarness harness{
            options.lua_artifact,
            options.size,
            kLuaAsync,
            options.resume_budget,
            options.lua_policy, options.vm_accounting
        };
        for (std::size_t frame{}; frame < options.warmups; ++frame)
        {
            harness.dispatch();
            harness.advance(SimulationDuration{1});
            harness.stablePoint();
        }
        for (std::size_t frame{}; frame < options.frames; ++frame)
        {
            rows.push_back(measureRow("scene-lua-coroutine", "lua-coroutine", options.size, frame, [&] {
                harness.dispatch();
                harness.advance(SimulationDuration{1});
                harness.stablePoint();
                Row row;
                appendLuaStats(row, harness);
                return row;
            }));
        }
        if (harness.system->activeInstanceCount() != options.size)
            throw std::runtime_error("Lua coroutine scene benchmark lost instances");
    }

    void runLuaCoroutineMicro(const Options& options, std::vector<Row>& rows)
    {
        LuaRuntimeHarness harness{
            options.lua_artifact,
            options.size,
            kLuaAsync,
            options.resume_budget,
            options.lua_policy, options.vm_accounting
        };
        rows.push_back(measureRow("micro-lua-coroutine-start", "lua-coroutine", options.size, 0U, [&] {
            harness.dispatch();
            Row row;
            appendLuaStats(row, harness);
            return row;
        }));
        if (harness.system->activeContinuationCount() != options.size)
            throw std::runtime_error("Lua coroutine micro did not suspend every invocation");
        std::size_t frame{};
        do
        {
            harness.advance(SimulationDuration{1});
            rows.push_back(measureRow("micro-lua-coroutine-resume", "lua-coroutine", options.size, frame++, [&] {
                harness.stablePoint();
                Row row;
                appendLuaStats(row, harness);
                return row;
            }));
        } while (harness.system->activeContinuationCount() != 0U);
    }

    void runLuaSuspendedIdle(const Options& options, std::vector<Row>& rows)
    {
        LuaRuntimeHarness harness{
            options.lua_artifact,
            options.size,
            kLuaAsync,
            options.resume_budget,
            options.lua_policy, options.vm_accounting
        };
        harness.dispatch();
        for (std::size_t frame{}; frame < options.warmups; ++frame)
            harness.stablePoint();
        for (std::size_t frame{}; frame < options.frames; ++frame)
        {
            rows.push_back(measureRow("scene-lua-suspended-idle", "lua-coroutine", options.size, frame, [&] {
                harness.stablePoint();
                Row row;
                appendLuaStats(row, harness);
                return row;
            }));
        }
        if (harness.system->activeContinuationCount() != options.size)
            throw std::runtime_error("Lua suspended-idle benchmark resumed without an eligible step");
    }

    void runLuaResumeStorm(const Options& options, std::vector<Row>& rows)
    {
        LuaRuntimeHarness harness{
            options.lua_artifact,
            options.size,
            kLuaAsync,
            options.resume_budget,
            options.lua_policy, options.vm_accounting
        };
        harness.dispatch();
        std::size_t frame{};
        do
        {
            harness.advance(SimulationDuration{1});
            rows.push_back(measureRow("scene-lua-resume-storm", "lua-coroutine", options.size, frame++, [&] {
                harness.stablePoint();
                Row row;
                appendLuaStats(row, harness);
                return row;
            }));
        } while (harness.system->activeContinuationCount() != 0U);
        const auto expected_frames = (options.size + options.resume_budget - 1U) / options.resume_budget;
        if (frame != expected_frames)
            throw std::runtime_error("Lua resume storm did not respect the resume budget");
    }

    void runLuaEvent(const Options& options, std::vector<Row>& rows, bool micro)
    {
        LuaRuntimeHarness harness{
            options.lua_artifact,
            options.size,
            kLuaEventWait,
            options.size,
            options.lua_policy, options.vm_accounting
        };
        const auto execute_cycle = [&](std::size_t frame, bool record) {
            const auto operation = [&] {
                harness.dispatch();
                harness.deliverEvent(31);
                harness.advance(SimulationDuration{0});
                harness.stablePoint();
                Row row;
                appendLuaStats(row, harness);
                row.events = 1U;
                row.payload_bytes = options.size * sizeof(std::int32_t);
                return row;
            };
            if (record)
            {
                rows.push_back(measureRow(
                    micro ? "micro-lua-event" : "scene-lua-event",
                    "lua-event-coroutine",
                    options.size,
                    frame,
                    operation
                ));
            }
            else
            {
                static_cast<void>(operation());
            }
        };
        for (std::size_t frame{}; frame < options.warmups; ++frame)
            execute_cycle(frame, false);
        const auto frames = micro ? std::size_t{1U} : options.frames;
        for (std::size_t frame{}; frame < frames; ++frame)
            execute_cycle(frame, true);
        if (harness.system->activeContinuationCount() != 0U || harness.system->stats().active_event_waiters != 0U)
            throw std::runtime_error("Lua Event benchmark left pending runtime state");
    }

    void runLuaChurn(const Options& options, std::vector<Row>& rows)
    {
        LuaRuntimeHarness harness{
            options.lua_artifact,
            options.size,
            kLuaPlain,
            options.resume_budget,
            options.lua_policy, options.vm_accounting
        };
        const auto churn_count = (std::max)(std::size_t{1U}, (std::min)(std::size_t{100U}, options.size / 10U));
        for (std::size_t frame{}; frame < options.warmups; ++frame)
        {
            harness.rematerialize(frame * churn_count, churn_count);
            harness.stablePoint();
        }
        for (std::size_t frame{}; frame < options.frames; ++frame)
        {
            const auto first = (frame + options.warmups) * churn_count;
            rows.push_back(measureRow("scene-lua-object-churn", "lua-object", options.size, frame, [&] {
                harness.rematerialize(first, churn_count);
                harness.stablePoint();
                Row row;
                appendLuaStats(row, harness);
                return row;
            }));
        }
        if (harness.system->activeInstanceCount() != options.size || harness.lifecycle_ends == 0U)
            throw std::runtime_error("Lua churn benchmark observation mismatch");
    }
#endif
} // namespace

int main(int argc, char** argv)
{
    const auto options = parseOptions(argc, argv);
    if (!options)
        return 2;
    try
    {
        std::vector<Row> rows;
        if (options->group == "micro-hook-channel")
            runHookChannelMicro(*options, rows);
        else if (options->group == "micro-sync")
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
        else if (options->group == "scheduler-simulation-delay")
            runScheduler(*options, rows, EScenarioMode::SIMULATION_DELAY);
        else if (options->group == "integration-real-delay")
            runRealDelay(*options, rows);
        else if (options->group == "micro-event-wait")
            runEventMicro(*options, rows);
        else if (options->group == "scene-event-idle")
            runEventIdle(*options, rows);
        else if (options->group == "scene-event-fanout")
            runEventFanout(*options, rows);
        else if (options->group == "scene-event-sparse")
            runEventSparse(*options, rows);
        else if (options->group == "micro-cpp-coroutine")
            runCppCoroutineMicro(*options, rows);
#if LUX_BENCHMARK_HAS_LUA
        else if (options->group == "micro-lua-sync")
            runLuaSync(*options, rows, kLuaPlain, options->group);
        else if (options->group == "micro-lua-ability-query")
            runLuaSync(*options, rows, kLuaQuery, options->group);
        else if (options->group == "micro-lua-coroutine")
            runLuaCoroutineMicro(*options, rows);
        else if (options->group == "micro-lua-event")
            runLuaEvent(*options, rows, true);
        else if (options->group == "scene-lua-update-heavy")
            runLuaSync(*options, rows, kLuaPlain, options->group);
        else if (options->group == "scene-lua-ability")
            runLuaSync(*options, rows, kTick, options->group);
        else if (options->group == "scene-lua-coroutine")
            runLuaCoroutineFrames(*options, rows);
        else if (options->group == "scene-lua-event")
            runLuaEvent(*options, rows, false);
        else if (options->group == "scene-lua-suspended-idle")
            runLuaSuspendedIdle(*options, rows);
        else if (options->group == "scene-lua-resume-storm")
            runLuaResumeStorm(*options, rows);
        else
            runLuaChurn(*options, rows);
#endif
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
