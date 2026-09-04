#include "PhysicsQuery2D.ability.generated.hpp"
#include "Physics2DScriptTestSupport.hpp"
#include "DelayAbility.ability.generated.hpp"

#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>
#include <lux/engine/function/script/native/NativeModule.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/abilities/DelayAbility.hpp>
#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptBridge.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#include <lux/engine/simulation/scripting/native/NativeScriptBackend.hpp>
#include <lux/engine/task/TaskExecutor.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
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
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifndef LUX_BENCHMARK_GIT_COMMIT
#define LUX_BENCHMARK_GIT_COMMIT "unknown"
#endif
#ifndef LUX_BENCHMARK_BUILD_TYPE
#define LUX_BENCHMARK_BUILD_TYPE "unknown"
#endif

namespace
{
    using Clock = std::chrono::steady_clock;
    using namespace lux;
    using namespace lux::physics2d;
    using namespace lux::physics2d::test;
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    inline constexpr lux::script::ScriptSymbolId CppTickSymbol{0x502D3001U};
    inline std::atomic_size_t g_allocations{};
    inline std::atomic_bool g_count_allocations{};
    inline std::uint64_t g_cpp_checksum{};

    struct Options final
    {
        std::string group{"physics-micro"};
        std::size_t size{2500U};
        std::size_t frames{30U};
        std::size_t warmups{5U};
        std::uint64_t seed{0x5EED2026ULL};
        std::filesystem::path output{"physics2d_script_pb3.csv"};
        std::filesystem::path lua_artifact;
        std::filesystem::path flowforge_artifact;
        lux::script::lua::ELuaExecutionPolicy lua_policy{lux::script::lua::ELuaExecutionPolicy::DEFAULT};
    };

    struct Row final
    {
        std::string scenario;
        std::string backend;
        std::size_t size{};
        std::size_t sample{};
        std::uint64_t nanoseconds{};
        std::size_t allocations{};
        std::size_t active_instances{};
        std::uint64_t physics_queries{};
        std::uint64_t script_calls{};
        std::uint64_t events{};
        std::size_t continuations{};
        std::size_t awaitables{};
        std::size_t event_waiters{};
        std::size_t next_step_waits{};
        std::size_t queue_depth{};
        std::size_t queue_high_water{};
        std::uint64_t checksum{};
    };

