#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/simulation/ecs/EcsSnapshot.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ScriptBindingSession.hpp>
#include <lux/engine/simulation/SystemEventBuffer.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskGraphBuilder.hpp>

#include <entt/signal/sigh.hpp>

#include <atomic>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
#include <utility>
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
    using namespace lux::simulation::ecs;

    std::atomic_size_t g_allocation_count{};
    std::atomic_bool g_count_allocations{};

    struct Options final
    {
        std::string group{"task-graph"};
        std::string mode{"diagnostic"};
        std::size_t size{1024U};
        std::filesystem::path output{"ecs_l1_benchmark.csv"};
        std::size_t warmups{1U};
        std::size_t samples{3U};
    };

    [[nodiscard]] std::optional<Options> parseOptions(int argc, char** argv)
    {
        Options result;
        for (int index = 1; index < argc; ++index)
        {
            if (index + 1 >= argc)
                return std::nullopt;
            const std::string_view key{argv[index++]};
            const std::string_view value{argv[index]};
            if (key == "--group")
                result.group = value;
            else if (key == "--mode")
                result.mode = value;
            else if (key == "--output")
                result.output = value;
            else if (key == "--size")
            {
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result.size);
                const bool is_parse_error = parsed.ec != std::errc{};
                const bool has_trailing_characters = parsed.ptr != value.data() + value.size();
                const bool is_empty_size = result.size == 0U;
                const bool exceeds_max_size = result.size > std::numeric_limits<std::uint32_t>::max();
                const bool is_invalid_size = is_parse_error || has_trailing_characters || is_empty_size ||
                    exceeds_max_size;
                if (is_invalid_size)
                {
                    return std::nullopt;
                }
            }
            else
                return std::nullopt;
        }
        if (result.mode == "performance")
        {
            result.warmups = 5U;
            result.samples = 30U;
        }
        const bool is_known_group = result.group == "task-graph" || result.group == "world" ||
            result.group == "command-buffer" || result.group == "reactive-dirty" ||
            result.group == "cpp-method-prepared" || result.group == "hook-global-multi" ||
            result.group == "hook-entity-multi" || result.group == "global-event" ||
            result.group == "entity-targeted-event-sparse" || result.group == "owned-worker-event-buffer" ||
            result.group == "ecs-snapshot";
        if (!is_known_group)
        {
            return std::nullopt;
        }
        return result;
    }

    struct Observation final
    {
        std::size_t updates{};
        std::size_t notifications{};
        std::size_t callbacks{};
        std::size_t asset_resolution_delta{};
        std::size_t target_resolution_delta{};
        std::size_t entities_examined{};
        std::size_t target_range_lookups{};
        std::size_t handlers_visited{};
        std::size_t target_ranges_built{};
        std::size_t dispatch_handlers_built{};
        std::size_t instance_creates{};
        std::size_t method_prepares{};
        std::size_t frame_builds{};
    };

    struct Sample final
    {
        std::size_t index{};
        std::uint64_t nanoseconds{};
        std::size_t allocations{};
        Observation observation;
    };

    template <class Setup, class Operation, class Teardown>
    [[nodiscard]] std::vector<Sample>
    measure(const Options& options, Setup&& setup, Operation&& operation, Teardown&& teardown)
    {
        for (std::size_t index{}; index < options.warmups; ++index)
        {
            auto state = setup();
            (void)operation(*state);
            teardown(*state);
        }

        std::vector<Sample> result;
        result.reserve(options.samples);
        for (std::size_t index{}; index < options.samples; ++index)
        {
            auto state = setup();
            g_allocation_count.store(0U, std::memory_order_relaxed);
            g_count_allocations.store(true, std::memory_order_release);
            const auto begin = Clock::now();
            const Observation observation = operation(*state);
            const auto end = Clock::now();
            g_count_allocations.store(false, std::memory_order_release);
            teardown(*state);
            result.push_back(Sample{
                index,
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()),
                g_allocation_count.load(std::memory_order_relaxed),
                observation}
            );
        }
        return result;
    }

    void writeCsv(const Options& options, std::string_view metric, const std::vector<Sample>& samples)
    {
        if (!options.output.parent_path().empty())
            std::filesystem::create_directories(options.output.parent_path());
        auto temporary = options.output;
        temporary += ".tmp";
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output)
                throw std::runtime_error("cannot open benchmark output");
            output << "benchmark_schema_version,git_commit,build_type,group,"
                      "metric,size,sample,nanoseconds,allocations,updates,"
                      "notifications,callbacks,asset_resolution_delta,"
                      "target_resolution_delta,entities_examined,"
                      "target_range_lookups,handlers_visited,"
                      "target_ranges_built,dispatch_handlers_built,"
                      "instance_creates,method_prepares,frame_builds\n";
            for (const auto& sample : samples)
            {
                output << "8," << LUX_BENCHMARK_GIT_COMMIT << ',' << LUX_BENCHMARK_BUILD_TYPE << ',' << options.group
                       << ',' << metric << ',' << options.size << ',' << sample.index << ',' << sample.nanoseconds
                       << ',' << sample.allocations << ',' << sample.observation.updates << ','
                       << sample.observation.notifications << ',' << sample.observation.callbacks << ','
                       << sample.observation.asset_resolution_delta << ',' << sample.observation.target_resolution_delta
                       << ',' << sample.observation.entities_examined << ',' << sample.observation.target_range_lookups
                       << ',' << sample.observation.handlers_visited << ',' << sample.observation.target_ranges_built
                       << ',' << sample.observation.dispatch_handlers_built << ','
                       << sample.observation.instance_creates << ',' << sample.observation.method_prepares << ','
                       << sample.observation.frame_builds << '\n';
            }
        }
        std::error_code error;
        std::filesystem::remove(options.output, error);
        std::filesystem::rename(temporary, options.output);
    }

    struct Position final
    {
        std::uint32_t value{};
    };

    struct WorldState final
    {
        explicit WorldState(std::size_t count)
        {
            entities.reserve(count);
            registry.storage<Position>().reserve(count);
            for (std::size_t index{}; index < count; ++index)
            {
                const Entity entity = registry.create();
                registry.emplace<Position>(entity, Position{static_cast<std::uint32_t>(index)});
                entities.push_back(entity);
            }
        }

        Registry registry;
        std::vector<Entity> entities;
    };

    struct ReactiveDirtyState final
    {
        explicit ReactiveDirtyState(std::size_t count)
        {
            entities.reserve(count);
            dirty.reserve(count);
            registry.storage<Position>().reserve(count);
            for (std::size_t index{}; index < count; ++index)
            {
                const Entity entity = registry.create();
                registry.emplace<Position>(entity);
                entities.push_back(entity);
            }
            connection = registry.on_update<Position>().connect<&ReactiveDirtyState::onUpdate>(*this);
        }

        void onUpdate(Registry&, Entity entity) noexcept
        {
            dirty.push_back(entity);
        }

        Registry registry;
        std::vector<Entity> entities;
        std::vector<Entity> dirty;
        entt::scoped_connection connection;
    };

    struct CommandState final
    {
        explicit CommandState(std::size_t count)
        {
            entities.reserve(count);
            registry.storage<Position>().reserve(count);
            for (std::size_t index{}; index < count; ++index)
            {
                const Entity entity = registry.create();
                registry.emplace<Position>(entity);
                entities.push_back(entity);
            }
            const EcsCommandProducerCapacity capacity{count, 0U};
            if (!commands.prepare(std::span{&capacity, 1U}))
                throw std::runtime_error("command prepare failed");
        }

        Registry registry;
        std::vector<Entity> entities;
        EcsCommandBuffer commands;
    };

    inline constexpr std::array kBindingHooks{
        lux::simulation::makeSystemHookPoint<void()>("update", lux::simulation::ESystemHookCardinality::MULTI)};
    inline constexpr std::array kBindingEvents{
        lux::simulation::makeSystemEvent<void>(
            "global-event",
            kBindingHooks[0],
            lux::simulation::ESystemEventTarget::GLOBAL,
            {},
            0U
        ),
        lux::simulation::makeSystemEvent<void>(
            "entity-event",
            kBindingHooks[0],
            lux::simulation::ESystemEventTarget::ENTITY_TARGETED,
            {},
            0U)};
    inline constexpr lux::simulation::SystemDescription kBindingSystem{
        .canonical_name = "lux.benchmark.binding",
        .version = 1U,
        .hooks = kBindingHooks,
        .events = kBindingEvents};

    struct BindingBenchmarkState final
    {
        static constexpr std::size_t kSparseTargetCount{300U};

        BindingBenchmarkState(std::size_t count, std::string_view kind) : kind(kind)
        {
            std::array<std::uint8_t, 16U> id_bytes{};
            id_bytes[0] = 0xB6U;
            asset_id = lux::asset::AssetId{id_bytes};
            asset.description.module_name = "l1.benchmark.binding";
            const bool entity_model = kind == "hook-entity-multi" || kind == "entity-targeted-event-sparse";
            asset.description.model =
                entity_model ? lux::rdesc::EScriptModel::ENTITY_BEHAVIOR : lux::rdesc::EScriptModel::GLOBAL_MODULE;
            asset.description.body = lux::rdesc::CppStaticScript{"benchmark"};
            const auto hook_count = kind == "hook-global-multi" ? 4U : 1U;
            for (std::uint64_t symbol = 1U; symbol <= hook_count; ++symbol)
            {
                asset.description.exports.push_back(
                    lux::rdesc::ScriptFunction{"hook-" + std::to_string(symbol), symbol, {}, {}}
                );
            }
            asset.description.exports.push_back(lux::rdesc::ScriptFunction{"global", 5U, {}, {}});
            asset.description.exports.push_back(lux::rdesc::ScriptFunction{"entity", 6U, {}, {}});

            std::vector<std::string> sparse_hook_names;
            std::vector<std::string> sparse_event_names;
            std::vector<lux::simulation::SystemHookPoint> sparse_hooks;
            std::vector<lux::simulation::SystemEventDescription> sparse_events;
            auto benchmark_system = kBindingSystem;
            if (kind == "entity-targeted-event-sparse")
            {
                sparse_hook_names.reserve(kSparseTargetCount);
                sparse_event_names.reserve(kSparseTargetCount);
                sparse_hooks.reserve(kSparseTargetCount);
                sparse_events.reserve(kSparseTargetCount);
                for (std::size_t index{}; index < kSparseTargetCount; ++index)
                {
                    sparse_hook_names.push_back("sparse-dispatch-" + std::to_string(index));
                    sparse_event_names.push_back("sparse-event-" + std::to_string(index));
                }
                for (std::size_t index{}; index < kSparseTargetCount; ++index)
                {
                    sparse_hooks.push_back(lux::simulation::SystemHookPoint{
                        sparse_hook_names[index],
                        lux::simulation::ESystemHookCardinality::MULTI,
                        kBindingHooks[0].signature}
                    );
                    sparse_events.push_back(lux::simulation::makeSystemEvent<void>(
                        sparse_event_names[index],
                        sparse_hooks[index],
                        lux::simulation::ESystemEventTarget::ENTITY_TARGETED,
                        {},
                        0U)
                    );
                }
                benchmark_system.hooks = sparse_hooks;
                benchmark_system.events = sparse_events;
            }

            lux::simulation::SimulationDescriptionBuilder builder;
            if (!builder.addSystem("benchmark", benchmark_system))
                throw std::runtime_error("binding system build failed");
            std::vector<lux::simulation::ScriptBindingDescription> global_bindings;
            if (kind == "cpp-method-prepared" || kind == "hook-global-multi")
            {
                const auto handler_count = kind == "hook-global-multi" ? 4U : 1U;
                for (std::uint64_t symbol = 1U; symbol <= handler_count; ++symbol)
                {
                    global_bindings.push_back(
                        {symbol,
                         lux::simulation::SystemHookBindingTarget{
                             lux::simulation::systemTypeId(kBindingSystem.canonical_name),
                             "benchmark",
                             "update"}}
                    );
                }
            }
            else if (kind == "global-event")
            {
                global_bindings.push_back(
                    {5U,
                     lux::simulation::SystemEventBindingTarget{
                         lux::simulation::systemTypeId(kBindingSystem.canonical_name),
                         "benchmark",
                         "global-event"}}
                );
            }
            if (!global_bindings.empty() && !builder.addGlobalScriptMount(lux::simulation::ScriptMountDescription{
                                                lux::simulation::ScriptMountId{1U},
                                                asset_id,
                                                std::move(global_bindings)}))
            {
                throw std::runtime_error("binding mount build failed");
            }
            auto description = std::move(builder).build();
            if (!description)
                throw std::runtime_error("binding description build failed");

            if (entity_model)
            {
                const auto scripted_count =
                    kind == "entity-targeted-event-sparse" ? std::min<std::size_t>(count, 10000U) : count;
                entities.reserve(scripted_count);
                registry.storage<lux::simulation::ScriptComponent>().reserve(scripted_count);
                std::size_t scripted_ordinal{};
                for (std::size_t index{}; index < count; ++index)
                {
                    const auto entity = registry.create();
                    const auto scripted_index =
                        scripted_count == 1U ? count - 1U : scripted_ordinal * (count - 1U) / (scripted_count - 1U);
                    if (scripted_ordinal < scripted_count && index == scripted_index)
                    {
                        const auto symbol = kind == "hook-entity-multi" ? 1U : 6U;
                        const auto sparse_target_index = scripted_ordinal % kSparseTargetCount;
                        lux::simulation::ScriptBindingTarget target =
                            kind == "hook-entity-multi"
                                ? lux::simulation::ScriptBindingTarget{lux::simulation::SystemHookBindingTarget{
                                      lux::simulation::systemTypeId(kBindingSystem.canonical_name),
                                      "benchmark",
                                      "update"}}
                                : lux::simulation::ScriptBindingTarget{lux::simulation::SystemEventBindingTarget{
                                      lux::simulation::systemTypeId(kBindingSystem.canonical_name),
                                      "benchmark",
                                      "sparse-event-" + std::to_string(sparse_target_index)}};
                        registry.emplace<lux::simulation::ScriptComponent>(
                            entity,
                            lux::simulation::ScriptComponent{
                                {{lux::simulation::ScriptMountId{1U}, asset_id, {{symbol, std::move(target)}}}}}
                        );
                        entities.push_back(entity);
                        ++scripted_ordinal;
                    }
                }
                if (entities.size() != scripted_count)
                    throw std::runtime_error("scripted entity distribution failed");
            }

            const lux::simulation::ScriptBackendDescriptor backend{
                lux::rdesc::Script::Kind::CPP_STATIC,
                this,
                &BindingBenchmarkState::createInstance,
                &BindingBenchmarkState::prepareMethod,
                &BindingBenchmarkState::releaseMethod,
                &BindingBenchmarkState::destroyInstance};
            const auto mount_count = std::max<std::size_t>(entities.size() + (!global_bindings.empty() ? 1U : 0U), 1U);
            auto created = lux::simulation::ScriptBindingSession::create(
                std::move(*description),
                registry,
                lux::simulation::ScriptBindingCapacities{
                    mount_count + 4U,
                    mount_count * 4U + 4U,
                    entities.size() + 4U,
                    mount_count * 4U + 4U,
                    mount_count * 4U + 4U,
                    entities.size() + 4U,
                    16U},
                lux::simulation::ScriptAssetResolver{this, &BindingBenchmarkState::resolveAsset},
                std::span{&backend, 1U}
            );
            if (!created)
                throw std::runtime_error("binding session create failed");
            session = std::make_unique<lux::simulation::ScriptBindingSession>(std::move(*created));
            if (!session->prepare())
                throw std::runtime_error("binding session prepare failed");
            hook = session->hookSlot("benchmark", "update");
            global_event = session->eventSlot("benchmark", "global-event");
            if (kind == "entity-targeted-event-sparse")
            {
                entity_events.reserve(kSparseTargetCount);
                for (std::size_t index{}; index < kSparseTargetCount; ++index)
                {
                    const auto slot = session->eventSlot("benchmark", "sparse-event-" + std::to_string(index));
                    if (!slot)
                        throw std::runtime_error("sparse event slot missing");
                    entity_events.push_back(slot);
                }
            }
        }

        static bool
        resolveAsset(void* opaque, const lux::asset::AssetId& id, lux::simulation::ResolvedScriptAsset& result) noexcept
        {
            auto& self = *static_cast<BindingBenchmarkState*>(opaque);
            if (id != self.asset_id)
                return false;
            result.asset = std::addressof(self.asset);
            return true;
        }

        static lux::simulation::EScriptBackendResult createInstance(
            void* opaque,
            const lux::simulation::ScriptInstanceCreateContext&,
            const lux::asset::ScriptAssetContent&,
            lux::simulation::ScriptBackendInstance& result
        ) noexcept
        {
            result.value = opaque;
            return lux::simulation::EScriptBackendResult::SUCCESS;
        }

        static lux::simulation::EScriptBackendResult prepareMethod(
            void* opaque,
            lux::simulation::ScriptBackendInstance,
            const lux::rdesc::ScriptFunction&,
            lux::script::BoundScriptCall& result
        ) noexcept
        {
            result = lux::script::BoundScriptCall{&BindingBenchmarkState::invoke, opaque};
            return lux::simulation::EScriptBackendResult::SUCCESS;
        }

        static void releaseMethod(void*, lux::simulation::ScriptBackendInstance, lux::script::BoundScriptCall) noexcept
        {
        }

        static void destroyInstance(void*, lux::simulation::ScriptBackendInstance) noexcept
        {
        }

        static int invoke(lux_script_call_frame* frame) noexcept
        {
            ++static_cast<BindingBenchmarkState*>(frame->user_context)->callbacks;
            return 0;
        }

        std::string kind;
        Registry registry;
        std::vector<Entity> entities;
        lux::asset::AssetId asset_id;
        lux::asset::ScriptAssetContent asset;
        std::unique_ptr<lux::simulation::ScriptBindingSession> session;
        lux::simulation::ScriptHookSlot hook;
        lux::simulation::ScriptEventSlot global_event;
        std::vector<lux::simulation::ScriptEventSlot> entity_events;
        std::size_t callbacks{};
    };

    struct OwnedEventBufferBenchmarkState final
    {
        explicit OwnedEventBufferBenchmarkState(std::size_t count) : count(count)
        {
            if (!buffer.prepare(4U, (count + 3U) / 4U))
                throw std::runtime_error("event buffer prepare failed");
            lux::task::TaskGraphBuilder builder;
            for (std::size_t producer{}; producer < 4U; ++producer)
            {
                if (!builder.add([this, producer]() noexcept {
                        auto begun = buffer.writer(producer);
                        if (!begun)
                        {
                            producer_failed.store(true);
                            return;
                        }
                        auto writer = std::move(*begun);
                        for (std::size_t index = producer; index < this->count; index += 4U)
                        {
                            if (!writer.emit(NullEntity, static_cast<std::uint64_t>(index)))
                            {
                                producer_failed.store(true);
                                return;
                            }
                        }
                        producers_completed.fetch_add(1U);
                    }))
                {
                    throw std::runtime_error("event producer task add failed");
                }
            }
            auto built = std::move(builder).build();
            if (!built)
                throw std::runtime_error("event producer graph build failed");
            graph = std::move(*built);
            executor = std::make_unique<lux::task::TaskExecutor>(lux::task::TaskExecutorConfig{4U, graph.taskCount()});
        }

        std::size_t count{};
        lux::simulation::SystemEventBuffer<std::uint64_t> buffer;
        lux::task::TaskGraph graph;
        std::unique_ptr<lux::task::TaskExecutor> executor;
        std::atomic_bool producer_failed{};
        std::atomic_size_t producers_completed{};
        std::size_t callbacks{};
    };

    struct TaskGraphState final
    {
        explicit TaskGraphState(std::size_t count)
        {
            lux::task::TaskGraphBuilder builder;
            for (std::size_t index{}; index < count; ++index)
            {
                if (!builder.add([this]() noexcept { ++executed; }))
                    throw std::runtime_error("task add failed");
            }
            auto built = std::move(builder).build();
            if (!built)
                throw std::runtime_error("task graph build failed");
            graph = std::move(*built);
            executor = std::make_unique<lux::task::TaskExecutor>(lux::task::TaskExecutorConfig{0U, graph.taskCount()});
        }

        std::atomic_size_t executed{};
        lux::task::TaskGraph graph;
        std::unique_ptr<lux::task::TaskExecutor> executor;
    };

    struct SnapshotState final
    {
        explicit SnapshotState(std::size_t count)
        {
            std::vector<ComponentSchema> schemas_values;
            const auto transform_schemas = transformComponentSchemas();
            schemas_values.insert(schemas_values.end(), transform_schemas.begin(), transform_schemas.end());
            auto built_schemas = ComponentSchemaSet::build(std::move(schemas_values));
            if (!built_schemas)
                throw std::runtime_error("schema build failed");
            schemas = std::move(*built_schemas);
            const std::array contributions{transformComponentSnapshotContribution()};
            auto built_components = ComponentSnapshotSet::build(schemas, contributions);
            if (!built_components)
                throw std::runtime_error("snapshot component build failed");
            components = std::move(*built_components);
            registry.storage<Transform3D>().reserve(count);
            for (std::size_t index{}; index < count; ++index)
                registry.emplace<Transform3D>(registry.create());
        }

        Registry registry;
        ComponentSchemaSet schemas;
        ComponentSnapshotSet components;
    };
}

