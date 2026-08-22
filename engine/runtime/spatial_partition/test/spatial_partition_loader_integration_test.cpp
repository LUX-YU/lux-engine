#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/scene/SceneAssetSerDeser.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionService.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/testing/AsyncCloseTestDriver.hpp>
#include <lux/engine/runtime/spatial_partition/SpatialPartitionSystem.hpp>

#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    uuids::uuid uuid(const char* value)
    {
        return uuids::uuid::from_string(value).value();
    }

    struct SectionFixture final
    {
        lux::ecs::scene_format::SectionRecord record;
        lux::asset::asset_id_t asset;
        std::vector<std::byte> bytes;
        std::string provider_path;
    };

    SectionFixture makeSection(
        const char* section_id,
        const char* asset_id,
        std::string provider_path)
    {
        namespace format = lux::ecs::scene_format;
        namespace scene = lux::scene;

        format::EntitySectionImage image;
        image.section = format::EntitySectionId{uuid(section_id)};
        image.component_names = {""};
        image.archetypes.push_back({{}});
        image.entities.push_back({0u, std::nullopt});
        auto bytes = format::encodeEntitySectionImage(image);
        assert(bytes);

        lux::ecs::scene_format::SectionRecord record;
        record.id = image.section;
        record.source = lux::ecs::scene_format::StoredSectionSource{
            "/Game/" + provider_path};
        record.content_digest = format::entitySectionContentDigest(*bytes);
        record.encoded_bytes = bytes->size();
        record.decoded_bytes = bytes->size();
        record.entity_count = 1u;
        record.demand_channels.emplace_back("org.lux.test.visible");
        return {
            std::move(record),
            uuid(asset_id),
            std::move(*bytes),
            std::move(provider_path)};
    }

    class MemoryProvider final : public lux::asset::IAssetProvider
    {
    public:
        explicit MemoryProvider(std::vector<SectionFixture> sections)
            : sections_(std::move(sections))
        {}

        [[nodiscard]] std::optional<lux::asset::asset_id_t> resolve(
            std::string_view path) const override
        {
            const auto* value = findPath(path);
            return value ? std::optional{value->asset} : std::nullopt;
        }

        [[nodiscard]] bool contains(
            const lux::asset::asset_id_t& id) const override
        {
            return find(id) != nullptr;
        }

        [[nodiscard]] lux::cxx::expected<
            lux::asset::AssetBlob,
            lux::asset::EAssetError>
        open(const lux::asset::asset_id_t& id) const override
        {
            const auto* value = find(id);
            if (!value)
            {
                return lux::cxx::unexpected(
                    lux::asset::EAssetError::ASSET_NOT_EXIST);
            }
            auto owner = std::shared_ptr<std::byte[]>{
                new std::byte[value->bytes.size()]};
            std::memcpy(
                owner.get(), value->bytes.data(), value->bytes.size());
            return lux::asset::AssetBlob::fromSharedArray(
                std::move(owner), value->bytes.size());
        }

        void enumerate(
            const std::function<void(const lux::asset::ProviderEntry&)>& fn)
            const override
        {
            for (const auto& value : sections_)
            {
                fn({
                    value.asset,
                    lux::ecs::scene_format::kEntitySectionImageMagic,
                    value.provider_path,
                    false});
            }
        }

        [[nodiscard]] std::optional<std::string> pathOf(
            const lux::asset::asset_id_t& id) const override
        {
            const auto* value = find(id);
            return value ? std::optional{value->provider_path} : std::nullopt;
        }

    private:
        [[nodiscard]] const SectionFixture* find(
            const lux::asset::asset_id_t& id) const noexcept
        {
            const auto found = std::find_if(
                sections_.begin(), sections_.end(),
                [&id](const auto& value) { return value.asset == id; });
            return found == sections_.end() ? nullptr : &*found;
        }

        [[nodiscard]] const SectionFixture* findPath(
            std::string_view path) const noexcept
        {
            const auto found = std::find_if(
                sections_.begin(), sections_.end(),
                [path](const auto& value)
                {
                    return value.provider_path == path;
                });
            return found == sections_.end() ? nullptr : &*found;
        }

        std::vector<SectionFixture> sections_;
    };

    lux::runtime::spatial_partition::SpatialDemandSourceUpdate demand(
        std::string source,
        std::uint64_t generation,
        std::initializer_list<lux::ecs::scene_format::EntitySectionId> sections)
    {
        lux::runtime::spatial_partition::SpatialDemandSourceUpdate result;
        result.source =
            lux::runtime::spatial_partition::SpatialDemandSourceId{
                std::move(source)};
        result.generation = generation;
        result.channel = lux::ecs::scene_format::DemandChannelId{
            "org.lux.test.visible"};
        for (const auto section : sections)
            result.demands.push_back({section, 1u});
        return result;
    }

    template<class Predicate>
    void drive(
        lux::exec::AsyncRuntime& runtime,
        lux::ecs::Schedule& schedule,
        lux::runtime::entity_scene::EntitySectionLoaderSystem& loader,
        Predicate&& done)
    {
        lux::exec::testing::CloseEpoch progress{runtime};
        progress.driveWithStep(
            [&schedule]() noexcept { schedule.tick(0.0f); },
            done,
            [&loader]() noexcept
            {
                const auto snapshot = loader.snapshot();
                return snapshot.waiting_admission_sections != 0u ||
                    snapshot.staging_sections != 0u ||
                    snapshot.armed_sections != 0u;
            });
        assert(done());
    }

    void closeLoader(
        lux::runtime::entity_scene::EntitySectionLoaderSystem& loader,
        lux::exec::AsyncRuntime& runtime,
        lux::ecs::Schedule& schedule)
    {
        lux::exec::testing::CloseEpoch progress{runtime};
        std::atomic<bool> closed{false};
        auto close = loader.closeAsync()
            | stdexec::then(
                  [&closed, &progress]() noexcept
                  {
                      closed.store(true, std::memory_order_release);
                      progress.notify();
                  });
        ::experimental::execution::start_detached(std::move(close));
        progress.driveWithStep(
            [&schedule]() noexcept { schedule.tick(0.0f); },
            [&closed]() noexcept
            {
                return closed.load(std::memory_order_acquire);
            },
            [&loader]() noexcept
            {
                const auto snapshot = loader.snapshot();
                return snapshot.waiting_admission_sections != 0u ||
                    snapshot.staging_sections != 0u ||
                    snapshot.armed_sections != 0u ||
                    snapshot.active_sections != 0u;
            });
    }

    lux::runtime::entity_scene::EntitySceneCatalog catalog(
        std::vector<lux::ecs::scene_format::SectionRecord> records)
    {
        std::sort(
            records.begin(), records.end(),
            [](const auto& lhs, const auto& rhs)
            {
                return lhs.id.value() < rhs.id.value();
            });
        lux::scene::SceneDescription package;
        package.id = lux::asset::asset_id_t{uuid(
            "82000000-0000-4000-8000-000000000001")};
        package.sections = std::move(records);
        auto result = lux::runtime::entity_scene::EntitySceneCatalog::create(
            std::move(package));
        assert(result);
        return std::move(*result);
    }
}

