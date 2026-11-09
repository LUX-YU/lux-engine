#include <entt/container/dense_set.hpp>
#include <lux/engine/Engine.hpp>
#if defined(LUX_ENGINE_WITH_RENDER)
#include <lux/engine/render/capabilities/Mesh3dCapability.hpp>
#include <lux/engine/render/capabilities/Lighting3dCapability.hpp>
#include <lux/engine/render/extensions/CapabilityRegistry.hpp>
#endif
#include <lux/engine/Init.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/scene/SceneSystem.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationSystem.hpp>
#include <lux/engine/world/World.hpp>
#include <lux/engine/world/WorldSpatialIndex.hpp>
#include <random>
#include <lux/engine/world/WorldObjectId.type_static_info.hpp>
#include <lux/engine/world/ObserverComponent.hpp>
#include <lux/engine/world/LifecyclePolicyComponent.hpp>
#include <lux/engine/ecs/VisibilityComponent.hpp>
#include <lux/engine/ecs/RenderableMeshComponent.hpp>
#include <lux/engine/ecs/VisualSchema.hpp>
#include <lux/engine/ecs/Parent.hpp>
#include <lux/engine/ecs/HierarchySchema.hpp>
#include <lux/engine/scene/WorldMaterializer.hpp>
#include <lux/engine/serialization/Serialization.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <optional>
#include <exec/create.hpp>
#if defined(LUX_ENGINE_WITH_RENDER)
#include <lux/engine/render/commands/Commands.hpp>
#endif

namespace
{
    using namespace lux;
    using namespace std::chrono_literals;
    using Result = engine::Engine::Result;
    constexpr auto Failed = error::errorId("lux.engine.test.scheduling");

    template<class Component, class Predicate>
    bool componentRoundTrip(
        std::span<const ecs::ComponentSchema> schemas, Component value, std::size_t wire_size, Predicate predicate
    ) noexcept
    {
        const auto type = cxx::make_basic_type_info<Component>();
        const auto schema = std::ranges::find(schemas, type, &ecs::ComponentSchema::cpp_type);
        const bool has_codec = schema != schemas.end() && schema->persistent && schema->encode && schema->decode_value;
        if (!has_codec)
        {
            return false;
        }
        ecs::Registry registry;
        const auto entity = registry.create();
        registry.emplace<Component>(entity, value);
        std::vector<std::byte> bytes;
        if (!schema->encode(registry, entity, bytes, {1024, 1024, 16}) || bytes.size() != wire_size)
        {
            return false;
        }
        auto decoded = schema->decode_value(schema->version, bytes, {}, {});
        return decoded && predicate(*static_cast<const Component*>(decoded->data()));
    }

    bool componentSchemas() noexcept
    {
        const auto visual = ecs::visualComponentSchemas();
        const auto world = world::worldComponentSchemas();
        return componentRoundTrip(visual, ecs::VisibilityComponent{false}, 1,
                   [](const auto& value) { return !value.is_visible; }) &&
               componentRoundTrip(visual, ecs::RenderableMeshComponent{true, false}, 2,
                   [](const auto& value) { return value.keep_resources_when_hidden && !value.cast_shadow; }) &&
               componentRoundTrip(world, world::ObserverComponent{37.5}, 8,
                   [](const auto& value) { return value.range == 37.5; }) &&
               componentRoundTrip(world, world::LifecyclePolicyComponent{world::ELifecyclePolicy::MANUAL}, 4,
                   [](const auto& value) { return value.policy == world::ELifecyclePolicy::MANUAL; });
    }

    bool identityReferences() noexcept
    {
        static_assert(std::is_aggregate_v<world::WorldObjectId>);
        static_assert(meta::HasTypeStaticInfo<world::WorldObjectId>);
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = 1;
        const world::WorldObjectId first{uuids::uuid{bytes}};
        bytes.back() = 2;
        const world::WorldObjectId second{uuids::uuid{bytes}};
        world::World authored{{"Author", {}}};
        auto& registry = authored.registry();
        const auto child = registry.create();
        const auto parent = registry.create();
        const auto temporary = registry.create();
        registry.emplace<world::WorldObjectId>(child, first);
        registry.emplace<world::WorldObjectId>(parent, second);
        registry.emplace<ecs::Parent>(child, parent);
        const bool has_author_identities = authored.identities().size() == 2 &&
            authored.identities().entity(second) == parent && !registry.all_of<world::WorldObjectId>(temporary);
        if (!has_author_identities)
        {
            return false;
        }
        const auto schemas = ecs::hierarchyComponentSchemas();
        const auto type = cxx::make_basic_type_info<ecs::Parent>();
        const auto found = std::ranges::find(schemas, type, &ecs::ComponentSchema::cpp_type);
        if (found == schemas.end())
        {
            return false;
        }
        std::vector<std::byte> encoded;
        if (!found->encode(registry, child, encoded, {1024, 1024, 16}) || encoded.size() != 16)
        {
            return false;
        }
        auto captured = found->capture(registry, child, {});
        if (!captured)
        {
            return false;
        }
        auto frozen_bytes = captured->encode(registry, 1024);
        if (!frozen_bytes || *frozen_bytes != encoded)
        {
            return false;
        }

        world::World running{{"Running", {}}};
        auto& live = running.registry();
        const auto generated = live.create(); // Force a different Entity mapping in the loaded world.
        const std::array components{scene::WorldComponentInput{&*found, found->version, encoded}};
        const std::array objects{scene::WorldObjectInput{first, components}, scene::WorldObjectInput{second, {}}};
        auto prepared = scene::WorldMaterializer::prepareObjects(live, running.identities(), objects);
        if (!prepared)
        {
            return false;
        }
        prepared->commit(live, running.identities());
        const auto live_child = running.identities().entity(first);
        const auto live_parent = running.identities().entity(second);
        const bool has_remapped_reference = live_child != child &&
            live.get<ecs::Parent>(live_child).entity == live_parent &&
            live.get<world::WorldObjectId>(live_parent) == second;
        if (!has_remapped_reference)
        {
            return false;
        }
        live.patch<ecs::Parent>(live_child, [generated](auto& value) { value.entity = generated; });
        std::vector<std::byte> saved;
        if (!found->encode(registry, child, saved, {1024, 1024, 16}) || saved != encoded)
        {
            return false; // Runtime changes are not author-save input.
        }
        if (found->encode(live, live_child, saved, {1024, 1024, 16}) || saved != encoded)
        {
            return false; // Explicit encoding of an author reference requires a persistent target.
        }
        registry.destroy(parent);
        if (authored.identities().entity(second) != ecs::NullEntity ||
            found->encode(registry, child, saved, {1024, 1024, 16}))
        {
            return false;
        }
        registry.patch<ecs::Parent>(child, [](auto& value) { value.entity = ecs::NullEntity; });
        if (!found->encode(registry, child, saved, {1024, 1024, 16}) ||
            !std::ranges::all_of(saved, [](auto byte) { return byte == std::byte{}; }))
        {
            return false;
        }
        registry.remove<world::WorldObjectId>(child);
        return registry.valid(child) && authored.identities().size() == 0;
    }

