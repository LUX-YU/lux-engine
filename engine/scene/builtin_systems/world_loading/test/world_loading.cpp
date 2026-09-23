#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/process/world_loading/WorldPartitionLoadSender.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/simulation/ecs/HierarchySchema.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/WorldStorageCodec.hpp>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>

using namespace lux;
namespace ecs = simulation::ecs;
namespace loading = process::world_loading;
using namespace std::chrono_literals;

namespace
{
template <class T> T identity(std::uint8_t tail)
{
    std::array<std::uint8_t, 16> bytes{};
    bytes.back() = tail;
    return T{uuids::uuid{bytes}};
}

struct Fixture final
{
    std::shared_ptr<const world::WorldDescription> world;
    std::filesystem::path path;
    std::array<world::WorldPartitionData, 2> partitions;
};

Fixture fixture(const std::filesystem::path &path, const ecs::ComponentSchemaSet &schemas, std::uint8_t generation = 1,
                bool bad = false, bool unknown = false)
{
    using namespace world;
    ecs::Registry registry;
    ecs::WorldEntityMap identities;
    std::array<ecs::Entity, 2> entities{registry.create(), registry.create()};
    for (std::size_t index{}; index < entities.size(); ++index)
    {
        assert(identities.bind(identity<WorldObjectId>(static_cast<std::uint8_t>(index + 1)), entities[index]));
        registry.emplace<ecs::Transform3D>(entities[index]).translation.x() = generation * 10 + index;
    }
    registry.emplace<ecs::Parent>(entities[0], entities[1]);
    std::vector<WorldDataSchemaId> ids{worldDataSchemaId("lux.ecs.Parent"), worldDataSchemaId("lux.ecs.Transform3D")};
    if (unknown)
    {
        ids.push_back(worldDataSchemaId("test.Opaque"));
    }
    std::ranges::sort(ids, WorldDataSchemaIdLess{});
    std::array<std::vector<std::vector<std::byte>>, 2> owned;
    std::array<std::vector<WorldEncodedDataRecord>, 2> data;
    std::array<std::vector<std::byte>, 2> bytes;
    for (std::size_t index{}; index < entities.size(); ++index)
    {
        owned[index].reserve(ids.size());
        for (std::size_t ordinal{}; ordinal < ids.size(); ++ordinal)
        {
            const auto *schema = schemas.find(ecs::componentSchemaId(ids[ordinal].name));
            if (schema && !schema->operations.has(registry, entities[index]))
            {
                continue;
            }
            if (schema)
            {
                auto capture = schema->capture(registry, entities[index], schema->code_lifetime);
                assert(capture);
                auto value = capture->encode(identities, 4096);
                assert(value);
                owned[index].push_back(std::move(*value));
            }
            else
            {
                owned[index].push_back({std::byte{91}, std::byte{37}});
            }
            data[index].push_back({static_cast<std::uint32_t>(ordinal),
                                   static_cast<std::uint32_t>(bad && index == 1 ? 99 : 1), owned[index].back()});
        }
        const WorldEncodedObjectRecord object{identities.object(entities[index]), data[index]};
        auto encoded = encodeWorldPartitionData({static_cast<std::uint32_t>(index)}, std::span(&object, 1));
        assert(encoded);
        bytes[index] = std::move(*encoded);
    }
    const auto bundle = identity<WorldBundleId>(20);
    const auto version = identity<WorldBundleGeneration>(generation);
    std::array records{WorldPartitionRecord{identity<WorldPartitionId>(30), 0, 1},
                       WorldPartitionRecord{identity<WorldPartitionId>(31), 1, 1}};
    std::array extents{WorldPartitionExtent{0, 1, 1}, WorldPartitionExtent{0, 2, 1}};
    auto table = encodeWorldPartitionTablePage({0}, records, extents);
    assert(table);
    const std::array chunks{
        WorldStorageChunkInput{EWorldStorageChunkKind::PARTITION_TABLE_PAGE, EWorldStorageCodec::NONE, *table},
        WorldStorageChunkInput{EWorldStorageChunkKind::WORLD_PARTITION_DATA, EWorldStorageCodec::NONE, bytes[0]},
        WorldStorageChunkInput{EWorldStorageChunkKind::WORLD_PARTITION_DATA, EWorldStorageCodec::NONE, bytes[1]}};
    auto volume = encodeWorldStorageVolume(bundle, version, 0, chunks);
    assert(volume);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(volume->data()), volume->size());
    output.close();
    assert(output.good());
    WorldDescriptionBuilder builder;
    assert(builder.setIdentity(bundle, version, "Observer fixture"));
    for (const auto &id : ids)
    {
        assert(builder.addSchema(id));
    }
    assert(builder.setPartitioner({worldPartitionerId("test.explicit"), 1}, 2));
    assert(builder.addStorageVolume({path.filename().string(), 1, 3, volume->size()}));
    assert(builder.addPartitionTablePage({{0}, 2, {0, 0}}));
    auto description = std::move(builder).build();
    assert(description);
    Fixture result{std::make_shared<const WorldDescription>(std::move(*description)), path};
    for (std::size_t index{}; index < bytes.size(); ++index)
    {
        auto decoded = decodeWorldPartitionData(bytes[index], bundle, version, {static_cast<std::uint32_t>(index)},
                                                records[index].id, ids.size(), 4096);
        assert(decoded);
        result.partitions[index] = std::move(*decoded);
    }
    return result;
}