int main()
{
    namespace entity_runtime = lux::runtime::entity_scene;
    namespace partition = lux::runtime::spatial_partition;

    // Ordering is a hard Schedule prerequisite, not a registration-order
    // convention. Single-node mutation rejects the partition before adoption
    // when its loader is absent.
    {
        auto empty_catalog = catalog({});
        partition::EntitySectionRecordStore empty_store{empty_catalog};
        auto empty_planner = partition::SpatialDemandPlanner::create(
            std::move(empty_store),
            partition::SpatialPartitionBudget{1u, 1u});
        assert(empty_planner);
        lux::ecs::World missing_world;
        lux::ecs::Schedule missing_schedule{missing_world};
        auto owner = std::make_unique<partition::SpatialPartitionSystem>(
            entity_runtime::EntitySectionClient{},
            std::move(*empty_planner));
        auto rejected = missing_schedule.addSystem(std::move(owner));
        assert(!rejected);
        assert(rejected.error() ==
            lux::ecs::EScheduleMutationError::MissingPrerequisite);
        assert(missing_schedule.systemCount() == 0u);
    }

    auto first = makeSection(
        // Deliberately sorts after its dependent Section. Dependency
        // admission must be topological, never an incidental UUID order.
        "82000000-0000-4000-8000-000000000004",
        "83000000-0000-4000-8000-000000000001",
        "Sections/First_lxes");
    auto second = makeSection(
        "82000000-0000-4000-8000-000000000002",
        "83000000-0000-4000-8000-000000000002",
        "Sections/Second_lxes");
    auto dependency = makeSection(
        "82000000-0000-4000-8000-000000000003",
        "83000000-0000-4000-8000-000000000003",
        "Sections/Dependency_lxes");
    dependency.record.dependencies.push_back(first.record.id);
    const auto first_record = first.record;
    const auto second_record = second.record;
    const auto dependency_record = dependency.record;

    auto provider = std::make_shared<MemoryProvider>(
        std::vector<SectionFixture>{
            std::move(first), std::move(second), std::move(dependency)});
    auto vfs = std::make_shared<lux::asset::AssetVfs>();
    assert(vfs->mount({"/Game", provider, 0}) !=
        lux::asset::kInvalidMountId);

    lux::exec::AsyncRuntimeBuilder runtime_builder;
    auto service_result = entity_runtime::EntitySectionService::addTo(
        runtime_builder);
    assert(service_result);
    auto runtime_plan = std::move(runtime_builder).compile();
    assert(runtime_plan);
    lux::exec::AsyncRuntime runtime{
        std::move(*runtime_plan),
        lux::exec::AsyncRuntimeConfig{
            .blocking_io_threads = 1u,
            .background_cpu_concurrency = 1u}};
    auto service = std::move(*service_result);

    lux::ecs::ComponentTypeCatalog components;
    lux::ecs::World world;
    lux::ecs::PersistentEntityIndex persistent_entities{world.registry()};
    lux::ecs::Schedule schedule{world};
    auto loader = std::make_unique<
        entity_runtime::EntitySectionLoaderSystem>(
            runtime,
            service.loadClient(),
            vfs,
            components,
            persistent_entities);
    auto* loader_owner = loader.get();
    assert(schedule.addSystem(std::move(loader)));

    // A same-type loader prerequisite in this Schedule is insufficient when
    // the partition was handed a client bound to another registry. The
    // topology still compiles, but the system remains unbound and rejects the
    // first demand with a structured ownership error.
    {
        lux::ecs::World cross_world;
        lux::ecs::PersistentEntityIndex cross_persistent_entities{
            cross_world.registry()};
        lux::ecs::Schedule cross_schedule{cross_world};
        auto cross_loader = std::make_unique<
            entity_runtime::EntitySectionLoaderSystem>(
                runtime,
                service.loadClient(),
                vfs,
                components,
                cross_persistent_entities);
        auto* cross_loader_owner = cross_loader.get();
        assert(cross_schedule.addSystem(std::move(cross_loader)));
        auto cross_catalog = catalog({first_record});
        partition::EntitySectionRecordStore cross_store{cross_catalog};
        auto cross_planner = partition::SpatialDemandPlanner::create(
            std::move(cross_store),
            partition::SpatialPartitionBudget{
                first_record.decoded_bytes,
                first_record.entity_count});
        assert(cross_planner);
        auto cross_partition = std::make_unique<
            partition::SpatialPartitionSystem>(
                loader_owner->client(), std::move(*cross_planner));
        auto* cross_partition_owner = cross_partition.get();
        assert(cross_schedule.addSystem(std::move(cross_partition)));
        assert(cross_schedule.compile().valid());
        assert(!cross_partition_owner->snapshot().loader_binding_valid);
        auto cross_demand = cross_partition_owner->replaceDemandSource(
            demand(
                "org.lux.test.cross_registry",
                1u,
                {first_record.id}));
        assert(!cross_demand);
        assert(cross_demand.error().code ==
            partition::ESpatialPartitionError::
                LOADER_REGISTRY_MISMATCH);
        assert(cross_loader_owner->snapshot().outstanding_tickets == 0u);
        closeLoader(*cross_loader_owner, runtime, cross_schedule);
    }

    auto scene_catalog = catalog(
        {first_record, second_record, dependency_record});
    partition::EntitySectionRecordStore store{scene_catalog};
    const auto two_section_budget =
        first_record.decoded_bytes + second_record.decoded_bytes;
    auto planner = partition::SpatialDemandPlanner::create(
        std::move(store),
        partition::SpatialPartitionBudget{two_section_budget, 2u});
    assert(planner);
    auto partition_system = std::make_unique<
        partition::SpatialPartitionSystem>(
            loader_owner->client(), std::move(*planner));
    auto* partition_owner = partition_system.get();
    assert(schedule.addSystem(std::move(partition_system)));
    assert(schedule.compile().valid());

    const auto first_id = first_record.id;
    const auto second_id = second_record.id;
    assert(partition_owner->replaceDemandSource(demand(
        "org.lux.test.camera_a", 1u, {first_id})));
    assert(partition_owner->replaceDemandSource(demand(
        "org.lux.test.camera_b", 1u, {first_id})));
    assert(partition_owner->snapshot().loader_tickets == 1u);
    assert(partition_owner->snapshot().demand.source_references == 2u);
    drive(runtime, schedule, *loader_owner, [&]() noexcept
    {
        return partition_owner->snapshot().active_sections == 1u;
    });

    partition::SpatialDemandSourceId source_a{
        "org.lux.test.camera_a"};
    assert(partition_owner->removeDemandSource(source_a, 1u));
    assert(partition_owner->snapshot().loader_tickets == 1u);
    assert(partition_owner->snapshot().active_sections == 1u);

    // A failed prospective replacement retains source B's old Section and
    // its one loader ticket.
    auto rejected = partition_owner->replaceDemandSource(demand(
        "org.lux.test.camera_b",
        2u,
        {first_id, second_id, dependency_record.id}));
    assert(!rejected);
    assert(rejected.error().code ==
        partition::ESpatialPartitionError::
            DECODED_BYTE_BUDGET_EXCEEDED);
    assert(partition_owner->snapshot().loader_tickets == 1u);
    assert(partition_owner->snapshot().active_sections == 1u);

    // A demand for the dependent Section expands to the complete closure.
    // The already retained dependency is shared, while the dependent row is
    // acquired once and only becomes active after its prerequisite.
    auto dependent = partition_owner->replaceDemandSource(demand(
        "org.lux.test.dependent",
        1u,
        {dependency_record.id}));
    assert(dependent);
    assert(partition_owner->snapshot().loader_tickets == 2u);
    drive(runtime, schedule, *loader_owner, [&]() noexcept
    {
        return partition_owner->snapshot().active_sections == 2u &&
            loader_owner->snapshot().active_sections == 2u;
    });

    partition::SpatialDemandSourceId dependent_source{
        "org.lux.test.dependent"};
    assert(partition_owner->removeDemandSource(dependent_source, 1u));
    assert(partition_owner->snapshot().loader_tickets == 1u);
    drive(runtime, schedule, *loader_owner, [&]() noexcept
    {
        return partition_owner->snapshot().active_sections == 1u &&
            loader_owner->snapshot().active_sections == 1u;
    });

    assert(partition_owner->replaceDemandSource(demand(
        "org.lux.test.camera_b", 2u, {second_id})));
    assert(partition_owner->snapshot().loader_tickets == 1u);
    drive(runtime, schedule, *loader_owner, [&]() noexcept
    {
        const auto snapshot = partition_owner->snapshot();
        return snapshot.loader_tickets == 1u &&
            snapshot.active_sections == 1u &&
            loader_owner->snapshot().active_sections == 1u;
    });

    partition::SpatialDemandSourceId source_b{
        "org.lux.test.camera_b"};
    // Loader close seals new admission before dependants release their
    // retained tickets.  A release-only partition transaction must remain
    // legal in that interval, otherwise interest close can never clear its
    // final tracked source.
    lux::exec::testing::CloseEpoch loader_close_progress{runtime};
    std::atomic<bool> loader_closed{false};
    auto loader_close = loader_owner->closeAsync()
        | stdexec::then(
              [&loader_closed, &loader_close_progress]() noexcept
              {
                  loader_closed.store(true, std::memory_order_release);
                  loader_close_progress.notify();
              });
    ::experimental::execution::start_detached(std::move(loader_close));
    auto sealed_client = loader_owner->client();
    assert(!sealed_client);
    auto acquire_after_seal = partition_owner->replaceDemandSource(demand(
        "org.lux.test.camera_b", 3u, {first_id, second_id}));
    assert(!acquire_after_seal);
    assert(acquire_after_seal.error().code ==
        partition::ESpatialPartitionError::LOADER_UNAVAILABLE);
    assert(partition_owner->snapshot().loader_tickets == 1u);
    auto released_after_seal =
        partition_owner->removeDemandSource(source_b, 2u);
    assert(released_after_seal);
    assert(partition_owner->snapshot().loader_tickets == 0u);
    loader_close_progress.driveWithStep(
        [&schedule]() noexcept { schedule.tick(0.0f); },
        [&loader_closed]() noexcept
        {
            return loader_closed.load(std::memory_order_acquire);
        },
        [&loader_owner]() noexcept
        {
            const auto snapshot = loader_owner->snapshot();
            return snapshot.waiting_admission_sections != 0u ||
                snapshot.staging_sections != 0u ||
                snapshot.armed_sections != 0u ||
                snapshot.active_sections != 0u;
        });
    assert(loader_closed.load(std::memory_order_acquire));
    assert(loader_owner->snapshot().active_sections == 0u);
    assert(partition_owner->snapshot().demand.source_count == 0u);
    assert(partition_owner->snapshot().demand.decoded_bytes == 0u);

    service.close();
    const auto closed = lux::exec::testing::closeRuntime(runtime);
    assert(closed.clean());
    return 0;
}
