#include "../../../builtin/script/test/ScriptRuntimeTestRegion.hpp"
#include "../../../builtin/script/test/ScriptTestClock.hpp"
#include "../test/TaskDomain.hpp"
#include "ScriptBenchmarkAbility.ability.generated.hpp"
#include "ScriptBenchmarkAbility.ability.lua.generated.hpp"
#include <lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp>
#include "DelayAbility.ability.lua.generated.hpp"
#include "LuaValueTestTypes.lua.value.generated.hpp"
#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#if NA1_CANDIDATE
#include "Tasks.CompleteTasks.script.generated.hpp"
#include "Tasks.hpp"
#include <lux/engine/simulation/scripting/native_lua_tasks/NativeLuaTaskBackend.hpp>
#endif
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace lux::simulation;
using namespace lux::simulation::script;
using namespace lux::simulation::script::test;
using Clock = std::chrono::steady_clock;
using Nanos = std::chrono::nanoseconds;
void require(bool valid, const char *text)
{
    if (!valid)
        throw std::runtime_error(text);
}
struct Options
{
    std::string scenario{"P1"}, output{"cost.csv"};
    std::size_t count{64U}, warmups{2U}, batches{4U};
    bool diagnostics{}, keep_lua_reserve{};
};
struct Provider
{
    std::size_t calls{}, oracle_count{};
    std::uint64_t checksum{};
    std::int32_t value{7};
    std::span<std::int32_t> oracle;
    std::int32_t read(std::int32_t input) noexcept
    {
        ++calls;
        checksum += static_cast<std::uint32_t>(input + value);
        return input + value;
    }
    void write(std::int32_t input) noexcept
    {
        if (!oracle.empty())
        {
            if (oracle_count < oracle.size())
                oracle[oracle_count] = input;
            ++oracle_count;
        }
        ++calls;
        value = input;
        checksum += static_cast<std::uint32_t>(input);
    }
};
auto loadArtifact()
{
    std::ifstream file(NA1_BUSINESS_ARTIFACT, std::ios::binary | std::ios::ate);
    require(static_cast<bool>(file), "artifact open");
    const auto size = file.tellg();
    require(size > 56, "artifact length");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(bytes.data()), size);
    std::array<std::uint8_t, 16U> id{};
    for (std::size_t i{}; i < id.size(); ++i)
        id[i] = std::to_integer<std::uint8_t>(bytes[40U + i]);
    auto artifact = lux::asset::TAssetSerDeser<lux::script::ScriptArtifactAsset>::decode(
        lux::asset::AssetId{id}, lux::cxx::SharedBytes<>::copyOf(bytes),
        {bytes.size(), (std::numeric_limits<std::size_t>::max)(), 0U});
    require(artifact.has_value(), "artifact decode");
    return std::move(*artifact);
}
lux::asset::AssetId nativeId()
{
    std::array<std::uint8_t, 16U> bytes{};
    bytes[0] = 0xA1;
    return lux::asset::AssetId{bytes};
}
struct Harness
{
    explicit Harness(const Options &input)
        : options(input), domain(na1::domain()), artifact(loadArtifact()), clock(registry)
    {
        const auto count = options.count;
        const auto symbol = options.scenario == "P1"   ? 45065U
                            : options.scenario == "P2" ? 45062U
                            : options.scenario == "P3" ? 45066U
                            : options.scenario == "P4" ? 45068U
                                                       : 45069U;
        require(hook.prepare(1U) == EEndpointMutationError::NONE, "hook prepare");
        require(pose_hook.prepare(1U) == EEndpointMutationError::NONE, "pose hook prepare");
        require(read_hook.prepare(1U) == EEndpointMutationError::NONE, "read hook prepare");
        require(event.prepare({1U, count}) == EEndpointMutationError::NONE, "event prepare");
        hook_endpoint.emplace(na1::System, na1::Hook, hook);
        pose_endpoint.emplace(na1::System, na1::PoseHook, pose_hook);
        read_endpoint.emplace(na1::System, na1::NestedHook, read_hook);
        event_endpoint.emplace(na1::System, na1::Event, event);
        auto projected = projectScriptEventSource(domain.findEvent(na1::System, na1::Event),
                                                  event_endpoint->descriptor(), "Benchmark", "event");
        require(projected.has_value(), "event projection");
        source = *projected;
        const std::array contributions{lux::script::lua::makeScriptAbilityLuaContribution<benchmark::ValueAbility>(),
                                       lux::script::lua::makeScriptAbilityLuaContribution<DelayAbility>()};
        const auto requirements = describeLuaPreparedRequirements(artifact->data().description(), contributions);
        require(requirements.has_value(), "Lua import requirements");
        const std::array abilities{LuaPreparedBlockClass{requirements->ability_methods, count}};
        const std::array events{LuaPreparedBlockClass{requirements->event_sources, count}};
        const std::array values{lux::script::lua::makeLuaValueOperation<ValuePose>()};
        LuaScriptBackendConfig lua_config{.instance_capacity = count,
                                          .prepared_call_capacity = count * 10U,
                                          .continuation_capacity =
                                              NA1_CANDIDATE && !options.keep_lua_reserve ? 0U : count,
                                          .execution_depth_capacity = 16U,
                                          .ability_catalog_method_capacity = 6U,
                                          .prepared_ability_capacity = count * requirements->ability_methods,
                                          .values = values,
                                          .abilities = contributions,
                                          .event_catalog_capacity = 1U,
                                          .prepared_event_capacity = count * requirements->event_sources,
                                          .events = std::span{&source, 1U},
                                          .track_vm_allocations = options.diagnostics,
                                          .prepared_ability_blocks = abilities,
                                          .prepared_ability_storage_bytes = 64U * 1024U * 1024U,
                                          .prepared_event_blocks = events,
                                          .prepared_event_storage_bytes = 64U * 1024U * 1024U};
#if NA1_CANDIDATE
        const auto &contract = generated::CompleteTasks;
        auto description = materializeCppStaticScript(contract);
        require(description.has_value(), "native contract");
        auto made = lux::script::ScriptArtifact::create(std::move(*description), {});
        require(made.has_value(), "native artifact");
        native_artifact.emplace(std::move(*made));
        auto typed = CppScriptEventSource<std::int32_t>::create(contract, source);
        require(typed.has_value(), "native Event mapping");
        na1::cost::source = *typed;
        const auto native_symbol = options.scenario == "P1"   ? 201U
                                   : options.scenario == "P2" ? 202U
                                   : options.scenario == "P3" ? 203U
                                   : options.scenario == "P4" ? 204U
                                                              : 205U;
        const std::array routes{NativeLuaTaskRoute{symbol, native_symbol}};
        std::array<lux::rdesc::ScriptFunction, 6U> steps;
        for (std::size_t i{}; i < steps.size(); ++i)
        {
            auto *function =
                artifact->data().findExport(lux::script::ScriptSymbolId{45070U + static_cast<std::uint32_t>(i)});
            require(function != nullptr, "step absent");
            steps[i] = *function;
        }
        const std::array pools{CppStaticScriptPoolDescription{&contract, count, count, count * 1024U + 4096U,
                                                              alignof(std::max_align_t), count, 512U,
                                                              options.diagnostics}};
        const std::array plans{NativeLuaTaskPlan{artifact->id(), artifact->data().contentIdentity(), nativeId(),
                                                 native_artifact->contentIdentity(), &contract, routes, steps}};
        auto made_backend = NativeLuaTaskBackend::create({.lua = lua_config,
                                                          .native_pools = pools,
                                                          .plans = plans,
                                                          .artifacts = {this, &resolve},
                                                          .instance_capacity = count,
                                                          .prepared_method_capacity = count * 4U});
#else
        auto made_backend = LuaScriptBackend::create(lua_config);
#endif
        require(made_backend.has_value(), "backend creation");
        backend.emplace(std::move(*made_backend));
        const auto descriptor = backend->descriptor();
        std::vector<ScriptRuntimeMount> mounts;
        for (std::size_t i{}; i < count; ++i)
        {
            const auto entity = registry.create();
            mounts.push_back(
                {ScriptMountId{i + 1U},
                 artifact->id(),
                 EntityScriptScope{entity},
                 {{symbol, HookScriptTarget{na1::System, options.scenario == "P5" ? na1::PoseHook : na1::Hook}},
                  {45067U, HookScriptTarget{na1::System, na1::NestedHook}}}});
        }
        const auto capacity = planScriptRuntimeCapacity(mounts);
        require(capacity.has_value(), "runtime capacity");
        const auto binding = lux::script::bindScriptAbility<benchmark::ValueAbility>(provider);
        const std::array capabilities{publishScriptAbility(binding)};
        const std::array hooks{hook_endpoint->descriptor(), pose_endpoint->descriptor(), read_endpoint->descriptor()};
        const auto event_desc = event_endpoint->descriptor();
        auto made_system = ScriptSystem::create(
            domain, *capacity, mounts, registry, clock.clock(),
            {count, count, count, 1U, count, count, 64U, count, count, count, count, count}, {this, &resolve},
            capabilities, std::span{&descriptor, 1U}, hooks, std::span{&event_desc, 1U});
        require(made_system.has_value(), "runtime create");
        system.emplace(std::move(*made_system));
        require(system->prepare().has_value(), "runtime prepare");
    }
    static bool resolve(void *opaque, const lux::asset::AssetId &id, ResolvedScriptArtifact &output) noexcept
    {
        auto &self = *static_cast<Harness *>(opaque);
        if (id == self.artifact->id())
            output.artifact = &self.artifact->data();
#if NA1_CANDIDATE
        else if (id == nativeId())
            output.artifact = &*self.native_artifact;
#endif
        else
            return false;
        ++self.leases;
        output.lease = &self;
        output.release = [](void *opaque) noexcept { --static_cast<Harness *>(opaque)->leases; };
        return true;
    }
    void stable(SimulationDuration duration)
    {
        clock.advance(duration);
        require(executeRuntimeStablePoint(*system).has_value(), "stable point");
    }
    void deliver()
    {
        {
            auto writer = event.begin(0U);
            require(static_cast<bool>(writer.record(std::int32_t{31})), "record Event");
        }
        require(deliverRuntimeEvent(*system, event_endpoint) == 1U, "deliver Event");
    }
    void wave(bool observe = false)
    {
        const ValuePose input{7, {2.0f, 3.0}, ValueMode::RUN};
        const auto calls = options.scenario == "P5" ? dispatchRuntimeHook(*system, pose_hook, input)
                                                    : dispatchRuntimeHook(*system, hook);
        require(calls == 1U, "dispatch");
        if (observe)
            memory("waiting");
        if (options.scenario == "P2")
            stable(SimulationDuration{0});
        else if (options.scenario == "P3")
        {
            stable(SimulationDuration{0});
            deliver();
            stable(SimulationDuration{0});
            stable(SimulationDuration{1'000'000});
        }
        else
        {
            const std::size_t rounds = options.scenario == "P4" ? 32U : 1U;
            for (std::size_t i{}; i < rounds; ++i)
            {
                deliver();
                stable(SimulationDuration{0});
            }
        }
    }
    LuaScriptBackendStats luaStats() const noexcept
    {
#if NA1_CANDIDATE
        return backend->stats().lua;
#else
        return backend->stats();
#endif
    }
    void memory(const char *phase) const
    {
        if (!options.diagnostics)
            return;
        const auto vm = luaStats();
        const auto core = system->stats();
        const auto &m = vm.vm_allocations;
        std::printf(
            "MEMORY phase=%s live=%zu peak=%zu active_pages=%zu idle_pages=%zu pinned=%zu rounding=%zu "
            "heap_alloc=%llu heap_free=%llu mount=%zu method=%zu binding=%zu feedback=%zu awaitable=%zu external=%zu "
            "awaitable_record=%zu event_record=%zu threads=%zu resumes=%zu roots_released=%zu\n",
            phase, m.live_bytes, m.peak_live_bytes, m.active_page_backing_bytes, m.idle_page_backing_bytes,
            m.pinned_free_slot_bytes, m.class_rounding_bytes, m.system_allocations, m.system_frees,
            core.mount_backing_bytes, core.method_backing_bytes, core.binding_backing_bytes,
            core.mount_feedback_backing_bytes, core.awaitable_storage_bytes, core.external_ticket_storage_bytes,
            core.awaitable_record_bytes, core.event_waiter_record_bytes, vm.vm_coroutine_creations,
            vm.vm_coroutine_resumes, vm.vm_coroutine_releases);
#if NA1_CANDIDATE
        const auto b = backend->stats();
        std::printf("NATIVE_MEMORY phase=%s composition=%zu frame=%zu metadata=%zu prepared=%zu association=%zu "
                    "active_frames=%zu frame_failures=%zu leases=%zu\n",
                    phase, b.composition_backing_bytes, b.native.frame_storage_bytes, b.native.frame_metadata_bytes,
                    b.native.prepared_method_storage_bytes, b.native.artifact_association_storage_bytes,
                    b.native.active_frames, b.native.frame_capacity_failures, b.active_companion_leases);
#endif
    }
    Options options;
    SimulationDescription domain;
    std::shared_ptr<const lux::script::ScriptArtifactAsset> artifact;
    ecs::Registry registry;
    ScriptTestClock clock;
    HookPoint<void()> hook, read_hook;
    HookPoint<void(const ValuePose &)> pose_hook;
    HookChannel<SimulationBroadcastRoute, std::int32_t> event;
    std::optional<ScriptHookEndpoint<void()>> hook_endpoint, read_endpoint;
    std::optional<ScriptHookEndpoint<void(const ValuePose &)>> pose_endpoint;
    std::optional<ScriptEventEndpoint<SimulationBroadcastRoute, std::int32_t>> event_endpoint;
    lux::script::ScriptEventSourceDescription source;
    Provider provider;
    std::size_t leases{};
#if NA1_CANDIDATE
    std::optional<lux::script::ScriptArtifact> native_artifact;
    std::optional<NativeLuaTaskBackend> backend;
#else
    std::optional<LuaScriptBackend> backend;
#endif
    std::optional<ScriptSystem> system;
};
} // namespace
int main(int argc, char **argv)
{
    try
    {
        Options o;
        for (int i = 1; i < argc; i += 2)
        {
            require(i + 1 < argc, "missing argument");
            const std::string key = argv[i], value = argv[i + 1];
            if (key == "--case")
                o.scenario = value;
            else if (key == "--size")
                o.count = std::stoull(value);
            else if (key == "--warmups")
                o.warmups = std::stoull(value);
            else if (key == "--batches")
                o.batches = std::stoull(value);
            else if (key == "--output")
                o.output = value;
            else if (key == "--diagnostics")
                o.diagnostics = value == "on";
            else if (key == "--keep-lua-reserve")
                o.keep_lua_reserve = value == "on";
            else
                throw std::runtime_error("unknown option");
        }
        require(o.count && o.batches &&
                    (o.scenario == "P1" || o.scenario == "P2" || o.scenario == "P3" || o.scenario == "P4" ||
                     o.scenario == "P5"),
                "invalid workload");
        const auto prepare_begin = Clock::now();
        Harness h(o);
        const auto prepare_ns = std::chrono::duration_cast<Nanos>(Clock::now() - prepare_begin).count();
        h.memory("prepared");
        for (std::size_t i{}; i < o.warmups; ++i)
            h.wave(o.diagnostics && i == 0U);
        h.memory("warm");
        require(h.system->failures().empty(), "warmup failed");
        const auto start = h.system->stats();
        const auto lua_start = h.luaStats();
        const auto provider_start = h.provider.calls;
        std::vector<std::uint64_t> durations(o.batches);
        const auto total_start = Clock::now();
        for (std::size_t i{}; i < o.batches; ++i)
        {
            const auto begin = Clock::now();
            h.wave();
            durations[i] = std::chrono::duration_cast<Nanos>(Clock::now() - begin).count();
        }
        const auto elapsed = std::chrono::duration_cast<Nanos>(Clock::now() - total_start).count();
        const auto finish = h.system->stats();
        const auto lua_finish = h.luaStats();
        h.memory("end");
        const auto tasks = o.count * o.batches;
        const auto waits_per_task = o.scenario == "P3" ? 3U : o.scenario == "P4" ? 32U : 1U;
        require(finish.step_invocations - start.step_invocations == tasks, "new task count");
        require(finish.backend_resume_calls - start.backend_resume_calls == tasks * waits_per_task, "resume count");
        require(finish.suspensions_admitted - start.suspensions_admitted == tasks * waits_per_task, "wait count");
        require(!finish.active_continuations && !finish.active_awaitables && !finish.active_event_waiters &&
                    !finish.next_step_waits && !finish.simulation_delay_waits && !finish.resume_queue_depth,
                "backlog");
        require(h.system->failures().empty() && !finish.invocation_failures, "runtime errors");
#if NA1_CANDIDATE
        require(!lua_finish.vm_coroutine_creations && !lua_finish.vm_coroutine_resumes &&
                    !lua_finish.vm_coroutine_releases && !h.backend->stats().native.active_frames,
                "task Lua resources");
#else
        require(lua_finish.vm_coroutine_creations - lua_start.vm_coroutine_creations == tasks &&
                    lua_finish.vm_coroutine_releases - lua_start.vm_coroutine_releases == tasks,
                "Lua thread lifetime");
#endif
        const auto provider_calls = h.provider.calls - provider_start;
        require(provider_calls == (o.scenario == "P3" ? tasks : 0U), "provider business count");
        const auto cycles = o.warmups + o.batches;
        const auto increment = o.scenario == "P1"   ? 31U
                               : o.scenario == "P2" ? 11U
                               : o.scenario == "P3" ? 1U
                               : o.scenario == "P4" ? 32U * 31U
                                                    : 61U;
        const auto expected = 1U + cycles * increment;
        std::vector<std::int32_t> readback(o.count, -1);
        h.provider.oracle = readback;
        require(dispatchRuntimeHook(*h.system, h.read_hook) == 1U, "readback dispatch");
        h.provider.oracle = {};
        require(h.provider.oracle_count == o.count, "readback count");
        for (const auto value : readback)
            require(value == expected, "independent instance value");
        require(h.system->shutdown().has_value(), "shutdown");
        require(h.leases == 0U && !h.system->activeInstanceCount(), "resource release");
        h.memory("closed");
        std::ofstream csv(o.output);
        require(static_cast<bool>(csv), "CSV open");
        csv << "batch,nanoseconds,validated_tasks_per_wave,validated_waits_per_wave,per_wave_errors_unobserved,per_"
               "wave_backlog_unobserved\n";
        for (std::size_t i{}; i < durations.size(); ++i)
            csv << i << ',' << durations[i] << ',' << o.count << ',' << o.count * waits_per_task << ",,\n";
        std::printf(
            "BUSINESS case=%s side=%c instances=%zu warmups=%zu batches=%zu tasks=%zu waits=%zu "
            "resumes=%llu provider=%zu per_instance=%zu errors=%llu backlog=%zu elapsed_ns=%lld prepare_ns=%lld "
            "threads=%zu lua_resumes=%zu released=%zu leases=%zu diagnostics=%d\n",
            o.scenario.c_str(), NA1_CANDIDATE ? 'B' : 'A', o.count, o.warmups, o.batches, tasks, tasks * waits_per_task,
            finish.backend_resume_calls - start.backend_resume_calls, provider_calls, expected,
            finish.invocation_failures, finish.resume_queue_depth, elapsed, prepare_ns,
            lua_finish.vm_coroutine_creations - lua_start.vm_coroutine_creations,
            lua_finish.vm_coroutine_resumes - lua_start.vm_coroutine_resumes,
            lua_finish.vm_coroutine_releases - lua_start.vm_coroutine_releases, h.leases, o.diagnostics);
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "INVALID_TRIAL: %s\n", error.what());
        return 1;
    }
}
