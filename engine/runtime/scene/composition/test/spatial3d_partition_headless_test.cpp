#include <lux/engine/core/serialization/Archive.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/scene/SceneAssetSerDeser.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/runtime/entity_scene/EntitySceneCatalog.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionGeneratorCatalog.hpp>
#include <lux/engine/ecs/entity_scene/EntitySectionLoaderSystem.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionService.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/execution/testing/AsyncCloseTestDriver.hpp>
#include <lux/engine/ecs/spatial3d/components/SpatialInterest3DComponent.hpp>
#include <lux/engine/ecs/spatial3d/streaming/SpatialInterest3DSystem.hpp>
#include <lux/engine/ecs/entity_scene/residency/EntitySectionRecordStore.hpp>
#include <lux/engine/ecs/entity_scene/residency/EntitySectionResidencySystem.hpp>

#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    constexpr std::string_view kDemandChannel =
        "org.lux.test.spatial3d.resident";
    constexpr std::uint32_t kParameterMagic = 0x3344534cu;

    lux::runtime::entity_scene::EntitySceneCatalog emptyCatalog()
    {
        lux::scene::SceneDescription package;
        package.id = lux::asset::asset_id_t{
            uuids::uuid::from_string(
                "84000000-0000-4000-8000-000000000001").value()};
        auto result = lux::runtime::entity_scene::EntitySceneCatalog::create(
            std::move(package));
        assert(result);
        return std::move(*result);
    }

    struct GeneratorState final
    {
        lux::ecs::scene_format::SectionGeneratorId id{
            "org.lux.test.spatial3d.rule_grid"};
        mutable std::atomic<std::uint64_t> generated{0u};
    };

    std::uint64_t mix(std::uint64_t value) noexcept
    {
        value ^= value >> 30u;
        value *= 0xbf58476d1ce4e5b9ull;
        value ^= value >> 27u;
        value *= 0x94d049bb133111ebull;
        return value ^ (value >> 31u);
    }

    lux::ecs::scene_format::EntitySectionId sectionId(
        lux::math::GridCoord3i64 coordinate)
    {
        const auto first =
            mix(static_cast<std::uint64_t>(coordinate.x)) ^
            (mix(static_cast<std::uint64_t>(coordinate.y)) << 1u) ^
            (mix(static_cast<std::uint64_t>(coordinate.z)) << 2u);
        const auto second =
            mix(static_cast<std::uint64_t>(coordinate.z) ^
                0x9e3779b97f4a7c15ull) ^
            (mix(static_cast<std::uint64_t>(coordinate.x)) << 1u) ^
            (mix(static_cast<std::uint64_t>(coordinate.y)) << 3u);
        std::array<std::uint8_t, 16u> bytes{};
        for (std::size_t index = 0u; index < 8u; ++index)
        {
            bytes[index] = static_cast<std::uint8_t>(
                first >> (index * 8u));
            bytes[8u + index] = static_cast<std::uint8_t>(
                second >> (index * 8u));
        }
        bytes[6] = static_cast<std::uint8_t>(
            (bytes[6] & 0x0fu) | 0x40u);
        bytes[8] = static_cast<std::uint8_t>(
            (bytes[8] & 0x3fu) | 0x80u);
        return lux::ecs::scene_format::EntitySectionId{uuids::uuid{bytes}};
    }

    std::vector<std::byte> parameters(
        lux::math::GridCoord3i64 coordinate)
    {
        std::vector<std::byte> result;
        lux::serialize::ArchiveWriter writer{result};
        writer.writePod(kParameterMagic);
        writer.writePod(coordinate.x);
        writer.writePod(coordinate.y);
        writer.writePod(coordinate.z);
        return result;
    }

    bool decodeParameters(
        std::span<const std::byte> bytes,
        lux::math::GridCoord3i64& coordinate) noexcept
    {
        lux::serialize::ArchiveReader reader{bytes.data(), bytes.size()};
        const auto magic = reader.readPod<std::uint32_t>();
        coordinate.x = reader.readPod<std::int64_t>();
        coordinate.y = reader.readPod<std::int64_t>();
        coordinate.z = reader.readPod<std::int64_t>();
        return reader.ok() && reader.eof() && magic == kParameterMagic;
    }

    lux::ecs::scene_format::EntitySectionImage image(
        lux::ecs::scene_format::EntitySectionId id)
    {
        lux::ecs::scene_format::EntitySectionImage result;
        result.section = id;
        result.component_names = {""};
        result.archetypes.push_back({{}});
        result.entities.push_back({0u, std::nullopt});
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
                lux::ecs::scene_format::GeneratedSectionSource>(
                    &request.record.source);
            lux::math::GridCoord3i64 coordinate;
            if (!source || source->generator != state.id ||
                !decodeParameters(source->parameters, coordinate) ||
                request.record.id != sectionId(coordinate))
            {
                return lux::cxx::unexpected(EntitySectionGeneratorFailure{
                    EEntitySectionGeneratorError::GENERATION_FAILED,
                    state.id,
                    {},
                    "invalid Spatial3D rule-grid source"});
            }
            state.generated.fetch_add(1u, std::memory_order_relaxed);
            return image(request.record.id);
        };
        return result;
    }

    lux::ecs::scene_format::SectionRecord record(
        const GeneratorState& state,
        lux::math::GridCoord3i64 coordinate)
    {
        lux::ecs::scene_format::SectionRecord result;
        result.id = sectionId(coordinate);
        result.source = lux::ecs::scene_format::GeneratedSectionSource{
            state.id, 0u, parameters(coordinate)};
        auto encoded = lux::ecs::scene_format::encodeEntitySectionImage(
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

    template<class Predicate>
    void drive(
        lux::exec::AsyncRuntime& runtime,
        lux::ecs::Schedule& schedule,
        const lux::ecs::entity_scene::EntitySectionLoaderSystem& loader,
        const lux::ecs::entity_scene::residency::EntitySectionResidencySystem&
            partition,
        const lux::ecs::spatial3d::streaming::SpatialInterest3DSystem&
            interest,
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
        lux::ecs::entity_scene::EntitySectionLoaderSystem& loader,
        lux::exec::AsyncRuntime& runtime,
        lux::ecs::Schedule& schedule)
    {
        lux::exec::testing::CloseEpoch progress{runtime};
        loader.requestClose();
        progress.driveWithStep(
            [&schedule]() noexcept { schedule.tick(0.0f); },
            [&loader]() noexcept { return loader.closeComplete(); },
            [&loader]() noexcept
            {
                const auto state = loader.snapshot();
                return state.waiting_sections != 0u ||
                    state.staging_sections != 0u ||
                    state.armed_sections != 0u ||
                    state.active_sections != 0u;
            });
    }

    template<class Sender>
    void closeOwner(lux::exec::AsyncRuntime& runtime, Sender sender)
    {
        lux::exec::testing::CloseEpoch progress{runtime};
        std::atomic<bool> closed{false};
        auto completion = std::move(sender)
            | stdexec::then(
                  [&closed, &progress]() noexcept
                  {
                      closed.store(true, std::memory_order_release);
                      progress.notify();
                  });
        ::experimental::execution::start_detached(std::move(completion));
        progress.drive([&closed]() noexcept
        {
            return closed.load(std::memory_order_acquire);
        });
    }
}

int main()
{
    namespace entity_runtime = lux::runtime::entity_scene;
    namespace residency = lux::ecs::entity_scene::residency;
    namespace spatial3d = lux::ecs::spatial3d::streaming;

    auto generator_state = std::make_shared<GeneratorState>();
    auto generators = entity_runtime::EntitySectionGeneratorCatalog::create(
        std::vector<entity_runtime::EntitySectionGeneratorDescriptor>{
            descriptor(generator_state)});
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
    auto load_client = service.loadClient();

    const auto sample = record(*generator_state, {0, 0, 0});
    auto vfs = std::make_shared<lux::asset::AssetVfs>();

    // Backpressure is fail-closed before any worker observes the request.
    // This deterministic byte-budget case complements the 125-Section queue
    // burst below without depending on coordinator/worker timing.
    lux::exec::AsyncScope backpressure_scope{runtime};
    {
        std::optional<lux::async::OperationOutcome<
            lux::ecs::entity_scene::LoadEntitySection>> outcome;
        std::atomic<bool> done{false};
        lux::exec::testing::CloseEpoch progress{runtime};
        auto operation = lux::exec::execute(
                load_client.operation(),
                load_client.loadOperation(vfs, sample, 1u),
                lux::async::SubmitOptions{
                    .accounted_bytes =
                        lux::ecs::entity_scene::
                            kEntitySectionLoadByteBudget + 1u})
            | stdexec::continues_on(
                  lux::exec::mainThreadScheduler(runtime))
            | stdexec::then(
                  [&](auto value) noexcept
                  {
                      outcome.emplace(std::move(value));
                      done.store(true, std::memory_order_release);
                      progress.notify();
                  });
        assert(lux::exec::spawn(
            backpressure_scope, std::move(operation)));
        progress.drive([&done]() noexcept
        {
            return done.load(std::memory_order_acquire);
        });
        assert(outcome && !*outcome);
        assert(outcome->error().isRuntime());
        assert(outcome->error().runtimeError() ==
            lux::async::ESubmitError::BYTE_BUDGET_EXHAUSTED);
    }
    closeOwner(runtime, backpressure_scope.closeAsync());

    auto scene_catalog = emptyCatalog();
    residency::EntitySectionRecordStore store{scene_catalog.sections()};
    auto planner = residency::SectionResidencyPlanner::create(
        std::move(store),
        residency::SectionResidencyBudget{
            sample.decoded_bytes * 125u,
            125u});
    assert(planner);
    auto source = spatial3d::Spatial3DSectionSource::ruleGrid(
        [generator_state](lux::math::GridCoord3i64 coordinate)
            -> lux::cxx::expected<
                lux::ecs::scene_format::SectionRecord,
                spatial3d::Spatial3DSourceFailure>
        {
            return record(*generator_state, coordinate);
        });
    assert(source);

    lux::ecs::ComponentTypeCatalog components;
    lux::ecs::World world;
    lux::ecs::PersistentEntityIndex persistent_entities{world.registry()};
    lux::ecs::Schedule schedule{world};
    auto loader = std::make_unique<lux::ecs::entity_scene::EntitySectionLoaderSystem>(
        load_client,
        vfs,
        std::make_unique<entity_runtime::SectionBlobStore>(),
        components,
        persistent_entities,
        lux::ecs::entity_scene::EntitySectionLoaderConfig{4u});
    auto* loader_owner = loader.get();
    assert(schedule.addSystem(std::move(loader)));
    auto partition_system = std::make_unique<
        residency::EntitySectionResidencySystem>(
            loader_owner->client(), std::move(*planner));
    auto* partition_owner = partition_system.get();
    assert(schedule.addSystem(std::move(partition_system)));
    spatial3d::SpatialInterest3DConfig interest_config;
    interest_config.maximum_sources = 2u;
    interest_config.bands.push_back(spatial3d::SpatialInterest3DBand{
        .source_namespace = residency::SectionDemandSourceId{
            "org.lux.test.spatial3d.band0"},
        .sections = std::move(*source),
        .cell_world_size = 64.0,
        .channel = lux::ecs::scene_format::DemandChannelId{
            std::string{kDemandChannel}},
        .active_distance_scale = 1.0,
        .resident_distance_scale = 1.0,
        .maximum_sections_per_source = 256u});
    auto interest_system = std::make_unique<
        spatial3d::SpatialInterest3DSystem>(
            *partition_owner,
            std::move(interest_config));
    auto* interest_owner = interest_system.get();
    assert(schedule.addSystem(std::move(interest_system)));
    assert(schedule.compile().valid());

    auto& registry = world.registry();
    const auto interest_entity = registry.create();
    registry.emplace<lux::ecs::SpatialInterest3DComponent>(
        interest_entity);
    registry.emplace<lux::ecs::ResolvedTransform3DComponent>(
        interest_entity);

    const auto moveInterest = [&](lux::math::Position3d position)
    {
        registry.patch<lux::ecs::ResolvedTransform3DComponent>(
            interest_entity,
            [position](auto& transform) noexcept
            {
                transform.position = position;
            });
    };
    const auto readyAt = [&](lux::math::GridCoord3i64 center)
    {
        return [&, center]() noexcept
        {
            const auto selected = interest_owner->snapshot();
            const auto partitioned = partition_owner->snapshot();
            const auto loaded = loader_owner->snapshot();
            return selected.active_sections == 27u &&
                selected.resident_sections == 125u &&
                partitioned.demand.resident_sections == 125u &&
                partitioned.demand.dynamic_records == 125u &&
                partitioned.active_sections == 125u &&
                loaded.active_sections == 125u &&
                interest_owner->isActive(center);
        };
    };

    // Submit the origin window, but deliberately do not pump main-thread
    // completions. Replacing it immediately makes every late origin result a
    // stale generation while the new 125-Section batch also saturates the
    // operation's bounded queue.
    schedule.tick(0.0f);
    moveInterest({6400.0, -6400.0, 3200.0});
    schedule.tick(0.0f);
    drive(
        runtime,
        schedule,
        *loader_owner,
        *partition_owner,
        *interest_owner,
        readyAt({100, -100, 50}));
    const auto first_load = loader_owner->snapshot();
    assert(first_load.cancelled_requests != 0u);
    assert(first_load.stale_completions != 0u);
    assert(generator_state->generated.load(std::memory_order_relaxed) >=
        125u);

    // Floor ownership is correct on all three negative boundaries.
    moveInterest({-0.001, -64.001, -128.001});
    drive(
        runtime,
        schedule,
        *loader_owner,
        *partition_owner,
        *interest_owner,
        readyAt({-1, -2, -3}));
    assert(interest_owner->isActive({-1, -2, -3}));
    assert(!interest_owner->isActive({1, -2, -3}));

    // Large double coordinates stay in the 3D leaf and never become a float
    // or a partition-core dimension variant.
    constexpr double kLarge = 1'000'000'000'000.0;
    moveInterest({kLarge, -kLarge, kLarge});
    drive(
        runtime,
        schedule,
        *loader_owner,
        *partition_owner,
        *interest_owner,
        readyAt({
            15'625'000'000ll,
            -15'625'000'000ll,
            15'625'000'000ll}));

    // A valid predicted union can still exceed the admitted resident budget.
    // The whole demand revision fails and the previous 125-Section window
    // remains live; no prefix is ever handed to the loader.
    const auto before_prediction_reject = partition_owner->snapshot();
    registry.patch<lux::ecs::SpatialInterest3DComponent>(
        interest_entity,
        [](auto& interest) noexcept
        {
            interest.prediction_offset_x = 256.0;
        });
    schedule.tick(0.0f);
    const auto prediction_reject = interest_owner->snapshot();
    assert(prediction_reject.last_failure);
    assert(prediction_reject.last_failure->code ==
        spatial3d::ESpatialInterest3DError::PARTITION_REJECTED);
    assert(prediction_reject.last_failure->partition);
    assert(prediction_reject.last_failure->partition->code ==
        residency::ESectionResidencyError::
            DECODED_BYTE_BUDGET_EXCEEDED);
    assert(partition_owner->snapshot().demand.revision ==
        before_prediction_reject.demand.revision);
    assert(partition_owner->snapshot().demand.resident_sections == 125u);
    registry.patch<lux::ecs::SpatialInterest3DComponent>(
        interest_entity,
        [](auto& interest) noexcept
        {
            interest.prediction_offset_x = 0.0;
        });
    schedule.tick(0.0f);

    const auto stable_partition = partition_owner->snapshot();
    registry.patch<lux::ecs::SpatialInterest3DComponent>(
        interest_entity,
        [](auto& interest) noexcept
        {
            interest.resident_distance = 32.0;
        });
    schedule.tick(0.0f);
    const auto invalid = interest_owner->snapshot();
    assert(invalid.last_failure);
    assert(invalid.last_failure->code ==
        spatial3d::ESpatialInterest3DError::INVALID_INTEREST);
    assert(partition_owner->snapshot().demand.revision ==
        stable_partition.demand.revision);
    registry.patch<lux::ecs::SpatialInterest3DComponent>(
        interest_entity,
        [](auto& interest) noexcept
        {
            interest.resident_distance = 128.0;
        });
    schedule.tick(0.0f);

    // The generic partition remains the sole generation authority. A stale
    // leaf-source removal is rejected and cannot change its resident plan.
    residency::SectionDemandSourceId source_id{
        "org.lux.test.spatial3d.band0.e" +
        std::to_string(entt::to_integral(interest_entity))};
    const auto stale = partition_owner->removeDemandSource(source_id, 1u);
    assert(!stale);
    assert(stale.error().code ==
        residency::ESectionResidencyError::STALE_SOURCE_GENERATION);
    assert(partition_owner->snapshot().demand.resident_sections == 125u);

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
            return selected.closed && selected.tracked_sources == 0u &&
                partitioned.loader_tickets == 0u &&
                partitioned.demand.source_count == 0u &&
                partitioned.demand.resident_sections == 0u &&
                partitioned.demand.dynamic_records == 0u &&
                loaded.active_sections == 0u &&
                loaded.outstanding_tickets == 0u;
        });

    loader_owner->requestClose();
    schedule.tick(0.0f);
    closeLoader(*loader_owner, runtime, schedule);
    const auto closed_loader = loader_owner->snapshot();
    assert(closed_loader.section_mappings == 0u);
    assert(closed_loader.outstanding_tickets == 0u);
    assert(closed_loader.blobs.current_bytes == 0u);
    assert(closed_loader.blobs.allocation_count == 0u);
    service.close();
    const auto closed_runtime =
        lux::exec::testing::closeRuntime(runtime);
    assert(closed_runtime.clean());
    return 0;
}
