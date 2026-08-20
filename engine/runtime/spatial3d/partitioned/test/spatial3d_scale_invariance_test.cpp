#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/spatial3d/components/SpatialInterest3DComponent.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/resource/asset/AssetVfs.hpp>
#include <lux/engine/scene/ScenePackageCodec.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/runtime/entity_scene/EntitySceneCatalog.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionGeneratorCatalog.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionLoaderSystem.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionService.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/execution/testing/AsyncCloseTestDriver.hpp>
#include <lux/engine/runtime/spatial3d/partitioned/Spatial3DSectionSource.hpp>
#include <lux/engine/runtime/spatial3d/partitioned/SpatialInterest3DSystem.hpp>
#include <lux/engine/runtime/spatial_partition/EntitySectionRecordStore.hpp>
#include <lux/engine/runtime/spatial_partition/SpatialPartitionSystem.hpp>

#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace
{
    constexpr std::size_t kResidentSections = 125u;
    constexpr std::size_t kActiveSections = 27u;
    constexpr std::size_t kBaseCatalogSections = 250u;
    constexpr std::string_view kDemandChannel =
        "org.lux.test.spatial3d.scale.resident";

    struct GeneratorState final
    {
        lux::scene::SectionGeneratorId id{
            "org.lux.test.spatial3d.scale.generator"};
        mutable std::atomic<std::uint64_t> generated{0u};
    };

    lux::ecs::scene_format::EntitySectionId sectionId(std::uint64_t ordinal)
    {
        std::array<std::uint8_t, 16u> bytes{};
        for (std::size_t index = 0u; index < 8u; ++index)
        {
            bytes[15u - index] = static_cast<std::uint8_t>(
                ordinal >> (index * 8u));
        }
        bytes[6] = static_cast<std::uint8_t>(
            (bytes[6] & 0x0fu) | 0x40u);
        bytes[8] = static_cast<std::uint8_t>(
            (bytes[8] & 0x3fu) | 0x80u);
        return lux::ecs::scene_format::EntitySectionId{uuids::uuid{bytes}};
    }

    lux::ecs::scene_format::EntitySectionImage image(
        lux::ecs::scene_format::EntitySectionId section)
    {
        lux::ecs::scene_format::EntitySectionImage result;
        result.section = section;
        result.component_names = {""};
        result.archetypes.push_back({{}});
        result.entities.push_back({0u, std::nullopt});
        return result;
    }

    lux::scene::SectionRecord record(
        const GeneratorState& generator,
        std::uint64_t ordinal)
    {
        lux::scene::SectionRecord result;
        result.id = sectionId(ordinal);
        result.source = lux::scene::GeneratedSectionSource{
            generator.id, ordinal, {}};
        const auto encoded = lux::ecs::scene_format::encodeEntitySectionImage(
            image(result.id));
        assert(encoded);
        result.content_digest =
            lux::ecs::scene_format::entitySectionContentDigest(*encoded);
        result.encoded_bytes = encoded->size();
        result.decoded_bytes = encoded->size();
        result.entity_count = 1u;
        result.demand_channels.emplace_back(std::string{kDemandChannel});
        return result;
    }

    lux::runtime::entity_scene::EntitySectionGeneratorDescriptor descriptor(
        std::shared_ptr<const GeneratorState> state)
    {
        using namespace lux::runtime::entity_scene;
        EntitySectionGeneratorDescriptor result;
        result.id = state->id;
        result.state = std::shared_ptr<const void>{std::move(state)};
        result.generate = [](
            const void* opaque,
            GeneratedEntitySectionRequest request) noexcept
            -> lux::cxx::expected<
                lux::ecs::scene_format::EntitySectionImage,
                EntitySectionGeneratorFailure>
        {
            const auto& state = *static_cast<const GeneratorState*>(opaque);
            const auto* source = std::get_if<
                lux::scene::GeneratedSectionSource>(
                    &request.record.source);
            if (!source || source->generator != state.id)
            {
                return lux::cxx::unexpected(EntitySectionGeneratorFailure{
                    EEntitySectionGeneratorError::GENERATION_FAILED,
                    state.id,
                    {},
                    "invalid scale-invariance generator source"});
            }
            state.generated.fetch_add(1u, std::memory_order_relaxed);
            return image(request.record.id);
        };
        return result;
    }

    struct CatalogFixture final
    {
        lux::runtime::entity_scene::EntitySceneCatalog catalog;
        lux::runtime::spatial3d::Spatial3DSectionCatalog spatial;
        std::size_t section_count{0u};
        std::uint64_t section_bytes{0u};
    };

    CatalogFixture makeCatalog(
        const GeneratorState& generator,
        std::uint32_t scale)
    {
        namespace spatial3d = lux::runtime::spatial3d;

        const auto section_count = kBaseCatalogSections * scale;
        lux::scene::ScenePackage package;
        package.id = lux::scene::ScenePackageId{
            uuids::uuid::from_string(
                "8a000000-0000-4000-8000-000000000001").value()};
        package.sections.reserve(section_count);
        std::vector<spatial3d::Spatial3DSectionCatalogEntry> entries;
        entries.reserve(section_count);

        std::uint64_t ordinal = 1u;
        const auto append = [&](lux::math::GridCoord3i64 coordinate)
        {
            auto section = record(generator, ordinal++);
            entries.push_back({coordinate, section.id});
            package.sections.push_back(std::move(section));
        };

        // Two disjoint complete 5x5x5 windows preserve the old teleport
        // coverage while keeping the active window independent of catalog
        // size. All scale-only records are deliberately far from both.
        for (const std::int64_t center_x : {0ll, 10ll})
        {
            for (std::int64_t x = center_x - 2; x <= center_x + 2; ++x)
            {
                for (std::int64_t y = -2; y <= 2; ++y)
                {
                    for (std::int64_t z = -2; z <= 2; ++z)
                        append({x, y, z});
                }
            }
        }
        for (std::size_t index = entries.size(); index < section_count;
             ++index)
        {
            append({
                1'000'000 + static_cast<std::int64_t>(index),
                1'000'000,
                1'000'000});
        }

        const auto section_bytes = package.sections.front().decoded_bytes;
        const auto encoded = lux::scene::encodeScenePackage(
            package);
        assert(encoded);
        const auto decoded = lux::scene::decodeScenePackage(
            *encoded);
        assert(decoded);
        auto catalog =
            lux::runtime::entity_scene::EntitySceneCatalog::create(
                std::move(*decoded));
        assert(catalog);
        auto spatial = spatial3d::Spatial3DSectionCatalog::create(
            std::move(entries));
        assert(spatial);
        assert(catalog->sections().size() == section_count);
        assert(spatial->entries().size() == section_count);
        return CatalogFixture{
            std::move(*catalog),
            std::move(*spatial),
            section_count,
            section_bytes};
    }

    template<class Predicate>
    void drive(
        lux::exec::AsyncRuntime& runtime,
        lux::ecs::Schedule& schedule,
        const lux::runtime::entity_scene::EntitySectionLoaderSystem& loader,
        const lux::runtime::spatial_partition::SpatialPartitionSystem&
            partition,
        const lux::runtime::spatial3d::SpatialInterest3DSystem& interest,
        Predicate&& done)
    {
        lux::exec::testing::CloseEpoch progress{runtime};
        progress.driveWithStep(
            [&schedule]() noexcept { schedule.tick(0.0f); },
            std::forward<Predicate>(done),
            [&]() noexcept
            {
                const auto loaded = loader.snapshot();
                const auto partitioned = partition.snapshot();
                const auto selected = interest.snapshot();
                assert(loaded.failed_sections == 0u);
                assert(partitioned.failed_sections == 0u);
                assert(!selected.last_failure);
                return loaded.waiting_sections != 0u ||
                    loaded.waiting_admission_sections != 0u ||
                    loaded.staging_sections != 0u ||
                    loaded.armed_sections != 0u ||
                    (selected.closing && !selected.closed) ||
                    partitioned.loader_tickets !=
                        partitioned.demand.resident_sections;
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
                const auto state = loader.snapshot();
                return state.waiting_sections != 0u ||
                    state.staging_sections != 0u ||
                    state.armed_sections != 0u ||
                    state.active_sections != 0u;
            });
    }

    struct ScaleSample final
    {
        std::uint32_t scale{0u};
        std::size_t catalog_sections{0u};
        std::uint64_t generated_sections{0u};
        lux::runtime::spatial3d::SpatialInterest3DSnapshot interest;
        lux::runtime::spatial_partition::SpatialPartitionSnapshot partition;
        lux::runtime::entity_scene::EntitySectionLoaderSnapshot loader;
    };

    ScaleSample runScale(
        std::uint32_t scale,
        lux::exec::AsyncRuntime& runtime,
        const lux::runtime::entity_scene::EntitySectionLoadClient&
            load_client,
        const std::shared_ptr<const GeneratorState>& generator)
    {
        namespace entity_runtime = lux::runtime::entity_scene;
        namespace partition = lux::runtime::spatial_partition;
        namespace spatial3d = lux::runtime::spatial3d;

        auto fixture = makeCatalog(*generator, scale);
        const auto generated_before =
            generator->generated.load(std::memory_order_relaxed);
        partition::EntitySectionRecordStore store{fixture.catalog};
        auto planner = partition::SpatialDemandPlanner::create(
            std::move(store),
            partition::SpatialPartitionBudget{
                fixture.section_bytes * kResidentSections,
                kResidentSections});
        assert(planner);

        lux::ecs::ComponentTypeCatalog components;
        lux::ecs::World world;
        lux::ecs::PersistentEntityIndex persistent_entities{
            world.registry()};
        lux::ecs::Schedule schedule{world};
        auto vfs = std::make_shared<lux::asset::AssetVfs>();
        auto loader = std::make_unique<
            entity_runtime::EntitySectionLoaderSystem>(
                runtime,
                load_client,
                std::move(vfs),
                components,
                persistent_entities,
                entity_runtime::EntitySectionLoaderConfig{64u});
        auto* loader_owner = loader.get();
        assert(schedule.addSystem(std::move(loader)));
        auto partition_system =
            std::make_unique<partition::SpatialPartitionSystem>(
                loader_owner->client(), std::move(*planner));
        auto* partition_owner = partition_system.get();
        assert(schedule.addSystem(std::move(partition_system)));

        spatial3d::SpatialInterest3DConfig interest_config;
        interest_config.maximum_sources = 1u;
        interest_config.bands.push_back(spatial3d::SpatialInterest3DBand{
            .source_namespace = partition::SpatialDemandSourceId{
                "org.lux.test.spatial3d.scale.band"},
            .sections = spatial3d::Spatial3DSectionSource::catalog(
                std::move(fixture.spatial)),
            .cell_world_size = 64.0,
            .channel = lux::scene::DemandChannelId{
                std::string{kDemandChannel}},
            .active_distance_scale = 1.0,
            .resident_distance_scale = 1.0,
            .maximum_sections_per_source = kResidentSections});
        auto interest_system =
            std::make_unique<spatial3d::SpatialInterest3DSystem>(
                *partition_owner, std::move(interest_config));
        auto* interest_owner = interest_system.get();
        assert(schedule.addSystem(std::move(interest_system)));
        assert(schedule.compile().valid());

        auto& registry = world.registry();
        const auto interest_entity = registry.create();
        registry.emplace<lux::ecs::SpatialInterest3DComponent>(
            interest_entity);
        registry.emplace<lux::ecs::ResolvedTransform3DComponent>(
            interest_entity);

        const auto readyAt = [&](lux::math::GridCoord3i64 center)
        {
            return [&, center]() noexcept
            {
                const auto selected = interest_owner->snapshot();
                const auto partitioned = partition_owner->snapshot();
                const auto loaded = loader_owner->snapshot();
                return selected.tracked_sources == 1u &&
                    selected.active_sections == kActiveSections &&
                    selected.resident_sections == kResidentSections &&
                    partitioned.demand.source_count == 1u &&
                    partitioned.demand.dynamic_records == 0u &&
                    partitioned.demand.resident_sections ==
                        kResidentSections &&
                    partitioned.demand.source_references ==
                        kResidentSections &&
                    partitioned.loader_tickets == kResidentSections &&
                    partitioned.waiting_sections == 0u &&
                    partitioned.staging_sections == 0u &&
                    partitioned.active_sections == kResidentSections &&
                    loaded.waiting_sections == 0u &&
                    loaded.waiting_admission_sections == 0u &&
                    loaded.staging_sections == 0u &&
                    loaded.armed_sections == 0u &&
                    loaded.active_sections == kResidentSections &&
                    loaded.outstanding_tickets == kResidentSections &&
                    interest_owner->isActive(center);
            };
        };

        drive(
            runtime,
            schedule,
            *loader_owner,
            *partition_owner,
            *interest_owner,
            readyAt({0, 0, 0}));
        assert(interest_owner->isActive({0, 0, 0}));

        // A whole-window teleport replaces the logical demand atomically.
        // The physical loader may overlap retirement with admission, but the
        // settled snapshot remains bounded to the same 125 tickets.
        registry.patch<lux::ecs::ResolvedTransform3DComponent>(
            interest_entity,
            [](auto& transform) noexcept
            {
                transform.position = {640.0, 0.0, 0.0};
            });
        drive(
            runtime,
            schedule,
            *loader_owner,
            *partition_owner,
            *interest_owner,
            readyAt({10, 0, 0}));
        assert(interest_owner->isActive({10, 0, 0}));
        assert(!interest_owner->isActive({0, 0, 0}));

        const auto interest_snapshot = interest_owner->snapshot();
        const auto partition_snapshot = partition_owner->snapshot();
        const auto loader_snapshot = loader_owner->snapshot();
        assert(partition_snapshot.demand.decoded_bytes ==
            partition_snapshot.demand.maximum_decoded_bytes);
        assert(partition_snapshot.demand.entity_count ==
            partition_snapshot.demand.maximum_entities);
        assert(partition_snapshot.committed_replacements == 2u);
        assert(generator->generated.load(std::memory_order_relaxed) -
                generated_before ==
            2u * kResidentSections);

        // An unchanged source performs no new catalog materialization or
        // ticket transition, regardless of the LXSC's total Section count.
        for (std::uint32_t tick = 0u; tick < 64u; ++tick)
            schedule.tick(0.0f);
        assert(partition_owner->snapshot().demand.revision ==
            partition_snapshot.demand.revision);
        assert(loader_owner->snapshot().outstanding_tickets ==
            loader_snapshot.outstanding_tickets);
        assert(generator->generated.load(std::memory_order_relaxed) -
                generated_before ==
            2u * kResidentSections);

        const ScaleSample sample{
            scale,
            fixture.section_count,
            generator->generated.load(std::memory_order_relaxed) -
                generated_before,
            interest_snapshot,
            partition_snapshot,
            loader_snapshot};

        interest_owner->requestClose();
        drive(
            runtime,
            schedule,
            *loader_owner,
            *partition_owner,
            *interest_owner,
            [&]() noexcept
            {
                const auto selected = interest_owner->snapshot();
                const auto partitioned = partition_owner->snapshot();
                const auto loaded = loader_owner->snapshot();
                return selected.closed &&
                    selected.tracked_sources == 0u &&
                    partitioned.demand.source_count == 0u &&
                    partitioned.demand.resident_sections == 0u &&
                    partitioned.loader_tickets == 0u &&
                    loaded.active_sections == 0u &&
                    loaded.outstanding_tickets == 0u;
            });
        loader_owner->requestClose();
        schedule.tick(0.0f);
        closeLoader(*loader_owner, runtime, schedule);
        const auto closed = loader_owner->snapshot();
        assert(closed.section_mappings == 0u);
        assert(closed.outstanding_tickets == 0u);
        assert(closed.blobs.current_bytes == 0u);
        assert(closed.blobs.allocation_count == 0u);
        return sample;
    }

    bool withinFivePercent(std::uint64_t baseline, std::uint64_t sample)
    {
        if (baseline == sample)
            return true;
        return std::abs(
                   static_cast<long double>(sample) -
                   static_cast<long double>(baseline)) /
                static_cast<long double>(
                    std::max<std::uint64_t>(1u, baseline)) <=
            0.05L;
    }

    void assertInvariant(
        const ScaleSample& baseline,
        const ScaleSample& sample)
    {
        // The LXSC catalog itself deliberately grows 1x/10x/100x.
        assert(sample.catalog_sections ==
            baseline.catalog_sections * sample.scale);

        // Every active-window, ticket and admitted-budget observation stays
        // within the locked five-percent gate. Current deterministic values
        // are identical; the tolerance expresses the public acceptance rule.
        const auto invariant = [](std::uint64_t expected, std::uint64_t value)
        {
            assert(withinFivePercent(expected, value));
        };
        invariant(
            baseline.generated_sections, sample.generated_sections);
        invariant(
            baseline.interest.active_sections,
            sample.interest.active_sections);
        invariant(
            baseline.interest.resident_sections,
            sample.interest.resident_sections);
        invariant(
            baseline.partition.demand.resident_sections,
            sample.partition.demand.resident_sections);
        invariant(
            baseline.partition.demand.source_references,
            sample.partition.demand.source_references);
        invariant(
            baseline.partition.demand.decoded_bytes,
            sample.partition.demand.decoded_bytes);
        invariant(
            baseline.partition.demand.entity_count,
            sample.partition.demand.entity_count);
        invariant(
            baseline.partition.demand.maximum_decoded_bytes,
            sample.partition.demand.maximum_decoded_bytes);
        invariant(
            baseline.partition.demand.maximum_entities,
            sample.partition.demand.maximum_entities);
        invariant(
            baseline.partition.loader_tickets,
            sample.partition.loader_tickets);
        invariant(
            baseline.partition.active_sections,
            sample.partition.active_sections);
        invariant(
            baseline.loader.active_sections,
            sample.loader.active_sections);
        invariant(
            baseline.loader.outstanding_tickets,
            sample.loader.outstanding_tickets);
    }
}