int
main(int argc, char** argv)
{
    const auto options = parseOptions(argc, argv);
    if (!options)
        return 2;

    std::string_view metric;
    std::vector<Sample> samples;
    const bool is_script_binding_group = options->group == "cpp-method-prepared" ||
        options->group == "hook-global-multi" || options->group == "hook-entity-multi" ||
        options->group == "global-event" || options->group == "entity-targeted-event-sparse";
    if (options->group == "world")
    {
        metric = "world_patch";
        samples = measure(
            *options,
            [&] { return std::make_unique<WorldState>(options->size); },
            [](WorldState& state) {
                for (const Entity entity : state.entities)
                {
                    state.registry.patch<Position>(entity, [](Position& value) noexcept { ++value.value; });
                }
                return Observation{.updates = state.entities.size()};
            },
            [](WorldState&) noexcept {}
        );
    }
    else if (options->group == "reactive-dirty")
    {
        metric = "reactive_dirty_patch";
        samples = measure(
            *options,
            [&] { return std::make_unique<ReactiveDirtyState>(options->size); },
            [](ReactiveDirtyState& state) {
                state.dirty.clear();
                for (const Entity entity : state.entities)
                {
                    state.registry.patch<Position>(entity, [](Position& value) noexcept { ++value.value; });
                }
                if (state.dirty.size() != state.entities.size())
                    throw std::runtime_error("dirty notification mismatch");
                return Observation{.updates = state.entities.size(), .notifications = state.dirty.size()};
            },
            [](ReactiveDirtyState&) noexcept {}
        );
    }
    else if (options->group == "command-buffer")
    {
        metric = "command_buffer_record";
        samples = measure(
            *options,
            [&] { return std::make_unique<CommandState>(options->size); },
            [](CommandState& state) {
                {
                    auto begun = state.commands.begin(0U);
                    if (!begun)
                        throw std::runtime_error("command begin failed");
                    auto writer = std::move(*begun);
                    for (const Entity entity : state.entities)
                    {
                        if (!writer.remove<Position>(entity))
                            throw std::runtime_error("command record failed");
                    }
                }
                return Observation{.updates = state.entities.size()};
            },
            [](CommandState& state) noexcept { state.commands.discardPending(); }
        );
    }
    else if (is_script_binding_group)
    {
        metric = options->group == "cpp-method-prepared" ? "cpp_method_prepared"
                 : options->group == "hook-global-multi" ? "hook_global_multi"
                 : options->group == "hook-entity-multi" ? "hook_entity_multi"
                 : options->group == "global-event"      ? "global_event"
                                                         : "entity_targeted_event_sparse";
        samples = measure(
            *options,
            [&] { return std::make_unique<BindingBenchmarkState>(options->size, options->group); },
            [&](BindingBenchmarkState& state) {
                state.callbacks = 0U;
                lux_script_call_frame frame{};
                const auto before = state.session->instrumentation();
                const auto iterations = options->group == "entity-targeted-event-sparse"
                                            ? std::min<std::size_t>(options->size, 100000U)
                                        : options->group == "hook-entity-multi" ? 1U
                                                                                : options->size;
                std::size_t dispatch_calls{};
                if (options->group == "global-event")
                {
                    for (std::size_t index{}; index < iterations; ++index)
                    {
                        dispatch_calls += state.session->dispatchEvent(state.global_event, NullEntity, frame).calls;
                    }
                }
                else if (options->group == "entity-targeted-event-sparse")
                {
                    for (std::size_t index{}; index < iterations; ++index)
                    {
                        dispatch_calls +=
                            state.session
                                ->dispatchEvent(
                                    state.entity_events
                                        [index % state.entities.size() % BindingBenchmarkState::kSparseTargetCount],
                                    state.entities[index % state.entities.size()],
                                    frame
                                )
                                .calls;
                    }
                }
                else if (options->group == "hook-entity-multi")
                {
                    dispatch_calls += state.session->dispatchHook(state.hook, frame).calls;
                }
                else
                {
                    for (std::size_t index{}; index < iterations; ++index)
                    {
                        dispatch_calls += state.session->dispatchHook(state.hook, frame).calls;
                    }
                }
                const auto expected_callbacks = options->group == "hook-global-multi"   ? iterations * 4U
                                                : options->group == "hook-entity-multi" ? state.entities.size()
                                                                                        : iterations;
                if (state.callbacks != expected_callbacks || dispatch_calls != expected_callbacks)
                {
                    throw std::runtime_error("binding callback mismatch");
                }
                const auto& instrumentation = state.session->instrumentation();
                return Observation{
                    .updates = iterations,
                    .notifications =
                        options->group == "global-event" || options->group == "entity-targeted-event-sparse"
                            ? iterations
                            : 0U,
                    .callbacks = state.callbacks,
                    .asset_resolution_delta = instrumentation.asset_resolutions - before.asset_resolutions,
                    .target_resolution_delta = instrumentation.target_resolutions - before.target_resolutions,
                    .entities_examined = instrumentation.entities_examined - before.entities_examined,
                    .target_range_lookups = instrumentation.target_range_lookups - before.target_range_lookups,
                    .handlers_visited = instrumentation.handlers_visited - before.handlers_visited,
                    .target_ranges_built = instrumentation.target_ranges_built,
                    .dispatch_handlers_built = instrumentation.dispatch_handlers_built,
                    .instance_creates = instrumentation.instance_creates,
                    .method_prepares = instrumentation.method_prepares,
                    .frame_builds = instrumentation.frame_builds - before.frame_builds};
            },
            [](BindingBenchmarkState&) noexcept {}
        );
    }
    else if (options->group == "owned-worker-event-buffer")
    {
        metric = "owned_worker_event_buffer";
        samples = measure(
            *options,
            [&] { return std::make_unique<OwnedEventBufferBenchmarkState>(options->size); },
            [&](OwnedEventBufferBenchmarkState& state) {
                state.callbacks = 0U;
                state.producer_failed.store(false);
                state.producers_completed.store(0U);
                if (!state.executor->execute(state.graph) || state.producer_failed.load() ||
                    state.producers_completed.load() != 4U)
                {
                    throw std::runtime_error("worker event production failed");
                }
                if (!state.buffer.drain([&state](Entity, const std::uint64_t&) noexcept { ++state.callbacks; }))
                {
                    throw std::runtime_error("event buffer drain failed");
                }
                if (state.callbacks != options->size)
                    throw std::runtime_error("event buffer callback mismatch");
                return Observation{
                    .updates = options->size,
                    .notifications = options->size,
                    .callbacks = state.callbacks,
                    .frame_builds = options->size};
            },
            [](OwnedEventBufferBenchmarkState&) noexcept {}
        );
    }
    else if (options->group == "task-graph")
    {
        metric = "task_graph_execute";
        samples = measure(
            *options,
            [&] { return std::make_unique<TaskGraphState>(options->size); },
            [](TaskGraphState& state) {
                state.executed.store(0U, std::memory_order_relaxed);
                if (!state.executor->execute(state.graph))
                    throw std::runtime_error("task graph execute failed");
                const auto count = state.executed.load(std::memory_order_relaxed);
                if (count != state.graph.taskCount())
                    throw std::runtime_error("task callback mismatch");
                return Observation{.callbacks = count};
            },
            [](TaskGraphState&) noexcept {}
        );
    }
    else
    {
        metric = "ecs_snapshot_capture";
        samples = measure(
            *options,
            [&] { return std::make_unique<SnapshotState>(options->size); },
            [](SnapshotState& state) {
                const auto snapshot = EcsSnapshot::capture(state.registry, state.components);
                if (!snapshot)
                    throw std::runtime_error("snapshot capture failed");
                return Observation{.updates = state.registry.storage<Entity>().free_list()};
            },
            [](SnapshotState&) noexcept {}
        );
    }

    writeCsv(*options, metric, samples);
    return 0;
}

void*
operator new(std::size_t size)
{
    if (g_count_allocations.load(std::memory_order_relaxed))
        g_allocation_count.fetch_add(1U, std::memory_order_relaxed);
    if (void* result = std::malloc(size == 0U ? 1U : size))
        return result;
    throw std::bad_alloc{};
}

void*
operator new[](std::size_t size)
{
    return ::operator new(size);
}
void
operator delete(void* value) noexcept
{
    std::free(value);
}
void
operator delete[](void* value) noexcept
{
    ::operator delete(value);
}
void
operator delete(void* value, std::size_t) noexcept
{
    std::free(value);
}
void
operator delete[](void* value, std::size_t) noexcept
{
    ::operator delete(value);
}
