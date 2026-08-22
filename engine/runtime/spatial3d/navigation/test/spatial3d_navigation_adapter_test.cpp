#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/navigation/components/NavigationRegion3DComponent.hpp>
#include <lux/engine/navigation/detour3d/NavigationDetour3D.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionGeneratorCatalog.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/execution/testing/AsyncCloseTestDriver.hpp>
#include <lux/engine/runtime/spatial3d/navigation/Navigation3DPrepareService.hpp>
#include <lux/engine/runtime/spatial3d/navigation/Spatial3DNavigationAdapterSystem.hpp>

#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>
#include <uuid.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    [[nodiscard]] uuids::uuid uuid(const char* text)
    {
        const auto parsed = uuids::uuid::from_string(text);
        assert(parsed);
        return *parsed;
    }

    [[nodiscard]] lux::navigation::detour3d::NavigationRegion3DBlob
    makeRegion(lux::navigation::NavigationRegionId id,
               double origin_x,
               double origin_z)
    {
        using namespace lux::navigation::detour3d;
        NavigationRegion3DDescription description;
        description.region = id;
        description.areas.push_back({{{origin_x, 8.0, origin_z},
                                      {origin_x + 64.0, 8.0, origin_z},
                                      {origin_x + 64.0, 8.0, origin_z + 64.0},
                                      {origin_x, 8.0, origin_z + 64.0}}});
        auto encoded = encodeNavigationRegion3D(description);
        assert(encoded);
        return std::move(*encoded);
    }

    struct StoredRegion final
    {
        lux::ecs::scene_format::ContentBlobRef reference;
        lux::ecs::entity_scene::ContentBlobLease owner;
    };

    [[nodiscard]] lux::ecs::scene_format::EntitySectionAttachment
    makeAttachment(
        lux::navigation::detour3d::NavigationRegion3DBlob blob)
    {
        using namespace lux::ecs::scene_format;
        EntitySectionAttachment attachment;
        attachment.reference.type = ContentTypeId{std::string{
            lux::navigation::detour3d::kNavigationRegion3DContentTypeName}};
        attachment.reference.schema_version =
            lux::navigation::detour3d::kNavigationRegion3DSchemaVersion;
        const auto bytes = blob.payload.view();
        attachment.payload.assign(bytes.begin(), bytes.end());
        attachment.reference.id =
            makeContentBlobId(attachment.reference.type,
                              attachment.reference.schema_version,
                              attachment.payload);
        return attachment;
    }

    [[nodiscard]] lux::ecs::scene_format::EntitySectionImage
    makeAttachmentImage(
        lux::ecs::scene_format::EntitySectionId section,
        lux::ecs::scene_format::EntitySectionAttachment attachment)
    {
        lux::ecs::scene_format::EntitySectionImage image;
        image.section = section;
        image.component_names = {""};
        image.attachments.push_back(std::move(attachment));
        return image;
    }

    [[nodiscard]] StoredRegion storeAttachment(
        lux::runtime::entity_scene::SectionBlobStore& store,
        lux::ecs::scene_format::EntitySectionAttachment attachment,
        lux::ecs::scene_format::EntitySectionId section,
        std::uint64_t generation)
    {
        auto acquired =
            store.acquire(std::move(attachment), section, generation);
        assert(acquired);
        auto reference = acquired->reference();
        return {std::move(reference), std::move(*acquired)};
    }

    [[nodiscard]] StoredRegion storeCookedRegion(
        lux::runtime::entity_scene::SectionBlobStore& store,
        lux::navigation::detour3d::NavigationRegion3DBlob blob,
        lux::ecs::scene_format::EntitySectionId section,
        std::uint64_t generation)
    {
        auto image = makeAttachmentImage(
            section, makeAttachment(std::move(blob)));
        auto encoded =
            lux::ecs::scene_format::encodeEntitySectionImage(image);
        assert(encoded);
        auto decoded =
            lux::ecs::scene_format::decodeEntitySectionImage(*encoded);
        assert(decoded);
        assert(decoded->attachments.size() == 1u);
        return storeAttachment(store,
                               std::move(decoded->attachments.front()),
                               section,
                               generation);
    }

    struct GeneratedRegionState final
    {
        lux::ecs::scene_format::EntitySectionAttachment attachment;
    };

    lux::cxx::expected<
        lux::ecs::scene_format::EntitySectionImage,
        lux::runtime::entity_scene::EntitySectionGeneratorFailure>
    generateRegion(
        const void* opaque,
        lux::runtime::entity_scene::GeneratedEntitySectionRequest request)
        noexcept
    {
        const auto& state =
            *static_cast<const GeneratedRegionState*>(opaque);
        return makeAttachmentImage(
            request.record.id,
            state.attachment);
    }

    [[nodiscard]] StoredRegion storeGeneratedRegion(
        lux::runtime::entity_scene::SectionBlobStore& store,
        lux::navigation::detour3d::NavigationRegion3DBlob blob,
        lux::ecs::scene_format::EntitySectionId section,
        std::uint64_t generation)
    {
        constexpr auto generator_name = "org.lux.test.navigation_region3d";
        auto state = std::make_shared<GeneratedRegionState>(
            GeneratedRegionState{makeAttachment(std::move(blob))});
        auto catalog = lux::runtime::entity_scene::
            EntitySectionGeneratorCatalog::create({
                {lux::ecs::scene_format::SectionGeneratorId{generator_name},
                 &generateRegion,
                 std::shared_ptr<const void>{state},
                 {}}});
        assert(catalog);

        auto expected_image =
            makeAttachmentImage(section, state->attachment);
        auto expected_bytes =
            lux::ecs::scene_format::encodeEntitySectionImage(expected_image);
        assert(expected_bytes);
        lux::ecs::scene_format::SectionRecord record;
        record.id = section;
        record.source = lux::ecs::scene_format::GeneratedSectionSource{
            lux::ecs::scene_format::SectionGeneratorId{generator_name},
            0x1234u,
            {std::byte{0x2au}}};
        record.content_digest =
            lux::ecs::scene_format::entitySectionContentDigest(*expected_bytes);
        record.encoded_bytes = expected_bytes->size();
        record.decoded_bytes = expected_bytes->size();
        auto generated = (*catalog)->generate(
            lux::runtime::entity_scene::GeneratedEntitySectionRequest{
                std::move(record)});
        assert(generated);
        assert(*generated == expected_image);
        return storeAttachment(store,
                               std::move(generated->attachments.front()),
                               section,
                               generation);
    }

    template <class Predicate>
    void drive(lux::exec::AsyncRuntime& runtime,
               lux::ecs::Schedule& schedule,
               const lux::ecs::Navigation3DSystem& navigation,
               const lux::runtime::spatial3d::Spatial3DNavigationAdapterSystem&
                   adapter,
               Predicate&& done)
    {
        lux::exec::testing::CloseEpoch progress{runtime};
        progress.driveWithStep(
            [&schedule]() noexcept { schedule.tick(0.0f); },
            std::forward<Predicate>(done),
            [&]() noexcept
            {
                const auto adapter_state = adapter.snapshot();
                const auto navigation_state = navigation.snapshot();
                return adapter_state.waiting_admission_requests != 0u ||
                       adapter_state.in_flight_requests != 0u ||
                       adapter_state.current_completions != 0u ||
                       !navigation.pendingPreparationRequests().empty() ||
                       navigation_state.staging_regions != 0u ||
                       navigation_state.ready_regions != 0u;
            });
    }

    template <class Sender>
    void closeOwner(lux::exec::AsyncRuntime& runtime, Sender sender)
    {
        lux::exec::testing::CloseEpoch progress{runtime};
        std::atomic<bool> closed{false};
        auto completion = std::move(sender) |
                          stdexec::then(
                              [&closed, &progress]() noexcept
                              {
                                  closed.store(true, std::memory_order_release);
                                  progress.notify();
                              });
        ::experimental::execution::start_detached(std::move(completion));
        progress.drive([&closed]() noexcept
                       { return closed.load(std::memory_order_acquire); });
    }
} // namespace