class Disk final : public async::OperationPort<loading::ReadWorldStorageRange>::Endpoint,
                   public std::enable_shared_from_this<Disk>
{
    using Range = loading::ReadWorldStorageRange;
    using Outcome = async::OperationOutcome<Range>;
    using Failure = async::OperationFailure<loading::WorldStorageRuntimeFailure>;
    struct Request final
    {
        Range range;
        void *state;
        void (*complete)(void *, Outcome &&) noexcept;
    };
    std::filesystem::path path_;
    process::BlockingScheduler blocking_;
    process::TaskScope &tasks_;
    std::mutex mutex_;
    std::vector<Request> pending_;
    bool hold_{};
    std::atomic<std::size_t> completed_{};

    void start(Request request)
    {
        auto self = shared_from_this();
        auto work = stdexec::then(stdexec::schedule(blocking_), [self, request]() noexcept {
            const auto &range = request.range;
            std::ifstream file(self->path_, std::ios::binary);
            auto bytes = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(range.size));
            file.seekg(range.offset);
            if (range.volume || !file.read(reinterpret_cast<char *>(bytes->data()), bytes->size()))
            {
                request.complete(request.state,
                                 cxx::unexpected(Failure::domain(
                                     {loading::EWorldStorageRuntimeError::IO_FAILURE, range.volume, range.offset})));
            }
            else
            {
                request.complete(request.state, cxx::SharedBytes<>::fromOwner(bytes, *bytes));
            }
            ++self->completed_;
        });
        auto error = stdexec::upon_error(std::move(work), [request](process::EExecutionError) noexcept {
            request.complete(request.state, cxx::unexpected(Failure::runtime(async::ESubmitError::STOPPING)));
        });
        auto task = stdexec::upon_stopped(std::move(error), [request]() noexcept {
            request.complete(request.state, cxx::unexpected(Failure::runtime(async::ESubmitError::STOPPING)));
        });
        const auto accepted = tasks_.start(std::move(task));
        assert(accepted);
    }

  public:
    Disk(std::filesystem::path path, process::BlockingScheduler scheduler, process::TaskScope &tasks)
        : path_(std::move(path)), blocking_(scheduler), tasks_(tasks)
    {
    }
    async::SubmitResult submit(Range range, void *state, void (*complete)(void *, Outcome &&) noexcept,
                               async::SubmitOptions) noexcept override
    {
        {
            std::lock_guard lock(mutex_);
            if (hold_)
            {
                pending_.push_back({range, state, complete});
                return {};
            }
        }
        start({range, state, complete});
        return {};
    }
    void hold()
    {
        std::lock_guard lock(mutex_);
        hold_ = true;
    }
    void release()
    {
        std::vector<Request> pending;
        {
            std::lock_guard lock(mutex_);
            hold_ = false;
            pending.swap(pending_);
        }
        for (const auto &request : pending)
        {
            start(request);
        }
    }
    std::size_t completed() const noexcept
    {
        return completed_.load();
    }
};