    bool spatialQueries() noexcept
    {
        using Bounds = world::WorldSpatialIndex::Bounds;
        world::World world{world::WorldDescription{"Spatial", {{"fixture", {}}}}};
        auto& index = world.spatialIndex(0);
        if (&std::as_const(world).spatialIndex(0) != &index) return false;
        std::vector<ecs::Entity> entities;
        std::vector<Bounds> bounds;
        for (unsigned i = 0; i < 128; ++i)
        {
            entities.push_back(world.registry().create());
            bounds.push_back({{double(i), -1, -1}, {double(i) + 1, 1, 1}});
        }
        std::vector<ecs::Entity> changed;
        if (!index.updateBatch(entities, bounds, 2, changed) || changed.size() != entities.size()) return false;
        changed.clear();
        Bounds inside{{0.5, -1, -1}, {1.5, 1, 1}};
        if (!index.updateBatch(std::span{entities}.first(1), std::span{&inside, 1}, 2, changed) || !changed.empty())
            return false;
        if (index.actualBounds(entities[0])->minimum.x != 0.5 ||
            index.coverageBounds(entities[0])->minimum.x != -2) return false;
        std::mt19937 random{0x5247};
        std::uniform_real_distribution<double> coordinate{-200, 200};
        const auto overlaps = [](const Bounds& a, const Bounds& b) noexcept
        {
            return a.minimum.x < b.maximum.x && a.maximum.x > b.minimum.x &&
                a.minimum.y < b.maximum.y && a.maximum.y > b.minimum.y &&
                a.minimum.z < b.maximum.z && a.maximum.z > b.minimum.z;
        };
        std::vector<std::size_t> offsets;
        std::vector<ecs::Entity> found;
        for (unsigned round = 0; round < 64; ++round)
        {
            std::vector<ecs::Entity> updates;
            std::vector<Bounds> updated;
            for (unsigned i = 0; i < 16; ++i)
            {
                updates.push_back(entities[(round * 17 + i) % entities.size()]);
                const auto x = coordinate(random), y = coordinate(random), z = coordinate(random);
                updated.push_back({{x, y, z}, {x + 4, y + 4, z + 4}});
            }
            changed.clear();
            if (!index.updateBatch(updates, updated, 2, changed)) return false;
            std::vector<Bounds> regions{{{-1000, -1000, -1000}, {1000, 1000, 1000}}};
            for (unsigned i = 0; i < 12; ++i)
            {
                const auto x = coordinate(random), y = coordinate(random), z = coordinate(random);
                regions.push_back({{x, y, z}, {x + 100, y + 100, z + 100}});
            }
            if (!index.queryBatch(regions, offsets, found) || offsets.size() != regions.size() + 1 || offsets[0])
                return false;
            for (std::size_t query = 0; query < regions.size(); ++query)
            {
                std::vector<ecs::Entity> expected;
                for (auto entity : entities)
                {
                    if (const auto* coverage = index.coverageBounds(entity); coverage && overlaps(*coverage, regions[query]))
                        expected.push_back(entity);
                }
                std::vector<ecs::Entity> actual(found.begin() + offsets[query], found.begin() + offsets[query + 1]);
                std::ranges::sort(expected);
                std::ranges::sort(actual);
                if (actual != expected) return false;
            }
            if (round % 3 == 0)
            {
                const auto entity = entities[round % entities.size()];
                const std::array twice{entity, entity};
                changed.clear();
                index.removeBatch(twice, changed);
                if (changed != std::vector{entity} || index.actualBounds(entity)) return false;
            }
        }
        const auto survivor = entities.back();
        Bounds stable{{0, 0, 0}, {1, 1, 1}};
        if (!index.updateBatch(std::span{&survivor, 1}, std::span{&stable, 1}, 2, changed)) return false;
        const std::array duplicate{survivor, survivor};
        const std::array inputs{inside, inside};
        changed = {survivor};
        if (index.updateBatch(duplicate, inputs, 2, changed) || changed != std::vector{survivor} ||
            index.actualBounds(survivor)->minimum.x != 0) return false;
        Bounds flat{{0, 0, 0}, {1, 1, 0}};
        Bounds edge{{1e308, 0, 0}, {1.5e308, 1, 1}};
        if (index.updateBatch(std::span{&survivor, 1}, std::span{&flat, 1}, 1, changed) ||
            index.updateBatch(std::span{&survivor, 1}, std::span{&edge, 1}, 4e307, changed) ||
            index.updateBatch(std::span{&survivor, 1}, std::span{&stable, 1}, -1, changed)) return false;
        if (changed != std::vector{survivor} || index.actualBounds(survivor)->minimum.x != 0) return false;
        const auto previous_offsets = offsets;
        const auto previous_found = found;
        Bounds invalid{{0, 0, 0}, {1, 1, std::numeric_limits<double>::infinity()}};
        if (index.queryBatch(std::span{&invalid, 1}, offsets, found) || offsets != previous_offsets ||
            found != previous_found) return false;
        const auto old = world.registry().create();
        if (!index.updateBatch(std::span{&old, 1}, std::span{&stable, 1}, 0, changed)) return false;
        index.removeBatch(std::span{&old, 1}, changed);
        world.registry().destroy(old);
        const auto replacement = world.registry().create();
        if (!index.updateBatch(std::span{&replacement, 1}, std::span{&stable, 1}, 0, changed)) return false;
        index.removeBatch(std::span{&old, 1}, changed);
        if (!index.actualBounds(replacement)) return false;
        const Bounds huge{{-1e308, -1e308, -1e308}, {1e308, 1e308, 1e308}};
        if (!index.updateBatch(std::span{&replacement, 1}, std::span{&huge, 1}, 0, changed)) return false;
        changed.clear();
        if (!index.updateBatch(std::span{&replacement, 1}, std::span{&stable, 1}, 0, changed) || changed.size() != 1)
            return false; // Tightening an oversized coverage emits the required relation update.
        return true;
    }