int main()
{
    namespace entity_runtime = lux::runtime::entity_scene;

    auto generator = std::make_shared<GeneratorState>();
    auto generators = entity_runtime::EntitySectionGeneratorCatalog::create(
        std::vector<entity_runtime::EntitySectionGeneratorDescriptor>{
            descriptor(generator)});
    assert(generators);

    lux::exec::AsyncRuntimeBuilder runtime_builder;
    auto service_result = entity_runtime::EntitySectionService::addTo(
        runtime_builder, *generators);
    assert(service_result);
    auto runtime_plan = std::move(runtime_builder).compile();
    assert(runtime_plan);
    lux::exec::AsyncRuntime runtime{
        std::move(*runtime_plan),
        lux::exec::AsyncRuntimeConfig{
            .blocking_io_threads = 1u,
            .background_cpu_concurrency = 4u}};
    auto service = std::move(*service_result);
    const auto load_client = service.loadClient();

    const std::array scales{1u, 10u, 100u};
    std::vector<ScaleSample> samples;
    samples.reserve(scales.size());
    for (const auto scale : scales)
        samples.push_back(runScale(scale, runtime, load_client, generator));
    for (std::size_t index = 1u; index < samples.size(); ++index)
        assertInvariant(samples.front(), samples[index]);

    service.close();
    const auto closed = lux::exec::testing::closeRuntime(runtime);
    assert(closed.clean());
    return 0;
}