struct Constructed final
{
    std::size_t count{};
    void event(ecs::Registry &, ecs::Entity)
    {
        ++count;
    }
};
void materialization(const Fixture &valid, const Fixture &bad, const ecs::ComponentSchemaSet &schemas)
{
    auto materializer = scene::WorldMaterializer::create(valid.world, schemas);
    assert(materializer);
    ecs::Registry registry;
    ecs::WorldEntityMap identities;
    Constructed events;
    entt::scoped_connection connection = registry.on_construct<ecs::Transform3D>().connect<&Constructed::event>(events);
    std::vector<ecs::Entity> output{ecs::NullEntity};
    auto missing = materializer->partition(registry, identities, valid.partitions[0], &output);
    assert(!missing && missing.error().component.code == ecs::EComponentDecodeError::UNRESOLVED_REFERENCE);
    assert(missing.error().component.reference == identity<world::WorldObjectId>(2));
    assert(events.count == 0 && identities.size() == 0 && output == std::vector{ecs::NullEntity});
    std::array invalid{bad.partitions[0].objectAt(0), bad.partitions[1].objectAt(0)};
    auto failure = materializer->objects(registry, identities, invalid, &output);
    assert(!failure && failure.error().object == 1 &&
           failure.error().component.code == ecs::EComponentDecodeError::UNSUPPORTED_VERSION);
    assert(events.count == 0 && identities.size() == 0 && output == std::vector{ecs::NullEntity});
    std::array input{valid.partitions[0].objectAt(0), valid.partitions[1].objectAt(0)};
    auto capacity = materializer->objects(registry, identities, input, &output, 1);
    assert(!capacity && capacity.error().code == scene::EWorldMaterializeError::CAPACITY);
    assert(events.count == 0 && identities.size() == 0);
    std::size_t bytes{};
    assert(materializer->objects(registry, identities, input, &output, 4096, &bytes));
    assert(output.size() == 2 && events.count == 2 && identities.size() == 2);
    assert(bytes == 2 * sizeof(ecs::Transform3D) + sizeof(ecs::Parent));
    assert(registry.get<ecs::Parent>(output[0]).entity == output[1]);
    auto duplicate = materializer->objects(registry, identities, input);
    assert(!duplicate && duplicate.error().code == scene::EWorldMaterializeError::DUPLICATE_OBJECT);
    assert(events.count == 2);
    std::vector<ecs::Entity> references;
    const auto *parent = schemas.find(cxx::typeToken<ecs::Parent>());
    assert(parent->visit_references(registry, output[0], {&references, +[](void *state, ecs::Entity entity) noexcept {
                                                              static_cast<std::vector<ecs::Entity> *>(state)->push_back(
                                                                  entity);
                                                          }}));
    assert(references == std::vector{output[1]});
    std::cout << "PASS materialization: missing cross-partition reference and late decode failure before any Registry "
                 "event; retry, inline budget, typed reference traversal\n";
}

void observerCodec(const ecs::ComponentSchemaSet &schemas)
{
    ecs::Registry registry;
    ecs::WorldEntityMap identities;
    const auto source = registry.create();
    const auto destination = registry.create();
    registry.emplace<scene::Observer>(source, std::vector<partition::PartitionOrdinal>{{0}, {7}, {19}}, false);
    const auto *schema = schemas.find(cxx::typeToken<scene::Observer>());
    assert(schema && schema->capture && schema->decode_value && schema->visit_references);
    auto captured = schema->capture(registry, source, {});
    assert(captured);
    auto bytes = captured->encode(identities, 4096);
    assert(bytes);
    assert(schema->decode_emplace(registry, identities, destination, 1, *bytes));
    const auto &decoded = registry.get<scene::Observer>(destination);
    assert(decoded.partitions == registry.get<scene::Observer>(source).partitions && !decoded.required);
    auto truncated = *bytes;
    truncated.pop_back();
    assert(!schema->decode_emplace(registry, identities, destination, 1, truncated));
    assert(decoded.partitions.size() == 3 && !decoded.required);
    assert(schema->visit_references(registry, destination, {nullptr, +[](void *, ecs::Entity) noexcept {
                                                                assert(false &&
                                                                       "partition ordinals are not Entity references");
                                                            }}));
}