int main()
{
    namespace nav = lux::navigation::detour3d;
    namespace runtime = lux::runtime::spatial3d;

    lux::exec::AsyncRuntimeBuilder builder;
    auto service_result = runtime::Navigation3DPrepareService::addTo(
        builder,
        runtime::Navigation3DPrepareQueueConfig{.capacity = 1u,
                                                .byte_budget =
                                                    4u * 1024u * 1024u,
                                                .drain_batch = 1u});
    assert(service_result);
    auto service = std::move(*service_result);
    auto plan = std::move(builder).compile();
    assert(plan);
    lux::exec::AsyncRuntime async_runtime{
        std::move(*plan),
        lux::exec::AsyncRuntimeConfig{.blocking_io_threads = 1u,
                                      .background_cpu_concurrency = 1u}};
    lux::exec::AsyncScope scene_scope{async_runtime};

    auto cached_service_client = service.client();
    assert(cached_service_client);
    {
        // The service reservation spans queued and running work, rather than
        // ending when AsyncRuntime drains its endpoint queue.
        auto held = cached_service_client.execute(runtime::BuildNavigationRegion3D{
            makeRegion({91u, 901u}, 0.0, 0.0), 1u});
        assert(held);
        auto count_saturated = cached_service_client.execute(
            runtime::BuildNavigationRegion3D{
                makeRegion({92u, 902u}, 128.0, 0.0), 1u});
        assert(!count_saturated);
        assert(count_saturated.error() ==
               lux::async::ESubmitError::QUEUE_FULL);
    }
    {
        std::vector<std::byte> oversized_storage(
            4u * 1024u * 1024u + 1u, std::byte{0x5au});
        nav::NavigationRegion3DBlob oversized{
            {93u, 903u},
            nav::kNavigationRegion3DSchemaVersion,
            lux::cxx::SharedBytes<>::copyOf(oversized_storage)};
        auto byte_saturated = cached_service_client.execute(
            runtime::BuildNavigationRegion3D{std::move(oversized), 1u});
        assert(!byte_saturated);
        assert(byte_saturated.error() ==
               lux::async::ESubmitError::BYTE_BUDGET_EXHAUSTED);
    }
    {
        auto retried = cached_service_client.execute(
            runtime::BuildNavigationRegion3D{
                makeRegion({94u, 904u}, 256.0, 0.0), 1u});
        assert(retried);
    }

    lux::runtime::entity_scene::SectionBlobStore blobs;
    std::vector<lux::ecs::entity_scene::ContentBlobLease> blob_owners;
    blob_owners.reserve(3u);

    // One blob models stored cooked bytes.  The second is generated at
    // runtime.  Both enter the adapter through the same ContentBlobRef path.
    auto stored = storeCookedRegion(
        blobs,
        makeRegion({11u, 101u}, 100'000.0, 75'000.0),
        lux::ecs::scene_format::EntitySectionId{
            uuid("71000000-0000-4000-8000-000000000001")},
        1u);
    auto generated = storeGeneratedRegion(
        blobs,
        makeRegion({12u, 102u}, 200'000.0, 175'000.0),
        lux::ecs::scene_format::EntitySectionId{
            uuid("71000000-0000-4000-8000-000000000002")},
        1u);
    const auto stored_reference = stored.reference;
    const auto generated_reference = generated.reference;
    blob_owners.push_back(std::move(stored.owner));
    blob_owners.push_back(std::move(generated.owner));

    auto backend_result =
        nav::Navigation3DBackend::create(nav::Navigation3DBackendConfig{4u});
    assert(backend_result);
    auto backend = *backend_result;

    {
        lux::ecs::World world;
        auto& registry = world.registry();
        const auto stored_entity = registry.create();
        const auto generated_entity = registry.create();
        registry.emplace<lux::ecs::NavigationRegion3DComponent>(
            stored_entity,
            lux::ecs::NavigationRegion3DComponent{stored_reference});
        registry.emplace<lux::ecs::NavigationRegion3DComponent>(
            generated_entity,
            lux::ecs::NavigationRegion3DComponent{generated_reference});

        lux::ecs::Schedule schedule{world};
        auto navigation_owner = std::make_unique<lux::ecs::Navigation3DSystem>(
            backend, lux::ecs::Navigation3DSystemConfig{4u, 1u});
        auto* navigation = navigation_owner.get();
        const auto navigation_handle =
            schedule.addSystem(std::move(navigation_owner));
        assert(navigation_handle);

        auto adapter_owner =
            std::make_unique<runtime::Spatial3DNavigationAdapterSystem>(
                async_runtime,
                scene_scope,
                service.client(),
                *navigation,
                blobs.client(),
                runtime::Spatial3DNavigationAdapterConfig{
                    2u, 4u * 1024u * 1024u});
        auto* adapter = adapter_owner.get();
        const auto adapter_handle =
            schedule.addSystem(std::move(adapter_owner));
        assert(adapter_handle);

        schedule.tick(0.0f);
        schedule.tick(0.0f);
        assert(adapter->snapshot().queue_backpressure > 0u);
        drive(async_runtime,
              schedule,
              *navigation,
              *adapter,
              [&]() noexcept
              {
                  return backend->snapshot().active_regions == 2u &&
                         adapter->snapshot().current_requests == 0u;
              });
        assert(navigation->status(stored_entity)->state ==
               lux::ecs::ENavigationRegion3DState::ACTIVE);
        assert(navigation->status(generated_entity)->state ==
               lux::ecs::ENavigationRegion3DState::ACTIVE);

        // A completion that arrives after its entity generation was retired
        // is discarded and cannot reactivate backend state.
        auto stale = storeCookedRegion(
            blobs,
            makeRegion({13u, 103u}, 300'000.0, 275'000.0),
            lux::ecs::scene_format::EntitySectionId{
                uuid("71000000-0000-4000-8000-000000000003")},
            1u);
        const auto stale_reference = stale.reference;
        blob_owners.push_back(std::move(stale.owner));
        const auto stale_entity = registry.create();
        registry.emplace<lux::ecs::NavigationRegion3DComponent>(
            stale_entity,
            lux::ecs::NavigationRegion3DComponent{stale_reference});
        schedule.tick(0.0f);
        schedule.tick(0.0f);
        assert(adapter->snapshot().current_requests == 1u);
        const auto stale_before = adapter->snapshot().stale_completions;
        registry.destroy(stale_entity);
        schedule.tick(0.0f);
        drive(async_runtime,
              schedule,
              *navigation,
              *adapter,
              [&]() noexcept
              { return adapter->snapshot().current_requests == 0u; });
        assert(adapter->snapshot().stale_completions > stale_before);
        assert(backend->snapshot().active_regions == 2u);

        // Retiring the Section's owner must not invalidate bytes already
        // pinned by an admitted request.  Closing while that request is in
        // flight must still release the request, reservation and blob lease.
        auto closing = storeGeneratedRegion(
            blobs,
            makeRegion({14u, 104u}, 400'000.0, 375'000.0),
            lux::ecs::scene_format::EntitySectionId{
                uuid("71000000-0000-4000-8000-000000000004")},
            1u);
        const auto closing_entity = registry.create();
        registry.emplace<lux::ecs::NavigationRegion3DComponent>(
            closing_entity,
            lux::ecs::NavigationRegion3DComponent{closing.reference});
        schedule.tick(0.0f);
        schedule.tick(0.0f);
        assert(adapter->snapshot().current_requests == 1u);
        assert(adapter->snapshot().in_flight_requests == 1u);
        const auto pinned_before = blobs.snapshot();
        closing.owner = {};
        blobs.pruneExpired();
        const auto pinned_after = blobs.snapshot();
        assert(pinned_after.current_bytes == pinned_before.current_bytes);
        assert(pinned_after.allocation_count ==
               pinned_before.allocation_count);

        // Whole-Schedule close requests the consumer before its provider.
        // The in-flight result may stop or may arrive successfully; either
        // outcome must first leave the adapter, and every material success is
        // handed to Navigation3DSystem's bounded discard owner.
        schedule.requestClose();
        std::size_t synchronous_close_ticks = 0u;
        while (schedule.closeState().owner_work_pending &&
               synchronous_close_ticks != 64u)
        {
            const auto retirement_before =
                navigation->snapshot().retirement_work_items;
            schedule.tick(0.0f);
            const auto after = navigation->snapshot();
            assert(after.retirement_work_items - retirement_before <= 1u);
            ++synchronous_close_ticks;
        }
        const auto waiting_background = schedule.closeState();
        assert(synchronous_close_ticks != 64u);
        assert(synchronous_close_ticks > 1u);
        assert(!waiting_background.complete);
        assert(!waiting_background.owner_work_pending);
        assert(adapter->snapshot().in_flight_requests == 1u);

        // No amount of owner ticking can complete the background operation.
        // Once its main-thread completion is pumped, the adapter exposes a
        // new bounded local granule (or becomes closed) and Schedule can
        // finish normally.
        scene_scope.requestStop();
        closeOwner(async_runtime, scene_scope.closeAsync());
        assert(adapter->closeComplete() ||
               schedule.closeState().owner_work_pending);

        lux::exec::testing::CloseEpoch close_progress{async_runtime};
        close_progress.driveWithStep(
            [&]() noexcept
            {
                const auto retirement_before =
                    navigation->snapshot().retirement_work_items;
                schedule.tick(0.0f);
                const auto after = navigation->snapshot();
                assert(after.retirement_work_items - retirement_before <=
                       1u);
                assert(after.maximum_retirement_work_items_per_tick <= 1u);
                assert(after.maximum_close_hides_per_tick <= 1u);
            },
            [&]() noexcept { return schedule.closeState().complete; },
            [&]() noexcept
            { return schedule.closeState().owner_work_pending; });
        const auto closed = adapter->snapshot();
        assert(closed.current_requests == 0u);
        assert(closed.current_completions == 0u);
        assert(closed.current_bytes == 0u);
        assert(adapter->closeComplete());
        blobs.pruneExpired();
        assert(blobs.snapshot().allocation_count + 1u ==
               pinned_before.allocation_count);
        assert(navigation->closeComplete());
        assert(backend->snapshot().active_regions == 0u);
        assert(backend->snapshot().retiring_regions == 0u);
        assert(backend->snapshot().owned_bytes == 0u);
        registry.destroy(stored_entity);
        registry.destroy(generated_entity);
        registry.destroy(closing_entity);
        const auto adapter_removed = schedule.removeSystem(*adapter_handle);
        assert(adapter_removed);
        const auto navigation_removed =
            schedule.removeSystem(*navigation_handle);
        assert(navigation_removed);
    }

    blob_owners.clear();
    blobs.pruneExpired();
    assert(blobs.snapshot().current_bytes == 0u);
    assert(blobs.snapshot().allocation_count == 0u);
    // Move-assignment closes the displaced service admission before taking
    // ownership.  Existing clients of the displaced service cannot submit.
    lux::exec::AsyncRuntimeBuilder displaced_builder;
    auto displaced = runtime::Navigation3DPrepareService::addTo(
        displaced_builder);
    assert(displaced);
    auto displaced_client = displaced->client();
    *displaced = std::move(service);
    assert(!displaced_client);
    service = std::move(*displaced);

    service.close();
    assert(!cached_service_client);
    auto after_close = cached_service_client.execute(
        runtime::BuildNavigationRegion3D{
            makeRegion({95u, 905u}, 384.0, 0.0), 1u});
    assert(!after_close);
    assert(after_close.error() ==
           lux::async::ESubmitError::FEATURE_CLOSING);
    const auto close_report = lux::exec::testing::closeRuntime(async_runtime);
    assert(close_report.clean());
    return 0;
}