    struct Observations final
    {
        unsigned fixed_fast = 0;
        unsigned fixed_slow = 0;
        unsigned variable = 0;
        bool valid_time = true;
        bool task_finished = false;
        bool destroyed = false;
        scene::ESceneSystemStage stage = scene::ESceneSystemStage::AFTER_SIMULATION;
        std::function<Result()> on_execute;
        std::function<exec::task<Result>()> on_initialize;
    };

    class CountSystem final : public simulation::SimulationSystem
    {
    public:
        inline static unsigned release_calls{};
        inline static unsigned destroyed_count{};
        inline static bool reject_release{};
        simulation::SimulationTime last_time{};

        ~CountSystem() noexcept override
        {
            ++destroyed_count;
        }

        exec::task<Result> releaseResources() noexcept override
        {
            ++release_calls;
            if (!execution::ExecutionRuntime::instance().isMainThread() || reject_release)
            {
                co_return cxx::unexpected(error::Error{Failed});
            }
            co_return Result{};
        }

        CountSystem(
            unsigned& count, bool& valid, simulation::SimulationDuration step = {},
            std::vector<simulation::SimulationDuration>* order = nullptr,
            std::function<void(ecs::EcsCommandWriter&)> commands = {}
        ) noexcept
            : count_(count), valid_(valid), step_(step), order_(order), commands_(std::move(commands))
        {
        }

        simulation::SystemAccessSpec access() const noexcept override
        {
            static const std::array external{
                simulation::SystemExternalAccess{
                    cxx::make_basic_type_info<unsigned>(), simulation::ESystemAccessMode::WRITE
                },
                simulation::SystemExternalAccess{
                    cxx::make_basic_type_info<bool>(), simulation::ESystemAccessMode::WRITE
                },
                simulation::SystemExternalAccess{
                    cxx::make_basic_type_info<std::vector<simulation::SimulationDuration>>(),
                    simulation::ESystemAccessMode::WRITE
                }
            };
            return {{}, external};
        }

        ecs::EcsCommandProducerCapacity commandCapacity() const noexcept override
        {
            return commands_ ? ecs::EcsCommandProducerCapacity{4, 128} : ecs::EcsCommandProducerCapacity{};
        }

        task::ETaskAffinity taskAffinity() const noexcept override
        {
            return step_ == 13ms ? task::ETaskAffinity::WORKER : task::ETaskAffinity::CALLER_THREAD;
        }

        void execute(const simulation::SimulationTime& time) noexcept override
        {
            last_time = time;
            ++count_;
            valid_ &= time.step_index == count_ && time.delta >= simulation::SimulationDuration::zero();
            if (step_ != simulation::SimulationDuration::zero())
            {
                valid_ &= time.delta == step_ && time.elapsed == step_ * count_;
            }
            if (order_)
            {
                order_->push_back(step_);
            }
            if (commands_)
            {
                auto writer = beginCommands();
                valid_ &= writer.has_value();
                if (writer)
                {
                    commands_(*writer);
                }
            }
        }

    private:
        unsigned& count_;
        bool& valid_;
        simulation::SimulationDuration step_;
        std::vector<simulation::SimulationDuration>* order_;
        std::function<void(ecs::EcsCommandWriter&)> commands_;
    };