template <class Poll, class Ready> void until(Poll poll, Ready ready)
{
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < end)
    {
        poll();
        if (ready())
        {
            return;
        }
        std::this_thread::sleep_for(1ms);
    }
    assert(false && "world loading did not reach the specified state");
}

void measure(const scene::SceneCreateInfo &info, task::TaskExecutor &executor, const Disk &disk)
{
    using Clock = std::chrono::steady_clock;
    auto created = scene::SceneInstance::create(info);
    assert(created);
    auto instance = std::move(*created);
    assert(instance->simulation().seal());
    scene::SceneDriver driver(executor);
    auto &loader = *instance->findSceneSystem<scene::WorldLoadingSystem>();
    auto &registry = instance->registry();
    constexpr unsigned warmup = 4, cycles = 20;
    std::chrono::nanoseconds active{}, idle{}, waiting{}, longest{};
    std::size_t turns{}, peak_staged{}, peak_read{}, peak_source{}, peak_components{};
    std::uint64_t checksum{};
    scene::WorldLoadingStatistics start;
    std::size_t disk_start{};
    for (unsigned cycle = 0; cycle < warmup + cycles; ++cycle)
    {
        const bool measured = cycle >= warmup;
        if (cycle == warmup)
        {
            start = loader.statistics();
            disk_start = disk.completed();
        }
        auto poll = [&](bool static_demand = false) {
            scene::SceneAdvanceBudget budget{32, 1, 0};
            const auto begin = Clock::now();
            static_cast<void>(driver.advance(*instance, begin, budget));
            assert(instance->progress().result);
            const auto elapsed = Clock::now() - begin;
            if (measured)
            {
                (static_demand ? idle : active) += elapsed;
                longest = std::max(longest, std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed));
                ++turns;
                const auto stats = loader.statistics();
                peak_staged = std::max(peak_staged, stats.staged_bytes);
                peak_read = std::max(peak_read, stats.reserved_read_bytes);
                peak_source = std::max(peak_source, stats.resident_source_bytes);
                peak_components = std::max(peak_components, stats.resident_component_bytes);
            }
        };
        auto wait = [&](std::size_t residents) {
            const auto deadline = Clock::now() + 5s;
            do
            {
                poll();
                if (loader.statistics().resident_partitions == residents)
                {
                    return;
                }
                const auto begin = Clock::now();
                std::this_thread::sleep_for(1ms);
                if (measured)
                {
                    waiting += Clock::now() - begin;
                }
            } while (Clock::now() < deadline);
            assert(false && "cost run failed to complete real IO/materialization/unload");
        };
        const auto first = registry.create(), overlap = registry.create();
        registry.emplace<scene::Observer>(first, std::vector<partition::PartitionOrdinal>{{0}, {1}}, true);
        registry.emplace<scene::Observer>(overlap, std::vector<partition::PartitionOrdinal>{{1}}, true);
        wait(2);
        const auto reads = loader.statistics().reads;
        assert(loader.status());
        for (unsigned frame = 0; frame < 20; ++frame)
        {
            poll(true);
        }
        assert(loader.statistics().reads == reads);
        if (measured)
        {
            const auto a = loader.identities().entity(identity<world::WorldObjectId>(1));
            const auto b = loader.identities().entity(identity<world::WorldObjectId>(2));
            assert(registry.get<ecs::Parent>(a).entity == b);
            checksum += static_cast<std::uint64_t>(registry.get<ecs::Transform3D>(a).translation.x() +
                                                   registry.get<ecs::Transform3D>(b).translation.x());
        }
        registry.destroy(first);
        registry.destroy(overlap);
        wait(0);
        assert(loader.identities().size() == 0);
    }
    const auto final = loader.statistics();
    assert(final.reads - start.reads == cycles * 2 && final.adopted - start.adopted == cycles * 2);
    assert(final.unloaded - start.unloaded == cycles * 2 && final.rejected == start.rejected);
    assert(final.resident_component_bytes == 0 && final.resident_source_bytes == 0 && checksum == 420);
    const auto us = [](auto time) { return std::chrono::duration<double, std::micro>(time).count(); };
    std::cout << "MEASURE Observer warmup_cycles=" << warmup << " cycles=" << cycles
              << " observers=2 partitions=2 entities=2 idle_turns=" << cycles * 20
              << " reads=" << final.reads - start.reads << " disk_ranges=" << disk.completed() - disk_start
              << " adopted=" << final.adopted - start.adopted << " rejected=" << final.rejected - start.rejected
              << " unloaded=" << final.unloaded - start.unloaded << " checksum=" << checksum
              << " active_us=" << us(active) << " idle_us=" << us(idle) << " wait_us=" << us(waiting)
              << " longest_advance_us=" << us(longest) << " turns=" << turns
              << " sampled_peak_staged_bytes=" << peak_staged << " sampled_peak_reserved_read_bytes=" << peak_read
              << " resident_source_bytes=" << peak_source << " resident_component_bytes=" << peak_components << '\n';
    const auto stopped = Clock::now();
    instance.reset();
    std::cout << "MEASURE Observer cpu_exit_us=" << us(Clock::now() - stopped) << '\n';
}

} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2 || (argc == 3 && std::string_view(argv[2]) == "--cost"));
    std::vector<ecs::ComponentSchema> values;
    for (const auto schemas :
         {ecs::transformComponentSchemas(), ecs::hierarchyComponentSchemas(), scene::worldLoadingComponentSchemas()})
    {
        values.insert(values.end(), schemas.begin(), schemas.end());
    }
    auto schemas = ecs::ComponentSchemaSet::build(std::move(values));
    assert(schemas);
    observerCodec(*schemas);
    const std::filesystem::path root{argv[1]};
    auto valid = fixture(root / "valid.wvol", *schemas);
    auto bad = fixture(root / "bad.wvol", *schemas, 1, true);
    auto next = fixture(root / "next.wvol", *schemas, 2);
    auto opaque = fixture(root / "opaque.wvol", *schemas, 1, false, true);
    materialization(valid, bad, *schemas);

    auto runtime = process::ExecutionRuntime::create({2, 64, 64, {64}, process::BlockingSchedulerConfig{2, 64}});
    assert(runtime && runtime->blocking());
    process::TaskScope tasks;
    auto disk = std::make_shared<Disk>(valid.path, *runtime->blocking(), tasks);
    auto source =
        loading::WorldStorageSource::create(valid.world, async::OperationPort<loading::ReadWorldStorageRange>{disk});
    assert(source);
    auto executor = task::TaskExecutor::create({0, 1024});
    assert(executor);
    scene::SceneDriver driver(*executor);
    const lux::simulation::ecs::ComponentSchemaSet task_components{*schemas};
    const lux::simulation::SimulationSystemRegistry task_system_types;
    const std::array task_scene_systems{scene::worldLoadingSystemRegistration()};
    scene::SceneDescriptionBuilder builder;
    const auto registration = scene::worldLoadingSystemRegistration();
    std::vector<std::byte> configuration;
    assert(registration.configuration.encode_default(configuration));
    assert(builder.addSystem({1}, "loading", registration.type, 1, registration.description->configuration_schema_name,
                             1, configuration));
    auto description = std::move(builder).buildResolved();
    assert(description);
    auto shared = std::make_shared<const scene::SceneDescription>(std::move(*description));
    scene::WorldLoadingServices services{*source, tasks};
    auto provider =
        scene::makeSceneCapabilityProvider<scene::WorldLoadingServices>("loading", "lux.world.loading", services);
    scene::SceneCreateInfo info{shared, valid.world, std::make_shared<const simulation::SimulationDescription>(),
                                task_components, task_system_types, task_scene_systems, std::span(&provider, 1)};
    if (argc == 3)
    {
        measure(info, *executor, *disk);
        assert(stdexec::sync_wait(tasks.close()));
        runtime->requestStop();
        assert(runtime->join());
        return 0;
    }
    auto opened = scene::SceneInstance::create(info);
    assert(opened);
    auto instance = std::move(*opened);
    assert(instance->simulation().seal());
    auto &system = *instance->findSceneSystem<scene::WorldLoadingSystem>();
    auto &registry = instance->registry();
    const auto observer = registry.create();
    registry.emplace<scene::Observer>(observer, std::vector<partition::PartitionOrdinal>{{0}, {1}}, true);
    const auto second_observer = registry.create();
    registry.emplace<scene::Observer>(second_observer, std::vector<partition::PartitionOrdinal>{{1}}, true);
    auto poll = [&] {
        scene::SceneAdvanceBudget budget{32, 1, 0};
        static_cast<void>(driver.advance(*instance, std::chrono::steady_clock::now(), budget));
    };
    until(poll, [&] { return system.statistics().resident_partitions == 2; });
    assert(system.status() && system.statistics().reads == 2);
    assert(system.statistics().resident_entities == 2 && instance->progress().clock.step_index == 0);
    const auto reads = disk->completed();
    for (int turn{}; turn < 10; ++turn)
    {
        poll();
    }
    assert(disk->completed() == reads);
    const auto a = system.identities().entity(identity<world::WorldObjectId>(1));
    const auto b = system.identities().entity(identity<world::WorldObjectId>(2));
    assert(registry.get<ecs::Parent>(a).entity == b);
    assert(system.statistics().resident_component_bytes == 2 * sizeof(ecs::Transform3D) + sizeof(ecs::Parent));
    const auto duplicate_membership = system.replaceEntities({0}, {a, a});
    assert(!duplicate_membership &&
           duplicate_membership.error().code == scene::EWorldLoadingError::INVALID_CONFIGURATION);
    const auto foreign_membership = system.replaceEntities({0}, {a, b});
    assert(!foreign_membership && foreign_membership.error().code == scene::EWorldLoadingError::INVALID_CONFIGURATION);
    assert(!system.validateAdditional(services.limits.entities, 0));
    assert(system.statistics().resident_entities == 2);
    registry.destroy(second_observer);
    registry.patch<scene::Observer>(observer, [](auto &value) { value.partitions = {{0}}; });
    poll();
    assert(system.statistics().resident_partitions == 2);
    const auto rows = system.resident();
    assert(rows[1].retention == scene::EPartitionRetention::REFERENCE);
    registry.remove<ecs::Parent>(a);
    poll();
    assert(system.statistics().resident_partitions == 1 && !registry.valid(b));
    assert(system.statistics().resident_component_bytes == sizeof(ecs::Transform3D));
    {
        auto protection = system.retain({0});
        assert(protection && system.setDirty({0}, true));
        registry.patch<scene::Observer>(observer, [](auto &value) { value.partitions.clear(); });
        poll();
        assert(registry.valid(a) && system.resident()[0].retention == scene::EPartitionRetention::DIRTY);
        assert(system.setDirty({0}, false));
        poll();
        assert(registry.valid(a) && system.resident()[0].retention == scene::EPartitionRetention::EXTERNAL);
    }
    poll();
    assert(!registry.valid(a) && system.statistics().resident_partitions == 0);
    assert(system.statistics().resident_source_bytes == 0 && system.statistics().resident_component_bytes == 0);
    instance.reset();
    std::cout << "PASS Observer: actual Process blocking IO, overlapping demand dedup, no static reread, "
                 "cross-partition retention, dirty/external lifetime, unload\n";

    // A required-set failure must reach the unique Driver without advancing the
    // Simulation clock or turning an unresolved reference into an endless wait.
    {
        auto opened = scene::SceneInstance::create(info);
        assert(opened);
        auto failed_instance = std::move(*opened);
        assert(failed_instance->simulation().seal());
        auto &registry = failed_instance->registry();
        registry.emplace<scene::Observer>(registry.create(), std::vector<partition::PartitionOrdinal>{{0}}, true);
        assert(driver.play(*failed_instance));
        until(
            [&] {
                scene::SceneAdvanceBudget budget{32, 1, 0};
                static_cast<void>(driver.advance(*failed_instance, std::chrono::steady_clock::now(), budget));
            },
            [&] { return !failed_instance->progress().result; });
        const auto &final = failed_instance->progress();
        assert(final.clock.step_index == 0 && final.result.error().phase == scene::ESceneDrivePhase::MAINTENANCE);
        const auto &stage = std::get<scene::SceneExecutionFailure>(final.result.error().cause);
        const auto *cause = std::any_cast<scene::WorldLoadingFailure>(&stage.cause);
        assert(stage.system.value == 1 && cause && cause->code == scene::EWorldLoadingError::MATERIALIZE_FAILURE);
        const auto &component = std::get<scene::WorldMaterializeFailure>(cause->cause).component;
        assert(component.code == ecs::EComponentDecodeError::UNRESOLVED_REFERENCE &&
               component.reference == identity<world::WorldObjectId>(2));
        const auto *loading = failed_instance->findSceneSystem<scene::WorldLoadingSystem>();
        assert(loading->statistics().resident_entities == 0 && loading->identities().size() == 0);
        std::cout << "PASS required partition failure: original missing object, Main Driver MAINTENANCE, clock=0\n";
    }

    // Complete-set capacity failure must publish no partial Entity set, then recover.
    {
        ecs::Registry registry;
        services.limits.partitions = 1;
        scene::WorldLoadingSystem loader(registry, *schemas, services);
        const auto observer = registry.create();
        registry.emplace<scene::Observer>(observer, std::vector<partition::PartitionOrdinal>{{0}, {1}}, true);
        auto poll = [&] {
            std::size_t publications{};
            std::size_t resource_steps{};
            scene::SceneStageContext context{publications, resource_steps};
            assert(loader.maintain(context));
        };
        until(poll, [&] { return !loader.status(); });
        assert(loader.status().error().code == scene::EWorldLoadingError::CAPACITY);
        assert(loader.identities().size() == 0 && loader.statistics().resident_partitions == 0);
        registry.patch<scene::Observer>(observer, [](auto &value) { value.partitions = {{1}}; });
        until(poll, [&] { return loader.statistics().resident_partitions == 1; });
        assert(loader.status());
    }
    services.limits.partitions = 64;

    // Destroy the loading system with genuinely admitted IO outstanding. The
    // callbacks complete later without borrowing the system or its Registry.
    disk->hold();
    {
        ecs::Registry registry;
        scene::WorldLoadingSystem loader(registry, *schemas, services);
        registry.emplace<scene::Observer>(registry.create(), std::vector<partition::PartitionOrdinal>{{1}}, true);
        std::size_t publications{};
        std::size_t resource_steps{};
        scene::SceneStageContext context{publications, resource_steps};
        assert(loader.maintain(context) == scene::ESceneProgress::PENDING);
        assert(loader.statistics().in_flight == 1);
    }
    disk->release();

    // Replacement source while reads are pending cannot adopt stale generation.
    auto next_disk = std::make_shared<Disk>(next.path, *runtime->blocking(), tasks);
    auto next_source = loading::WorldStorageSource::create(
        next.world, async::OperationPort<loading::ReadWorldStorageRange>{next_disk});
    assert(next_source);
    disk->hold();
    {
        ecs::Registry registry;
        scene::WorldLoadingSystem loader(registry, *schemas, services);
        registry.emplace<scene::Observer>(registry.create(), std::vector<partition::PartitionOrdinal>{{1}}, true);
        auto poll = [&] {
            std::size_t publications{};
            std::size_t resource_steps{};
            scene::SceneStageContext context{publications, resource_steps};
            assert(loader.maintain(context));
        };
        poll();
        assert(loader.replaceSource(*next_source));
        disk->release();
        until(poll, [&] { return loader.statistics().resident_partitions == 1; });
        const auto entity = loader.identities().entity(identity<world::WorldObjectId>(2));
        assert(registry.get<ecs::Transform3D>(entity).translation.x() == 21);
        assert(loader.source({1})->generation() == next.world->generation());
    }
    std::cout << "PASS Observer: complete-set capacity/recovery, destructor before IO completion, source generation "
                 "replacement\n";

    // Real IO and codec errors retain their original identity. A changed
    // source retries the same explicit set without publishing failed contents.
    {
        auto reader = std::make_shared<Disk>(bad.path, *runtime->blocking(), tasks);
        auto input = loading::WorldStorageSource::create(bad.world,
                                                         async::OperationPort<loading::ReadWorldStorageRange>{reader});
        assert(input);
        ecs::Registry registry;
        scene::WorldLoadingSystem loader(registry, *schemas, {*input, tasks});
        registry.emplace<scene::Observer>(registry.create(), std::vector<partition::PartitionOrdinal>{{0}, {1}}, true);
        Constructed events;
        entt::scoped_connection connection =
            registry.on_construct<ecs::Transform3D>().connect<&Constructed::event>(events);
        auto poll = [&] {
            std::size_t publications{};
            std::size_t resource_steps{};
            scene::SceneStageContext context{publications, resource_steps};
            const auto progressed = loader.maintain(context);
            if (!progressed)
            {
                const auto *failure = std::any_cast<scene::WorldLoadingFailure>(&progressed.error().cause);
                assert(failure && !loader.status() && failure->code == loader.status().error().code);
            }
        };
        until(poll, [&] { return !loader.status(); });
        assert(loader.status().error().code == scene::EWorldLoadingError::MATERIALIZE_FAILURE);
        const auto &error = std::get<scene::WorldMaterializeFailure>(loader.status().error().cause);
        assert(error.component.code == ecs::EComponentDecodeError::UNSUPPORTED_VERSION);
        assert(loader.status().error().partition.value == 1 && events.count == 0 && loader.identities().size() == 0);
        assert(loader.replaceSource(*source));
        until(poll, [&] { return loader.statistics().resident_partitions == 2; });
        assert(events.count == 2 && loader.status());
    }
    {
        auto reader = std::make_shared<Disk>(root / "missing.wvol", *runtime->blocking(), tasks);
        auto input = loading::WorldStorageSource::create(valid.world,
                                                         async::OperationPort<loading::ReadWorldStorageRange>{reader});
        assert(input);
        ecs::Registry registry;
        scene::WorldLoadingSystem loader(registry, *schemas, {*input, tasks});
        registry.emplace<scene::Observer>(registry.create(), std::vector<partition::PartitionOrdinal>{{1}}, true);
        auto poll = [&] {
            std::size_t publications{};
            std::size_t resource_steps{};
            scene::SceneStageContext context{publications, resource_steps};
            const auto progressed = loader.maintain(context);
            if (!progressed)
            {
                const auto *failure = std::any_cast<scene::WorldLoadingFailure>(&progressed.error().cause);
                assert(failure && !loader.status() && failure->code == loader.status().error().code);
            }
        };
        until(poll, [&] { return !loader.status(); });
        assert(loader.status().error().code == scene::EWorldLoadingError::READ_FAILURE);
        assert(std::get<loading::WorldStorageRuntimeFailure>(loader.status().error().cause).code ==
               loading::EWorldStorageRuntimeError::IO_FAILURE);
        assert(loader.identities().size() == 0);
    }
    std::cout << "PASS Observer: original IO/codec failures, no partial installation, same-set source retry\n";

    // Unknown schema bytes remain owned and prevent unsafe automatic unloading.
    {
        auto reader = std::make_shared<Disk>(opaque.path, *runtime->blocking(), tasks);
        auto input = loading::WorldStorageSource::create(opaque.world,
                                                         async::OperationPort<loading::ReadWorldStorageRange>{reader});
        assert(input);
        ecs::Registry registry;
        scene::WorldLoadingSystem loader(registry, *schemas, {*input, tasks});
        const auto observer = registry.create();
        registry.emplace<scene::Observer>(observer, std::vector<partition::PartitionOrdinal>{{1}}, true);
        auto poll = [&] {
            std::size_t publications{};
            std::size_t resource_steps{};
            scene::SceneStageContext context{publications, resource_steps};
            assert(loader.maintain(context));
        };
        until(poll, [&] { return loader.statistics().resident_partitions == 1; });
        registry.destroy(observer);
        poll();
        assert(loader.resident()[0].retention == scene::EPartitionRetention::UNKNOWN_PAYLOAD);
        assert(loader.source({1})->objectAt(0).dataCount() == 2);
    }
    assert(stdexec::sync_wait(tasks.close()));
    runtime->requestStop();
    assert(runtime->join());
    std::cout
        << "PASS WorldLoading: CPU-only SceneDriver integration; no Renderer, Camera, Editor or temporary Registry\n";
}