    [[nodiscard]] bool parseSize(std::string_view text, std::size_t& output) noexcept
    {
        const auto result = std::from_chars(text.data(), text.data() + text.size(), output);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size() && output != 0U;
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
            else if (key == "--warmups")
            {
                if (!parseSize(value, result.warmups))
                    return std::nullopt;
            }
            else if (key == "--output")
                result.output = value;
            else if (key == "--lua-artifact")
                result.lua_artifact = value;
            else if (key == "--flowforge-artifact")
                result.flowforge_artifact = value;
            else if (key == "--lua-policy")
            {
                if (value == "default")
                    result.lua_policy = lux::script::lua::ELuaExecutionPolicy::DEFAULT;
                else if (value == "interpreter-only")
                    result.lua_policy = lux::script::lua::ELuaExecutionPolicy::INTERPRETER_ONLY;
                else
                    return std::nullopt;
            }
            else if (key == "--mode")
            {
                if (value == "performance")
                {
                    result.warmups = 300U;
                    if (!frames_supplied)
                        result.frames = 5000U;
                }
                else if (value != "diagnostic")
                    return std::nullopt;
            }
            else
                return std::nullopt;
        }
        const bool valid_group = result.group == "physics-micro" || result.group == "scene-physics-mixed";
        const bool missing_artifact =
            result.group == "scene-physics-mixed" && (result.lua_artifact.empty() || result.flowforge_artifact.empty());
        return valid_group && !missing_artifact ? std::optional<Options>{std::move(result)} : std::nullopt;
    }

    template <class Operation>
    [[nodiscard]] Row measure(std::string scenario,
                              std::string backend,
                              std::size_t size,
                              std::size_t sample,
                              Operation&& operation)
    {
        g_allocations.store(0U, std::memory_order_relaxed);
        g_count_allocations.store(true, std::memory_order_release);
        const auto begin = Clock::now();
        Row result = operation();
        const auto end = Clock::now();
        g_count_allocations.store(false, std::memory_order_release);
        result.scenario = std::move(scenario);
        result.backend = std::move(backend);
        result.size = size;
        result.sample = sample;
        result.nanoseconds =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
        result.allocations = g_allocations.load(std::memory_order_relaxed);
        return result;
    }

    void writeRows(const Options& options, std::span<const Row> rows)
    {
        if (!options.output.parent_path().empty())
            std::filesystem::create_directories(options.output.parent_path());
        std::ofstream output(options.output, std::ios::trunc);
        if (!output)
            throw std::runtime_error("cannot open Physics2D benchmark output");
        output << "git_commit,build_type,scenario,backend,size,seed,sample,nanoseconds,allocations,"
                  "active_instances,physics_queries,script_calls,events,continuations,awaitables,event_waiters,"
                  "next_step_waits,queue_depth,queue_high_water,checksum\n";
        for (const auto& row : rows)
        {
            output << LUX_BENCHMARK_GIT_COMMIT << ',' << LUX_BENCHMARK_BUILD_TYPE << ',' << row.scenario << ','
                   << row.backend << ',' << row.size << ',' << options.seed << ',' << row.sample << ','
                   << row.nanoseconds << ',' << row.allocations << ',' << row.active_instances << ','
                   << row.physics_queries << ',' << row.script_calls << ',' << row.events << ','
                   << row.continuations << ',' << row.awaitables << ',' << row.event_waiters << ','
                   << row.next_step_waits << ',' << row.queue_depth << ',' << row.queue_high_water << ','
                   << row.checksum << '\n';
        }
    }

    [[nodiscard]] std::shared_ptr<const lux::script::ScriptArtifactAsset> loadArtifact(
        const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("cannot open Physics2D benchmark artifact");
        input.seekg(0, std::ios::end);
        const auto encoded_size = input.tellg();
        if (encoded_size < std::streamoff{56})
            throw std::runtime_error("invalid Physics2D benchmark artifact");
        input.seekg(0, std::ios::beg);
        std::vector<std::byte> bytes(static_cast<std::size_t>(encoded_size));
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(encoded_size));
        std::array<std::uint8_t, 16U> id_bytes{};
        for (std::size_t index{}; index < id_bytes.size(); ++index)
            id_bytes[index] = std::to_integer<std::uint8_t>(bytes[40U + index]);
        auto decoded = asset::TAssetSerDeser<lux::script::ScriptArtifactAsset>::decode(
            asset::AssetId{id_bytes},
            lux::cxx::SharedBytes<>::copyOf(bytes),
            {bytes.size(), std::numeric_limits<std::size_t>::max(), 0U});
        if (!decoded)
            throw std::runtime_error("cannot decode Physics2D benchmark artifact");
        return std::move(*decoded);
    }

    [[nodiscard]] asset::AssetId cppAssetId()
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.front() = 0x2DU;
        bytes.back() = 0xC0U;
        return asset::AssetId{bytes};
    }

    [[nodiscard]] world::WorldObjectId objectId(std::size_t index)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.front() = 0x2DU;
        for (std::size_t byte{}; byte < sizeof(index); ++byte)
            bytes[8U + byte] = static_cast<std::uint8_t>((index >> (byte * 8U)) & 0xFFU);
        return world::WorldObjectId{uuids::uuid{bytes}};
    }

    struct CppObject final
    {
        std::uint64_t value{};
    };

    struct CppFixture final
    {
        explicit CppFixture(std::size_t capacity)
        {
            reflected.name = "PhysicsMixedCppObject";
            reflected.full_name = "lux.physics2d.benchmark.PhysicsMixedCppObject";
            reflected.type = meta::ref_type_of_v<CppObject>;
            reflected.construct = [](void* memory) { std::construct_at(static_cast<CppObject*>(memory)); };
            reflected.destruct = [](void* object) { std::destroy_at(static_cast<CppObject*>(object)); };
            tick.owner_class = std::addressof(reflected);
            tick.invokable.name = "tick";
            tick.invokable.full_name = "tick";
            tick.invokable.return_type = meta::ref_type_of_v<void>;
            tick.invokable.invoker = [](void* object, void**, void*) {
                auto& value = *static_cast<CppObject*>(object);
                ++value.value;
                g_cpp_checksum += value.value;
            };
            tick.is_noexcept = true;
            const std::array methods{std::addressof(tick)};
            const std::array symbols{CppTickSymbol};
            auto projected = projectCppStaticEntityScript("lux.physics2d.cpp-benchmark",
                                                          "physics2d-cpp-benchmark-v1",
                                                          reflected,
                                                          methods,
                                                          symbols,
                                                          {});
            if (!projected)
                throw std::runtime_error("cannot project Physics2D C++ benchmark script");
            descriptor.emplace(std::move(*projected));
            auto created_artifact = lux::script::ScriptArtifact::create(descriptor->description(), {});
            if (!created_artifact)
                throw std::runtime_error("cannot create Physics2D C++ benchmark artifact");
            artifact.emplace(std::move(*created_artifact));
            const std::array pools{CppStaticScriptPoolDescription{std::addressof(*descriptor), capacity}};
            auto created_backend = CppStaticScriptBackend::create(pools);
            if (!created_backend)
                throw std::runtime_error("cannot create Physics2D C++ benchmark backend");
            backend.emplace(std::move(*created_backend));
        }

        meta::RefClass reflected;
        meta::RefMethod tick;
        std::optional<CppStaticScriptDescriptor> descriptor;
        std::optional<lux::script::ScriptArtifact> artifact;
        std::optional<CppStaticScriptBackend> backend;
    };

    struct Sources final
    {
        const lux::script::ScriptArtifact* cpp{};
        const lux::script::ScriptArtifact* flow{};
        const lux::script::ScriptArtifact* lua{};
        const lux::script::NativeModule* flow_module{};
        asset::AssetId flow_id;
        asset::AssetId lua_id;
        std::unordered_map<world::WorldObjectId, ecs::Entity, world::WorldObjectIdHash> objects;

        static bool resolveArtifact(void* context,
                                    const asset::AssetId& requested,
                                    ResolvedScriptArtifact& output) noexcept
        {
            const auto& self = *static_cast<Sources*>(context);
            if (requested == cppAssetId())
                output.artifact = self.cpp;
            else if (requested == self.flow_id)
                output.artifact = self.flow;
            else if (requested == self.lua_id)
                output.artifact = self.lua;
            return output.artifact != nullptr;
        }

        static bool resolveModule(void* context,
                                  const asset::AssetId& requested,
                                  const lux::script::ScriptArtifact&,
                                  ResolvedNativeModule& output) noexcept
        {
            const auto& self = *static_cast<Sources*>(context);
            if (requested != self.flow_id)
                return false;
            output.module = self.flow_module;
            return output.module != nullptr;
        }

        static bool resolveWorld(void* context, const world::WorldObjectId& requested, ecs::Entity& output) noexcept
        {
            const auto& self = *static_cast<Sources*>(context);
            const auto found = self.objects.find(requested);
            if (found == self.objects.end())
                return false;
            output = found->second;
            return true;
        }
    };

    struct MixedHarness final
    {
        MixedHarness(const Options& options)
            : flow_asset(loadArtifact(options.flowforge_artifact)), lua_asset(loadArtifact(options.lua_artifact)),
              cpp(options.size / 3U)
        {
            const auto collider = registry.create();
            registry.emplace<ecs::Transform2D>(collider);
            registry.emplace<BoxCollider2D>(collider);
            auto created_simulation = createSimulation(registry);
            auto created_executor = task::TaskExecutor::create({0U, 1U});
            if (!created_simulation || !created_executor)
                throw std::runtime_error("Physics2D benchmark composition failed");
            simulation.emplace(std::move(*created_simulation));
            executor.emplace(std::move(*created_executor));
            if (!simulation->execute(*executor, SimulationDuration{}))
                throw std::runtime_error("Physics2D benchmark Simulation initialization failed");
            physics = static_cast<Physics2DSystem*>(simulation->scriptApiCapabilities().front().context);
            if (physics == nullptr)
                throw std::runtime_error("Physics2D benchmark provider is absent");

            flow_module.emplace(*lux::script::loadNativeModule(flow_asset->data().payload(),
                                                               flow_asset->data().description().module_name));
            sources.cpp = std::addressof(*cpp.artifact);
            sources.flow = std::addressof(flow_asset->data());
            sources.lua = std::addressof(lua_asset->data());
            sources.flow_module = std::addressof(*flow_module);
            sources.flow_id = flow_asset->id();
            sources.lua_id = lua_asset->id();
            sources.objects.reserve(options.size / 3U);

            const std::size_t cpp_count = options.size / 3U;
            const std::size_t flow_count = options.size / 3U;
            ScriptSystemDescriptionBuilder builder;
            for (std::size_t index{}; index < options.size; ++index)
            {
                asset::AssetId asset_id;
                lux::script::ScriptSymbolId symbol{};
                ScriptMountScope scope{SimulationScriptMount{}};
                if (index < cpp_count)
                {
                    asset_id = cppAssetId();
                    symbol = CppTickSymbol;
                    const auto object = objectId(index);
                    const auto entity = registry.create();
                    sources.objects.emplace(object, entity);
                    scope = EntityScriptMount{object};
                }
                else if (index < cpp_count + flow_count)
                {
                    asset_id = sources.flow_id;
                    symbol = FlowTickSymbol;
                }
                else
                {
                    asset_id = sources.lua_id;
                    symbol = TickSymbol;
                }
                if (!builder.addMount({ScriptMountId{index + 1U},
                                       asset_id,
                                       scope,
                                       true,
                                       {{symbol, HookScriptTarget{ProbeSystemId, TickHook}}}}))
                {
                    throw std::runtime_error("Physics2D benchmark mount rejected");
                }
            }
            description.emplace(*std::move(builder).build(simulation->description()));

            native.emplace(
                NativeModuleResolver{std::addressof(sources), &Sources::resolveModule},
                NativeScriptBackendConfig{
                    .module_capacity = 1U,
                    .instance_capacity = flow_count,
                    .prepared_call_capacity = flow_count * 2U,
                    .continuation_capacity = flow_count,
                    .max_ability_imports_per_module = 2U,
                    .max_continuation_frame_bytes = 256U,
                    .continuation_frame_storage_bytes = (std::max)(std::size_t{256U}, flow_count * 128U),
                    .max_event_wait_imports_per_module = 1U
                }
            );
            const std::array contributions{
                lux::script::lua::makeScriptAbilityLuaContribution<PhysicsQuery2D>(),
                lux::script::lua::makeScriptAbilityLuaContribution<DelayAbility>()
            };
            const std::array event_sources{pulseEventSource()};
            const auto lua_count = options.size - cpp_count - flow_count;
            lua.emplace(*LuaScriptBackend::create({
                .instance_capacity = lua_count,
                .prepared_call_capacity = lua_count * 5U,
                .continuation_capacity = lua_count,
                .execution_depth_capacity = 4U,
                .ability_catalog_method_capacity = 5U,
                .prepared_ability_capacity = lua_count * 5U,
                .abilities = contributions,
                .execution_policy = options.lua_policy,
                .event_catalog_capacity = 1U,
                .prepared_event_capacity = lua_count,
                .events = event_sources
            }));
            backends = {cpp.backend->descriptor(), native->descriptor(), lua->descriptor()};
            const auto bounded = (std::max)(options.size, std::size_t{1U});
            auto created = ScriptSystem::create(
                simulation->description(),
                *description,
                registry,
                simulation->clock(),
                {32U, bounded, bounded, bounded, bounded, bounded, 64U, bounded, bounded, bounded, bounded},
                {std::addressof(sources), &Sources::resolveArtifact},
                {std::addressof(sources), &Sources::resolveWorld},
                simulation->scriptApiCapabilities(),
                backends,
                simulation->scriptHookEndpoints(),
                simulation->scriptEventEndpoints());
            if (!created)
                throw std::runtime_error("Physics2D benchmark ScriptSystem creation failed");
            system.emplace(std::move(*created));
            if (!system->prepare())
                throw std::runtime_error("Physics2D benchmark ScriptSystem preparation failed");
        }

        ~MixedHarness()
        {
            if (system)
                static_cast<void>(system->shutdown());
        }

        [[nodiscard]] bool frame()
        {
            const auto simulated = simulation->execute(*executor, std::chrono::milliseconds{16});
            const auto active = system->activeInstanceCount();
            const auto dispatched = ActiveProbe != nullptr ? ActiveProbe->tick.dispatch() : 0U;
            bool event_recorded{};
            std::size_t event_count{};
            if (ActiveProbe != nullptr)
            {
                {
                    auto writer = ActiveProbe->pulse.begin(0U);
                    event_recorded = writer.record(1);
                }
                event_count = ActiveProbe->pulse.drain();
            }
            const auto stable = system->executeStablePoint();
            if (!simulated || ActiveProbe == nullptr || dispatched != 1U || !event_recorded || event_count != 1U ||
                !stable || !system->failures().empty())
            {
                std::fprintf(
                    stderr,
                    "Physics2D mixed frame mismatch: simulation=%d probe=%d dispatch=%zu active=%zu stable=%d\n",
                    simulated ? 1 : 0,
                    ActiveProbe != nullptr ? 1 : 0,
                    dispatched,
                    active,
                    stable ? 1 : 0);
                if (!system->failures().empty())
                {
                    const auto& failure = system->failures().back();
                    std::fprintf(stderr,
                                 "Physics2D mixed Script failure: error=%u status=%d\n",
                                 static_cast<unsigned>(failure.error),
                                 failure.status);
                }
                return false;
            }
            return true;
        }

        ecs::Registry registry;
        std::shared_ptr<const lux::script::ScriptArtifactAsset> flow_asset;
        std::shared_ptr<const lux::script::ScriptArtifactAsset> lua_asset;
        CppFixture cpp;
        std::optional<Simulation> simulation;
        std::optional<task::TaskExecutor> executor;
        Physics2DSystem* physics{};
        std::optional<lux::script::NativeModule> flow_module;
        Sources sources;
        std::optional<ScriptSystemDescription> description;
        std::optional<NativeScriptBackend> native;
        std::optional<LuaScriptBackend> lua;
        std::array<ScriptBackendDescriptor, 3U> backends;
        std::optional<ScriptSystem> system;
    };

    void runMicro(const Options& options, std::vector<Row>& rows)
    {
        ecs::Registry registry;
        const auto collider = registry.create();
        registry.emplace<ecs::Transform2D>(collider);
        registry.emplace<BoxCollider2D>(collider);
        auto simulation = createSimulation(registry);
        if (!simulation)
            throw std::runtime_error("Physics2D micro Simulation creation failed");
        auto created_executor = task::TaskExecutor::create({0U, 1U});
        if (!created_executor)
            throw std::runtime_error("Physics2D micro executor creation failed");
        auto executor = std::move(*created_executor);
        if (!simulation->execute(executor, SimulationDuration{}))
            throw std::runtime_error("Physics2D micro setup failed");
        const auto capability = simulation->scriptApiCapabilities().front();
        auto* provider = static_cast<Physics2DSystem*>(capability.context);
        const lux::script::ScriptAbilityBinding binding{&lux::script::ScriptAbilityTraits<PhysicsQuery2D>::Description,
                                                        capability.context,
                                                        capability.dispatch,
                                                        capability.methods};
        const auto prepared = lux::script::ScriptAbilityCpp<PhysicsQuery2D>::create(binding);
        if (provider == nullptr || !prepared)
            throw std::runtime_error("Physics2D micro binding failed");
        for (std::size_t sample{}; sample < options.frames; ++sample)
        {
            rows.push_back(measure("physics-direct", "physics2d-domain", options.size, sample, [&] {
                std::uint64_t checksum{};
                for (std::size_t index{}; index < options.size; ++index)
                    checksum += provider->overlapsBox(0.0, 0.0, 0.25, 0.25);
                return Row{.physics_queries = options.size, .checksum = checksum};
            }));
            rows.push_back(measure("physics-prepared", "script-ability", options.size, sample, [&] {
                std::uint64_t checksum{};
                for (std::size_t index{}; index < options.size; ++index)
                    checksum += prepared->overlapsBox(0.0, 0.0, 0.25, 0.25);
                return Row{.physics_queries = options.size, .checksum = checksum};
            }));
        }
    }

    void runMixed(const Options& options, std::vector<Row>& rows)
    {
        MixedHarness harness{options};
        for (std::size_t frame{}; frame < options.warmups; ++frame)
        {
            if (!harness.frame())
                throw std::runtime_error("Physics2D mixed warmup failed");
        }
        const auto query_start = harness.physics->stats().overlap_queries;
        const auto cpp_start = g_cpp_checksum;
        for (std::size_t frame{}; frame < options.frames; ++frame)
        {
            rows.push_back(measure("scene-physics-mixed", "cpp-flowforge-lua", options.size, frame, [&] {
                if (!harness.frame())
                    throw std::runtime_error("Physics2D mixed frame failed");
                const auto runtime = harness.system->stats();
                const auto physics = harness.physics->stats();
                return Row{
                    .active_instances = runtime.active_instances,
                    .physics_queries = physics.overlap_queries - query_start,
                    .script_calls = (frame + 1U) * options.size,
                    .events = frame + 1U,
                    .continuations = runtime.active_continuations,
                    .awaitables = runtime.active_awaitables,
                    .event_waiters = runtime.active_event_waiters,
                    .next_step_waits = runtime.next_step_waits,
                    .queue_depth = runtime.resume_queue_depth,
                    .queue_high_water = runtime.resume_queue_high_water,
                    .checksum = g_cpp_checksum - cpp_start
                };
            }));
        }
        if (harness.system->activeInstanceCount() != options.size ||
            harness.physics->stats().overlap_queries == query_start)
            throw std::runtime_error("Physics2D mixed observation mismatch");
    }
}

int main(int argc, char** argv)
{
    const auto options = parseOptions(argc, argv);
    if (!options)
        return 2;
    try
    {
        std::vector<Row> rows;
        if (options->group == "physics-micro")
            runMicro(*options, rows);
        else
            runMixed(*options, rows);
        writeRows(*options, rows);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Physics2D benchmark failed: %s\n", error.what());
        return 3;
    }
}

void* operator new(std::size_t size)
{
    if (g_count_allocations.load(std::memory_order_relaxed))
        g_allocations.fetch_add(1U, std::memory_order_relaxed);
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