    class BorrowSystem final : public scene::SceneSystem
    {
    public:
        explicit BorrowSystem(Observations& observations) noexcept : observations_(observations) {}

        ~BorrowSystem() noexcept override
        {
            // No extra scope or destructor wait. Engine must settle the task before destroying this object.
            observations_.valid_time &= !started_ || observations_.task_finished;
            observations_.destroyed = true;
        }

        Result execute() noexcept override
        {
            if (observations_.on_execute)
            {
                return observations_.on_execute();
            }
            if (started_)
            {
                return {};
            }
            started_ = true;
            auto work = stdexec::schedule(execution::ExecutionRuntime::instance().mainScheduler()) |
                        stdexec::let_value([]() noexcept
                                           { return execution::ExecutionRuntime::instance().timer().after(1h); }) |
                        stdexec::upon_error(
                            [this](auto failure) noexcept
                            {
                                if constexpr (std::same_as<decltype(failure), std::exception_ptr>)
                                {
                                    std::terminate();
                                }
                                observations_.valid_time = false;
                            }
                        ) |
                        stdexec::upon_stopped([this]() noexcept { observations_.task_finished = true; });
            auto accepted = taskScope().spawn(std::move(work));
            return accepted ? Result{} : Result{cxx::unexpected(execution::toError(accepted.error()))};
        }

        scene::ESceneSystemStage stage() const noexcept override { return observations_.stage; }

    protected:
        exec::task<Result> initialize() noexcept override
        {
            if (observations_.on_initialize)
            {
                co_return co_await observations_.on_initialize();
            }
            co_return Result{};
        }

    private:
        Observations& observations_;
        bool started_ = false;
    };

    exec::task<Result> admission() noexcept
    {
        co_await execution::ExecutionRuntime::instance().timer().after(40ms);
        co_return Result{};
    }

    Result waitTask(exec::task<Result> task) noexcept
    {
        auto& execution = execution::ExecutionRuntime::instance();
        std::optional<Result> completed;
        auto operation =
            stdexec::starts_on(execution.mainScheduler(), std::move(task)) |
            stdexec::upon_error([](std::exception_ptr) noexcept -> Result { std::terminate(); }) |
            stdexec::upon_stopped([]() noexcept -> Result { return cxx::unexpected(error::Error{Failed}); });
        auto accepted = execution.spawn(stdexec::then(
            std::move(operation),
            [&completed](Result result) noexcept { completed.emplace(std::move(result)); }
        ));
        if (!accepted || !execution.waitUntil([&completed]() noexcept { return completed.has_value(); }))
        {
            return cxx::unexpected(error::Error{Failed});
        }
        return std::move(*completed);
    }

    bool simulationControl(engine::EngineContext& context) noexcept
    {
        Observations values;
        std::vector<simulation::SimulationDuration> order;
        auto simulation = std::make_unique<simulation::Simulation>();
        simulation::TickGroup fast{10ms};
        fast.add(std::make_unique<CountSystem>(values.fixed_fast, values.valid_time, 10ms, &order));
        simulation::TickGroup slow{20ms};
        slow.add(std::make_unique<CountSystem>(values.fixed_slow, values.valid_time, 20ms, &order));
        simulation::TickGroup variable;
        variable.add(std::make_unique<CountSystem>(values.variable, values.valid_time, 0ns, &order));
        simulation->add(std::move(fast));
        simulation->add(std::move(slow));
        simulation->add(std::move(variable));
        auto candidate = std::make_unique<scene::SceneInstance>(
            std::make_unique<world::World>(world::WorldDescription{"Manual", {}}), std::move(simulation)
        );
        engine::Engine engine{context};
        auto id = engine.addScene(candidate, true);
        if (!id || !waitTask(engine.whenSceneInitialized(*id)))
        {
            return false;
        }
        if (engine.advanceSimulation(*id, 0ns) || engine.advanceSimulation(*id, -1ns) || !order.empty())
        {
            return false;
        }
        if (!engine.advanceSimulation(*id, 25ms) ||
            order != std::vector<simulation::SimulationDuration>{10ms, 10ms, 20ms, 0ns})
        {
            return false;
        }
        const auto prior = order;
        if (engine.advanceSimulation(*id, simulation::SimulationDuration::max()) || order != prior)
        {
            return false;
        }
        if (!engine.advanceSimulation(*id, 10ms) || values.fixed_fast != 3 || values.fixed_slow != 1 ||
            values.variable != 2 || !values.valid_time)
        {
            return false;
        }
        if (!engine.setSimulationPaused(*id, false) || engine.advanceSimulation(*id, 1ms) ||
            !engine.setSimulationPaused(*id, true))
        {
            return false;
        }
        return static_cast<bool>(waitTask(engine.whenSceneInitialized(*id)));
    }

