#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/physics3d/components/Physics3DComponents.hpp>
#include <lux/engine/ecs/physics3d/systems/Physics3DSystem.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/resource/physics3d/StaticColliderBatch3D.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/execution/testing/AsyncCloseTestDriver.hpp>
#include <lux/engine/runtime/spatial3d/physics/StaticCollider3DPrepareService.hpp>
#include <lux/engine/runtime/spatial3d/physics/StaticCollider3DSystem.hpp>

#include <exec/start_detached.hpp>
#include <stdexec/execution.hpp>
#include <uuid.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
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

    struct StoredBatch final
    {
        lux::ecs::scene_format::ContentBlobRef reference;
        lux::runtime::entity_scene::ContentBlobLease owner;
        lux::cxx::SharedBytes<> bytes;
    };

    [[nodiscard]] StoredBatch storeBatch(
        lux::runtime::entity_scene::SectionBlobStore& store,
        const char* section_id,
        std::uint16_t sample)
    {
        lux::physics3d::StaticColliderBatch3DBlobV1 blob;
        for (std::uint32_t index = 0u; index < 3u; ++index)
        {
            auto& heightfield = blob.heightfields.emplace_back();
            heightfield.local_origin.x = static_cast<double>(index) * 4.0;
            heightfield.sample_edge = 3u;
            heightfield.sample_spacing = 1.0f;
            heightfield.height_min = -1.0f;
            heightfield.height_max = 2.0f;
            heightfield.samples.assign(
                9u, static_cast<std::uint16_t>(sample + index));
        }
        auto encoded =
            lux::physics3d::encodeStaticColliderBatch3DBlob(blob);
        assert(encoded);

        lux::ecs::scene_format::EntitySectionAttachment attachment;
        attachment.reference.type = lux::ecs::scene_format::ContentTypeId{
            std::string{
                lux::physics3d::kStaticColliderBatch3DContentTypeName}};
        attachment.reference.schema_version =
            lux::physics3d::kStaticColliderBatch3DSchemaVersion;
        attachment.payload = std::move(*encoded);
        attachment.reference.id = lux::ecs::scene_format::makeContentBlobId(
            attachment.reference.type,
            attachment.reference.schema_version,
            attachment.payload);
        auto acquired = store.acquire(
            std::move(attachment),
            lux::ecs::scene_format::EntitySectionId{uuid(section_id)},
            1u);
        assert(acquired);
        const auto reference = acquired->reference();
        auto bytes = acquired->bytes();
        return {reference, std::move(*acquired), std::move(bytes)};
    }

    template <class Predicate, class StepCheck>
    void drive(
        lux::exec::AsyncRuntime& runtime,
        lux::ecs::Schedule& schedule,
        const lux::runtime::spatial3d::StaticCollider3DSystem& statics,
        Predicate&& done,
        StepCheck&& check)
    {
        lux::exec::testing::CloseEpoch progress{runtime};
        progress.driveWithStep(
            [&]() noexcept
            {
                schedule.tick(0.0f);
                check();
            },
            std::forward<Predicate>(done),
            [&]() noexcept
            {
                const auto state = statics.snapshot();
                return state.waiting_entities != 0u ||
                    state.background_entities != 0u ||
                    state.staging_entities != 0u ||
                    state.ready_entities != 0u ||
                    state.retirement_queue_size != 0u;
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
        progress.drive(
            [&closed]() noexcept
            {
                return closed.load(std::memory_order_acquire);
            });
    }
} // namespace

int main()
{
    namespace runtime = lux::runtime::spatial3d;

    lux::exec::AsyncRuntimeBuilder builder;
    auto service_result = runtime::StaticCollider3DPrepareService::addTo(
        builder,
        runtime::StaticCollider3DPrepareQueueConfig{
            .capacity = 1u,
            // A replacement keeps the old collision revision resident until
            // the candidate is published, so the fixture must budget two
            // conservative prepared owners at once.
            .byte_budget = 40u * 1024u * 1024u,
            .drain_batch = 1u});
    assert(service_result);
    auto service = std::move(*service_result);
    auto plan = std::move(builder).compile();
    assert(plan);
    lux::exec::AsyncRuntime async_runtime{
        std::move(*plan),
        lux::exec::AsyncRuntimeConfig{
            .blocking_io_threads = 1u,
            .background_cpu_concurrency = 1u}};
    lux::exec::AsyncScope scene_scope{async_runtime};

    lux::runtime::entity_scene::SectionBlobStore blobs;
    auto first = storeBatch(
        blobs,
        "81000000-0000-4000-8000-000000000001",
        21845u);
    auto second = storeBatch(
        blobs,
        "81000000-0000-4000-8000-000000000002",
        43690u);

    // Admission remains reserved while the typed sender owns queued work.
    const auto transform = lux::ecs::ResolvedTransform3DComponent{
        {100'000'000.0, 4.0, 75'000'000.0},
        Eigen::Matrix3f::Identity()};
    {
        auto held = service.client().execute(runtime::BuildStaticCollider3D{
            first.bytes, transform, 1u});
        assert(held);
        auto saturated = service.client().execute(
            runtime::BuildStaticCollider3D{second.bytes, transform, 2u});
        assert(!saturated);
        assert(saturated.error() ==
            lux::exec::EAsyncSubmitError::QUEUE_FULL);
        const auto admitted = service.snapshot();
        assert(admitted.active_requests == 1u);
        assert(admitted.owned_bytes > 16u * 1024u * 1024u);
        assert(admitted.request_high_water == 1u);
        assert(admitted.rejected_capacity == 1u);
    }
    assert(service.snapshot().active_requests == 0u);
    assert(service.snapshot().owned_bytes == 0u);
    {
        std::vector<std::byte> oversized_storage(
            4u * 1024u * 1024u + 1u, std::byte{0x5au});
        auto oversized = service.client().execute(
            runtime::BuildStaticCollider3D{
                lux::cxx::SharedBytes<>::copyOf(oversized_storage),
                transform,
                3u});
        assert(!oversized);
        assert(oversized.error() ==
            lux::exec::EAsyncSubmitError::BYTE_BUDGET_EXHAUSTED);
        assert(service.snapshot().rejected_bytes == 1u);
    }

    lux::ecs::Physics3DConfig config;
    config.gravity.setZero();
    config.maximum_bodies = 4096u;
    config.maximum_body_pairs = 8192u;
    config.maximum_contact_constraints = 2048u;
    auto created = lux::ecs::Physics3DScene::create(config);
    assert(created);
    auto scene = *created;

    {
        lux::ecs::World world;
        auto& registry = world.registry();
        const auto entity = registry.create();
        registry.emplace<lux::ecs::ResolvedTransform3DComponent>(
            entity, transform);
        registry.emplace<lux::ecs::StaticColliderBatch3DComponent>(
            entity,
            lux::ecs::StaticColliderBatch3DComponent{first.reference});

        lux::ecs::Schedule schedule{world};
        auto static_owner = std::make_unique<runtime::StaticCollider3DSystem>(
            async_runtime,
            scene_scope,
            service.client(),
            scene,
            blobs.client(),
            runtime::StaticCollider3DSystemConfig{1u, 4u, 1u});
        auto* statics = static_owner.get();
        assert(schedule.addSystem(
            std::make_unique<lux::ecs::Physics3DSystem>(scene)));
        assert(schedule.addSystem(std::move(static_owner)));

        schedule.tick(0.0f);
        std::uint32_t previous_bodies =
            scene->staticHeightfieldBodyCount();
        drive(
            async_runtime,
            schedule,
            *statics,
            [&]() noexcept
            {
                const auto status = statics->status(entity);
                return status &&
                    status->state == runtime::EStaticCollider3DState::ACTIVE;
            },
            [&]() noexcept
            {
                const auto current = scene->staticHeightfieldBodyCount();
                assert(current <= previous_bodies + 1u);
                previous_bodies = current;
            });
        assert(registry.all_of<
            runtime::StaticCollider3DBindingComponent>(entity));
        assert(scene->staticHeightfieldBodyCount() == 3u);
        assert(service.snapshot().active_requests == 0u);
        assert(service.snapshot().owned_bytes > 0u);
        assert(statics->snapshot().owned_budget_bytes ==
            service.snapshot().owned_bytes);

        auto original = registry.get<
            runtime::StaticCollider3DBindingComponent>(entity).binding;
        assert(original && original->active());

        // A bad desired revision reports FAILED but leaves the old active
        // collision revision and its blob lease in place.
        registry.patch<lux::ecs::StaticColliderBatch3DComponent>(
            entity,
            [](auto& component)
            {
                component.content.type = lux::ecs::scene_format::ContentTypeId{
                    std::string{"lux.physics3d.unsupported"}};
            });
        schedule.tick(0.0f);
        assert(registry.get<runtime::StaticCollider3DStatusComponent>(entity)
                   .state == runtime::EStaticCollider3DState::FAILED);
        assert(registry.get<runtime::StaticCollider3DBindingComponent>(entity)
                   .binding == original);
        assert(original->active());
        assert(scene->staticHeightfieldBodyCount() == 3u);

        // Patch success prepares a candidate beside the old revision.  The
        // ordinary domain update publishes the candidate after reserving its
        // command slot. The barrier only patches the armed transient owner;
        // the next bounded update then hides the old lease.
        registry.patch<lux::ecs::StaticColliderBatch3DComponent>(
            entity,
            [&](auto& component)
            {
                component.content = second.reference;
            });
        std::uint32_t minimum_bodies =
            scene->staticHeightfieldBodyCount();
        previous_bodies = minimum_bodies;
        schedule.tick(0.0f);
        drive(
            async_runtime,
            schedule,
            *statics,
            [&]() noexcept
            {
                const auto status = statics->status(entity);
                return status &&
                    status->state == runtime::EStaticCollider3DState::ACTIVE &&
                    registry.get<runtime::StaticCollider3DBindingComponent>(
                        entity).binding != original;
            },
            [&]() noexcept
            {
                minimum_bodies = std::min(
                    minimum_bodies,
                    scene->staticHeightfieldBodyCount());
                const auto current = scene->staticHeightfieldBodyCount();
                assert(current <= previous_bodies + 1u);
                previous_bodies = current;
            });
        assert(minimum_bodies >= 3u);
        assert(original->active());
        schedule.tick(0.0f);
        assert(!original->active());
        assert(scene->staticHeightfieldBodyCount() == 5u);
        schedule.tick(0.0f);
        assert(scene->staticHeightfieldBodyCount() == 4u);
        schedule.tick(0.0f);
        assert(scene->staticHeightfieldBodyCount() == 3u);

        // Fact, transform and binding destruction can all observe the same
        // lease. Signals hide immediately and coalesce one bounded retirement
        // intent for the following owner tick.
        auto binding = registry.get<
            runtime::StaticCollider3DBindingComponent>(entity).binding;
        const auto before = statics->snapshot();
        const auto stale = entity;
        registry.destroy(entity);
        assert(!registry.valid(stale));
        assert(!binding->active());
        const auto after_destroy = statics->snapshot();
        assert(after_destroy.immediate_hides ==
            before.immediate_hides + 1u);
        assert(after_destroy.retirement_enqueues ==
            before.retirement_enqueues);
        const auto replacement = registry.create();
        assert(entt::to_entity(replacement) == entt::to_entity(stale));
        assert(replacement != stale);
        schedule.tick(0.0f);
        assert(statics->snapshot().retirement_enqueues ==
            before.retirement_enqueues + 1u);
        assert(scene->staticHeightfieldBodyCount() == 3u);
        schedule.tick(0.0f);
        assert(scene->staticHeightfieldBodyCount() == 2u);
        schedule.tick(0.0f);
        assert(scene->staticHeightfieldBodyCount() == 1u);
        schedule.tick(0.0f);
        const auto after_retire = statics->snapshot();
        assert(after_retire.retired_batches ==
            before.retired_batches + 1u);
        assert(scene->staticHeightfieldBodyCount() == 0u);
        registry.destroy(replacement);

        // A desired entity can disappear after its operation was accepted but
        // before the main-thread completion is adopted. The complete prepared
        // batch must enter bounded retirement instead of being destructed as
        // a stale temporary on the completion callback.
        const auto stale_completion_before =
            statics->snapshot().stale_completions;
        const auto stale_completion_entity = registry.create();
        registry.emplace<lux::ecs::ResolvedTransform3DComponent>(
            stale_completion_entity, transform);
        registry.emplace<lux::ecs::StaticColliderBatch3DComponent>(
            stale_completion_entity,
            lux::ecs::StaticColliderBatch3DComponent{first.reference});
        schedule.tick(0.0f);
        const auto pending_status = statics->status(stale_completion_entity);
        assert(pending_status && pending_status->state ==
            runtime::EStaticCollider3DState::WAITING_BACKGROUND);
        registry.destroy(stale_completion_entity);
        drive(
            async_runtime,
            schedule,
            *statics,
            [&]() noexcept
            {
                return !statics->status(stale_completion_entity) &&
                    statics->snapshot().retirement_queue_size == 0u;
            },
            []() noexcept {});
        assert(statics->snapshot().stale_completions ==
            stale_completion_before + 1u);
        assert(scene->staticHeightfieldBodyCount() == 0u);

        // Rebuild one active batch, then overflow the fixed signal queue and
        // hard owner table. Overflow is counted, folded by a bounded rescan,
        // and fails closed without publishing untracked backend bodies.
        const auto close_entity = registry.create();
        registry.emplace<lux::ecs::ResolvedTransform3DComponent>(
            close_entity, transform);
        registry.emplace<lux::ecs::StaticColliderBatch3DComponent>(
            close_entity,
            lux::ecs::StaticColliderBatch3DComponent{first.reference});
        schedule.tick(0.0f);
        drive(
            async_runtime,
            schedule,
            *statics,
            [&]() noexcept
            {
                const auto status = statics->status(close_entity);
                return status && status->state ==
                    runtime::EStaticCollider3DState::ACTIVE;
            },
            []() noexcept {});
        assert(scene->staticHeightfieldBodyCount() == 3u);

        std::vector<entt::entity> overflow_entities;
        overflow_entities.reserve(20u);
        for (std::uint32_t index = 0u; index < 20u; ++index)
        {
            const auto overflow_entity = registry.create();
            registry.emplace<lux::ecs::ResolvedTransform3DComponent>(
                overflow_entity, transform);
            registry.emplace<lux::ecs::StaticColliderBatch3DComponent>(
                overflow_entity,
                lux::ecs::StaticColliderBatch3DComponent{first.reference});
            overflow_entities.push_back(overflow_entity);
        }
        schedule.tick(0.0f);
        const auto overflow_snapshot = statics->snapshot();
        assert(overflow_snapshot.observer_overflows >= 4u);
        assert(overflow_snapshot.capacity_rejections >= 17u);
        assert(overflow_snapshot.tracked_entities == 4u);
        assert(registry.view<
                   const runtime::StaticCollider3DBindingComponent>()
                   .size() <= 4u);

        // Domain-neutral close keeps all destruction on ordinary owner ticks
        // and removes transient components through the same command barrier.
        statics->requestClose();
        assert(!statics->closeComplete());
        assert(statics->closeNeedsOwnerTick());
        std::uint32_t close_previous =
            scene->staticHeightfieldBodyCount();
        lux::exec::testing::CloseEpoch close_progress{async_runtime};
        close_progress.driveWithStep(
            [&]() noexcept
            {
                schedule.tick(0.0f);
                const auto current = scene->staticHeightfieldBodyCount();
                assert(current <= close_previous);
                assert(close_previous - current <= 1u);
                close_previous = current;
            },
            [&]() noexcept
            {
                return statics->closeComplete();
            },
            [&]() noexcept
            {
                return statics->closeNeedsOwnerTick();
            });
        assert(scene->staticHeightfieldBodyCount() == 0u);
        assert(!statics->closeNeedsOwnerTick());
        assert(registry.view<
                   const runtime::StaticCollider3DBindingComponent>()
                   .empty());
        assert(registry.view<
                   const runtime::StaticCollider3DStatusComponent>()
                   .empty());
        const auto closed_snapshot = statics->snapshot();
        assert(closed_snapshot.closing);
        assert(closed_snapshot.closed);
        assert(closed_snapshot.retirement_body_count == 0u);
        assert(closed_snapshot.retirement_unit_count == 0u);
        assert(closed_snapshot.owned_budget_bytes == 0u);
        assert(scene->droppedContactFactCount() == 0u);
        assert(service.snapshot().owned_bytes == 0u);
    }

    first.owner = {};
    second.owner = {};
    first.bytes = {};
    second.bytes = {};
    blobs.pruneExpired();
    assert(blobs.snapshot().allocation_count == 0u);

    scene_scope.requestStop();
    closeOwner(async_runtime, scene_scope.closeAsync());
    service.close();
    assert(service.snapshot().closing);
    assert(service.snapshot().active_requests == 0u);
    assert(service.snapshot().owned_bytes == 0u);
    const auto close_report = lux::exec::testing::closeRuntime(async_runtime);
    assert(close_report.clean());
    return 0;
}