    bool commandStages(engine::EngineContext& context) noexcept
    {
        Observations counts;
        Observations before;
        Observations after;
        unsigned before_calls = 0;
        unsigned after_calls = 0;
        std::vector<bool> author_events;
        auto world = std::make_unique<world::World>(world::WorldDescription{"Commands", {}});
        auto& registry = world->registry();
        const auto entity = registry.create();
        registry.emplace<ecs::VisibilityComponent>(entity);
        auto simulation = std::make_unique<simulation::Simulation>();
        simulation::TickGroup group{13ms};
        group.add(std::make_unique<CountSystem>(counts.fixed_fast, counts.valid_time, 13ms, nullptr,
            [entity](ecs::EcsCommandWriter& writer) noexcept
            {
                static_cast<void>(writer.replace(entity, ecs::VisibilityComponent{false}));
                static_cast<void>(writer.replace(entity, ecs::VisibilityComponent{true}));
            }
        ));
        simulation->add(std::move(group));
        before.stage = scene::ESceneSystemStage::BEFORE_SIMULATION;
        before.on_execute = [&]() noexcept -> Result { ++before_calls; return {}; };
        after.on_execute = [&]() noexcept -> Result
        {
            ++after_calls;
            return registry.get<ecs::VisibilityComponent>(entity).is_visible
                ? Result{} : cxx::unexpected(error::Error{Failed});
        };
        auto candidate = std::make_unique<scene::SceneInstance>(std::move(world), std::move(simulation));
        candidate->add(std::make_unique<BorrowSystem>(after)); // Execution stage does not change ownership order.
        candidate->add(std::make_unique<BorrowSystem>(before));
        engine::Engine engine{context};
        entt::dense_set<ecs::Entity> dirty;
        auto observer = [&](ecs::Registry& current, ecs::Entity changed) noexcept
        {
            counts.valid_time &= context.execution().isMainThread();
            author_events.push_back(current.get<ecs::VisibilityComponent>(changed).is_visible);
            dirty.insert(changed);
        };
        entt::scoped_connection connection =
            registry.on_update<ecs::VisibilityComponent>().connect<&decltype(observer)::operator()>(observer);
        auto id = engine.addScene(candidate, true);
        if (!id || !waitTask(engine.whenSceneInitialized(*id)) || !engine.advanceSimulation(*id, 13ms))
        {
            return false;
        }
        const bool commands_adopted = before_calls != 0 && after_calls == 0 && counts.fixed_fast == 1 &&
            counts.valid_time && author_events == std::vector<bool>{false, true} && dirty.size() == 1;
        if (!commands_adopted)
        {
            return false;
        }
        engine.addEngineStageCallback([&]() noexcept -> Result { engine.requestStop(); return {}; },
            engine::EEngineStage::END_UPDATE);
        return engine.run() && after_calls == 1 && counts.fixed_fast == 1;
    }

    bool commandFailure(engine::EngineContext& context) noexcept
    {
        Observations values;
        Observations presentation;
        presentation.on_execute = []() noexcept -> Result { return {}; };
        auto world = std::make_unique<world::World>(world::WorldDescription{"Command failure", {}});
        auto& registry = world->registry();
        auto simulation = std::make_unique<simulation::Simulation>();
        simulation::TickGroup group;
        auto system = std::make_unique<CountSystem>(values.variable, values.valid_time, 0ns, nullptr,
            [](ecs::EcsCommandWriter& writer) noexcept
            {
                // The first command applies; the invalid second target must not roll that effect back.
                static_cast<void>(writer.create());
                writer.destroy(ecs::NullEntity);
            }
        );
        auto* observed = system.get();
        group.add(std::move(system));
        simulation->add(std::move(group));
        auto candidate = std::make_unique<scene::SceneInstance>(std::move(world), std::move(simulation));
        candidate->add(std::make_unique<BorrowSystem>(presentation));
        engine::Engine engine{context};
        auto id = engine.addScene(candidate, true);
        if (!id || !waitTask(engine.whenSceneInitialized(*id)))
        {
            return false;
        }
        const auto first = engine.advanceSimulation(*id, 13ms);
        if (first || first.error().type != error::errorId("lux.scene.ecs_commands") ||
            values.variable != 1 || observed->last_time.elapsed != 13ms)
        {
            return false;
        }
        const auto second = engine.advanceSimulation(*id, 1ms);
        const auto& storage = registry.storage<ecs::Entity>();
        const bool has_preserved_progress = !second && values.variable == 2 && values.valid_time &&
            observed->last_time.elapsed == 14ms &&
            storage.free_list() == 2;
        if (!has_preserved_progress)
        {
            return false;
        }
        ecs::EcsCommandBuffer commands;
        const std::array capacity{ecs::EcsCommandProducerCapacity{1, 0}};
        if (!commands.prepare(capacity))
        {
            return false;
        }
        {
            auto writer = commands.begin(0);
            if (!writer)
            {
                return false;
            }
            static_cast<void>(writer->create());
            if (writer->create().valid())
            {
                return false;
            }
        }
        if (ecs::applyEcsCommands(registry, commands) || storage.free_list() != 2)
        {
            return false; // Recording overflow rejects the whole batch before Registry mutation.
        }
        const std::array next_capacity{ecs::EcsCommandProducerCapacity{4, 128}};
        if (!commands.prepare(next_capacity))
        {
            return false;
        }
        const auto entity = registry.create();
        registry.emplace<ecs::VisibilityComponent>(entity);
        auto observer = [&](ecs::Registry& current, ecs::Entity changed) noexcept
        {
            if (!current.get<ecs::VisibilityComponent>(changed).is_visible)
            {
                auto writer = commands.begin(0);
                values.valid_time &= writer && writer->replace(changed, ecs::VisibilityComponent{true});
            }
        };
        entt::scoped_connection connection =
            registry.on_update<ecs::VisibilityComponent>().connect<&decltype(observer)::operator()>(observer);
        {
            auto writer = commands.begin(0);
            if (!writer || !writer->replace(entity, ecs::VisibilityComponent{false}))
            {
                return false;
            }
        }
        if (!ecs::applyEcsCommands(registry, commands) || registry.get<ecs::VisibilityComponent>(entity).is_visible)
        {
            return false;
        }
        if (!ecs::applyEcsCommands(registry, commands) || !values.valid_time ||
            !registry.get<ecs::VisibilityComponent>(entity).is_visible)
        {
            return false;
        }
        const std::array parallel_capacity{capacity[0], capacity[0]};
        if (!commands.prepare(parallel_capacity))
        {
            return false;
        }
        const auto initial_size = storage.free_list();
        auto record = [&](std::size_t producer) noexcept
        {
            return stdexec::starts_on(context.execution().cpuScheduler(), stdexec::just() |
                stdexec::then([&, producer]() noexcept
                {
                    auto writer = commands.begin(producer);
                    return writer && writer->create().valid();
                })
            );
        };
        auto parallel = [&]() -> exec::task<Result>
        {
            const auto completed = co_await stdexec::continues_on(
                stdexec::when_all(record(0), record(1)), context.execution().mainScheduler()
            );
            co_return std::get<0>(completed) && std::get<1>(completed)
                ? Result{} : cxx::unexpected(error::Error{Failed});
        };
        if (!waitTask(parallel()) || !ecs::applyEcsCommands(registry, commands) ||
            storage.free_list() != initial_size + 2)
        {
            return false;
        }
        bool stage_completed = false;
        engine.addEngineStageCallback(
            [&]() noexcept -> exec::task<Result>
            {
                co_await stdexec::schedule(context.execution().mainScheduler());
                // Produce an invalid structural command while an admitted stage is awaiting Main work.
                observed->execute({15ms, 1ms, 3});
                co_await stdexec::schedule(context.execution().mainScheduler());
                stage_completed = true;
                co_return Result{};
            },
            engine::EEngineStage::BEGIN_UPDATE
        );
        const auto stopped = engine.run();
        return !stopped && stopped.error().type == error::errorId("lux.scene.ecs_commands") && stage_completed;
    }

    bool initializationDelivery(engine::EngineContext& context) noexcept
    {
        Observations bad;
        Observations good;
        bad.on_initialize = []() -> exec::task<Result> { co_return cxx::unexpected(error::Error{Failed}); };
        unsigned good_calls = 0;
        good.on_execute = [&]() noexcept -> Result { ++good_calls; return {}; };
        auto rejected = std::make_unique<scene::SceneInstance>(
            std::make_unique<world::World>(world::WorldDescription{"Rejected", {}}),
            std::make_unique<simulation::Simulation>()
        );
        rejected->add(std::make_unique<BorrowSystem>(bad));
        auto accepted = std::make_unique<scene::SceneInstance>(
            std::make_unique<world::World>(world::WorldDescription{"Ready", {}}),
            std::make_unique<simulation::Simulation>()
        );
        accepted->add(std::make_unique<BorrowSystem>(good));
        engine::Engine engine{context};
        auto bad_id = engine.addScene(rejected);
        auto good_id = engine.addScene(accepted);
        if (!bad_id || !good_id)
        {
            return false;
        }
        auto failed = waitTask(engine.whenSceneInitialized(*bad_id));
        if (failed || failed.error().type != Failed || !waitTask(engine.whenSceneInitialized(*good_id)))
        {
            return false;
        }
        engine.addEngineStageCallback([&]() noexcept -> Result { engine.requestStop(); return {}; },
            engine::EEngineStage::END_UPDATE);
        if (!engine.run() || good_calls != 1)
        {
            return false;
        }
        return static_cast<bool>(waitTask(engine.removeScene(*bad_id)));
    }

    bool initializationRemoval(engine::EngineContext& context) noexcept
    {
        Observations observations;
        std::function<void()> release;
        observations.on_initialize = [&]() -> exec::task<Result>
        {
            co_await exec::create<stdexec::set_value_t()>([&](auto& state) noexcept
            {
                release = [&receiver = state.receiver]() noexcept { stdexec::set_value(std::move(receiver)); };
            });
            co_return Result{};
        };
        auto candidate = std::make_unique<scene::SceneInstance>(
            std::make_unique<world::World>(world::WorldDescription{"Pending", {}}),
            std::make_unique<simulation::Simulation>()
        );
        candidate->add(std::make_unique<BorrowSystem>(observations));
        engine::Engine engine{context};
        auto id = engine.addScene(candidate);
        if (!id)
        {
            return false;
        }
        std::optional<Result> first;
        auto& execution = context.execution();
        auto first_waiter = stdexec::starts_on(execution.mainScheduler(), engine.whenSceneInitialized(*id)) |
            stdexec::upon_error([](std::exception_ptr) noexcept -> Result { std::terminate(); }) |
            stdexec::upon_stopped([]() noexcept -> Result { return cxx::unexpected(error::Error{Failed}); }) |
            stdexec::then([&](Result value) noexcept { first.emplace(std::move(value)); });
        if (!execution.spawn(std::move(first_waiter)))
        {
            return false;
        }
        if (!execution.waitUntil([&]() noexcept { return static_cast<bool>(release); }))
        {
            return false;
        }
        // Let the first waiter register before issuing the second; no arbitrary sleep is required.
        if (!execution.dispatch())
        {
            return false;
        }
        const auto duplicate = waitTask(engine.whenSceneInitialized(*id));
        std::optional<Result> removed;
        auto removal = stdexec::starts_on(execution.mainScheduler(), engine.removeScene(*id)) |
            stdexec::upon_error([](std::exception_ptr) noexcept -> Result { std::terminate(); }) |
            stdexec::upon_stopped([]() noexcept -> Result { return cxx::unexpected(error::Error{Failed}); }) |
            stdexec::then([&](Result value) noexcept { removed.emplace(std::move(value)); });
        if (!execution.spawn(std::move(removal)))
        {
            release();
            return false;
        }
        auto resume = stdexec::schedule(execution.mainScheduler()) | stdexec::then([&]() noexcept { release(); });
        if (!execution.spawn(std::move(resume)))
        {
            release();
            return false;
        }
        if (!execution.waitUntil([&]() noexcept { return first.has_value() && removed.has_value(); }))
        {
            return false;
        }
        return !duplicate && !*first && *removed && observations.destroyed && !observations.task_finished;
    }

    bool removeWhileRunning(engine::EngineContext& context) noexcept
    {
        Observations observations;
        Observations added;
        auto scene = std::make_unique<scene::SceneInstance>(
            std::make_unique<world::World>(world::WorldDescription{"Removal", {}}),
            std::make_unique<simulation::Simulation>()
        );
        scene->add(std::make_unique<BorrowSystem>(observations));
        engine::Engine engine{context};
        auto id = engine.addScene(scene);
        if (!id)
        {
            return false;
        }
        bool requested = false;
        bool removed = false;
        engine.addEngineStageCallback(
            [&]() noexcept -> Result
            {
                if (requested)
                {
                    if (removed && added.variable != 0)
                    {
                        engine.requestStop();
                    }
                    return {};
                }
                requested = true;
                auto simulation = std::make_unique<simulation::Simulation>();
                simulation::TickGroup group;
                group.add(std::make_unique<CountSystem>(added.variable, added.valid_time));
                simulation->add(std::move(group));
                auto next = std::make_unique<scene::SceneInstance>(
                    std::make_unique<world::World>(world::WorldDescription{"Added while running", {}}),
                    std::move(simulation)
                );
                auto added_id = engine.addScene(next);
                if (!added_id)
                {
                    return cxx::unexpected(added_id.error());
                }
                auto operation = stdexec::starts_on(
                                     execution::ExecutionRuntime::instance().mainScheduler(),
                                     engine.removeScene(*id)
                                 ) |
                                 stdexec::upon_error([](std::exception_ptr) noexcept -> Result { std::terminate(); });
                auto accepted = execution::ExecutionRuntime::instance().spawn(stdexec::then(
                    std::move(operation),
                    [&](Result result) noexcept
                    {
                        removed = static_cast<bool>(result);
                        if (!result)
                        {
                            engine.requestStop();
                        }
                    }
                ));
                return accepted ? Result{} : Result{cxx::unexpected(execution::toError(accepted.error()))};
            },
            engine::EEngineStage::END_UPDATE
        );
        return engine.run() && removed && observations.destroyed && observations.task_finished &&
               observations.valid_time && added.variable != 0 && added.valid_time;
    }
} // namespace

int main()
{
    if (!componentSchemas() || !identityReferences() || !spatialQueries())
    {
        std::puts("Component reflection/persistence round trip failed.");
        return 1;
    }
#if defined(LUX_ENGINE_WITH_RENDER)
    {
        render::RenderServerConfig rejected;
        rejected.channel_name = "lux.engine.registration_failure";
        auto failed = engine::init(
            {.cpu_concurrency = 2, .timer = {16}, .blocking = execution::BlockingSchedulerConfig{1, 16}},
            {64U << 20U, 128U << 20U, 16}, std::move(rejected), {}, {}, {},
            [](render::RenderServer& server) noexcept -> render::RenderResult<void>
            {
                const auto& capabilities = server.capabilities();
                const auto mesh = capabilities.find(cxx::make_basic_type_info<render::Mesh3dCapability>());
                const auto lighting = capabilities.find(cxx::make_basic_type_info<render::Lighting3dCapability>());
                if (mesh.isNull() || lighting.isNull() || !capabilities.implementations(mesh).empty())
                {
                    return cxx::unexpected(error::Error{Failed + 1});
                }
                auto registered = server.registerCapability({cxx::make_basic_type_info<Observations>(), {}, {}, {}});
                if (!registered)
                {
                    return cxx::unexpected(registered.error());
                }
                return cxx::unexpected(error::Error{Failed});
            }
        );
        if (failed || failed.error().type != Failed)
        {
            std::fprintf(stderr, "Registration rejection: accepted=%d error=%llu expected=%llu\n",
                static_cast<bool>(failed), failed ? 0ULL : static_cast<unsigned long long>(failed.error().type),
                static_cast<unsigned long long>(Failed));
            return 1;
        }
    }
    bool registration_called = false;
    lux::render::RenderServerConfig rendering;
    rendering.channel_name = "lux.engine.scheduling";
    auto context = lux::engine::init(
        {.cpu_concurrency = 2, .timer = {16}, .blocking = lux::execution::BlockingSchedulerConfig{1, 16}},
        {64U << 20U, 128U << 20U, 16},
        std::move(rendering),
        {.texture_descriptors = true}, {}, {},
        [&](render::RenderServer& server) noexcept -> render::RenderResult<void>
        {
            registration_called = true;
            const auto command = server.commandId<render::CreateScene>();
            if (!command)
            {
                return cxx::unexpected(command.error());
            }
            auto registered = server.registerCapability({cxx::make_basic_type_info<Observations>(), {}, {}, {}});
            if (!registered)
            {
                return cxx::unexpected(registered.error());
            }
            return server.removeCapability(*registered);
        }
    );
#else
    auto context = lux::engine::init(
        {.cpu_concurrency = 2, .timer = {16}, .blocking = lux::execution::BlockingSchedulerConfig{1, 16}},
        {64U << 20U, 128U << 20U, 16}
    );
#endif
    if (!context)
    {
        std::fprintf(stderr, "Context creation failed: %llu\n", static_cast<unsigned long long>(context.error().type));
        return 1;
    }
#if defined(LUX_ENGINE_WITH_RENDER)
    if (!registration_called)
    {
        std::puts("Registration callback was not invoked.");
        return 1;
    }
#endif
    if (!simulationControl(**context) || !commandStages(**context) || !commandFailure(**context) ||
        !initializationDelivery(**context) || !initializationRemoval(**context))
    {
        std::puts("Scene clock, command barrier or initialization delivery failed.");
        return 1;
    }
    Observations observations;
    auto simulation = std::make_unique<lux::simulation::Simulation>();
    lux::simulation::TickGroup fast{8ms};
    fast.add(std::make_unique<CountSystem>(observations.fixed_fast, observations.valid_time, 8ms));
    lux::simulation::TickGroup slow{13ms};
    slow.add(std::make_unique<CountSystem>(observations.fixed_slow, observations.valid_time, 13ms));
    lux::simulation::TickGroup variable;
    variable.add(std::make_unique<CountSystem>(observations.variable, observations.valid_time));
    simulation->add(std::move(fast));
    simulation->add(std::move(slow));
    simulation->add(std::move(variable));
    auto scene = std::make_unique<lux::scene::SceneInstance>(
        std::make_unique<lux::world::World>(lux::world::WorldDescription{"Scheduling", {}}),
        std::move(simulation)
    );
    scene->add(std::make_unique<BorrowSystem>(observations));
    lux::engine::Engine engine{**context};
    auto id = engine.addScene(scene);
    if (!id)
    {
        return 1;
    }
    engine.addEngineStageCallback(&admission, lux::engine::EEngineStage::BEGIN_UPDATE);
    engine.addEngineStageCallback(
        [&]() noexcept -> Result
        {
            if (observations.variable == 3)
            {
                engine.requestStop();
            }
            return {};
        },
        lux::engine::EEngineStage::END_UPDATE
    );
    const auto ran = engine.run();
    const bool retained_scene = !observations.destroyed && observations.task_finished;
    const bool ran_groups = observations.variable == 3 && observations.fixed_fast >= 10 && observations.fixed_slow >= 6;
    if (!ran || !retained_scene || !ran_groups || !observations.valid_time)
    {
        std::fprintf(
            stderr,
            "Scheduling failed: fixed=%u/%u variable=%u settled=%d retained=%d\n",
            observations.fixed_fast,
            observations.fixed_slow,
            observations.variable,
            observations.task_finished,
            !observations.destroyed
        );
        return 1;
    }
    const auto released_before_failure = CountSystem::release_calls;
    const auto destroyed_before_failure = CountSystem::destroyed_count;
    CountSystem::reject_release = true;
    const auto rejected = waitTask(engine.removeScene(*id));
    CountSystem::reject_release = false;
    const bool retirement_failure_lost = rejected || rejected.error().type != Failed || !observations.destroyed ||
                                         CountSystem::destroyed_count != destroyed_before_failure ||
                                         CountSystem::release_calls != released_before_failure + 1;
    if (retirement_failure_lost)
    {
        return 1;
    }
    if (!waitTask(engine.removeScene(*id)) || !observations.destroyed || !observations.valid_time ||
        CountSystem::destroyed_count != destroyed_before_failure + 3 || waitTask(engine.removeScene(*id)))
    {
        return 1;
    }
    if (!removeWhileRunning(**context))
    {
        return 1;
    }
    Observations automatic;
    {
        auto assembled = std::make_unique<lux::scene::SceneInstance>(
            std::make_unique<lux::world::World>(lux::world::WorldDescription{"RAII", {}}),
            std::make_unique<lux::simulation::Simulation>()
        );
        assembled->add(std::make_unique<BorrowSystem>(automatic));
        lux::engine::Engine owned{**context};
        if (!owned.addScene(assembled))
        {
            return 1;
        }
        owned.addEngineStageCallback(
            [&owned]() noexcept -> Result
            {
                owned.requestStop();
                return {};
            },
            lux::engine::EEngineStage::END_UPDATE
        );
        const auto completed = owned.run();
        const bool invalid_shutdown = !completed || automatic.destroyed || !automatic.task_finished;
        if (invalid_shutdown)
        {
            return 1;
        }
        // No explicit retire/remove: the outer Engine destructor owns final resource retirement.
    }
    if (!automatic.destroyed || !automatic.valid_time)
    {
        return 1;
    }
    Observations cold;
    {
        auto assembled = std::make_unique<lux::scene::SceneInstance>(
            std::make_unique<lux::world::World>(lux::world::WorldDescription{"Never run", {}}),
            std::make_unique<lux::simulation::Simulation>()
        );
        assembled->add(std::make_unique<BorrowSystem>(cold));
        lux::engine::Engine owned{**context};
        if (!owned.addScene(assembled))
        {
            return 1;
        }
    }
    if (!cold.destroyed || !cold.valid_time || cold.task_finished)
    {
        return 1;
    }
    std::puts("Independent fixed groups, variable rounds, task drain, retained Scene and async removal passed.");
    return 0;
}
