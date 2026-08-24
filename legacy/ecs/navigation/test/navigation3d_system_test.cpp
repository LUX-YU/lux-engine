#include <lux/engine/ecs/Schedule.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/navigation/components/NavigationRegion3DComponent.hpp>
#include <lux/engine/ecs/navigation/components/NavigationRegion3DStatusComponent.hpp>
#include <lux/engine/ecs/navigation/NavigationQueryService.hpp>
#include <lux/engine/ecs/navigation/systems/Navigation3DSystem.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    [[nodiscard]] lux::navigation::detour3d::NavigationRegion3DDescription
    makeGrid(lux::navigation::NavigationRegionId id)
    {
        using namespace lux::navigation::detour3d;
        NavigationRegion3DDescription result;
        result.region = id;
        constexpr std::size_t kEdge = 16u;
        result.areas.reserve(kEdge * kEdge);
        for (std::size_t z = 0u; z < kEdge; ++z)
        {
            for (std::size_t x = 0u; x < kEdge; ++x)
            {
                const auto x0 = 300'000.0 + static_cast<double>(x) * 2.0;
                const auto z0 = 200'000.0 + static_cast<double>(z) * 2.0;
                result.areas.push_back({{{x0, 8.0, z0},
                                         {x0 + 2.0, 8.0, z0},
                                         {x0 + 2.0, 8.0, z0 + 2.0},
                                         {x0, 8.0, z0 + 2.0}}});
            }
        }
        return result;
    }

    [[nodiscard]] int runWrongThreadQueryChild(bool lifecycle)
    {
        using namespace lux::ecs;
        using namespace lux::navigation::detour3d;

        auto backend = Navigation3DBackend::create();
        if (!backend)
            return 0;
        Navigation3DSystem system{*backend};
        NavigationQueryService queries{system};
        std::thread wrong_thread(
            [&]
            {
                if (lifecycle)
                    (void)queries.lifecycleSnapshot();
                else
                    (void)queries.query({});
            });
        wrong_thread.join();
        return 0;
    }
} // namespace

int main(int argc, char** argv)
{
    using namespace lux::ecs;
    using namespace lux::navigation;
    using namespace lux::navigation::detour3d;

    constexpr std::string_view query_child =
        "--wrong-thread-query-child";
    constexpr std::string_view lifecycle_child =
        "--wrong-thread-lifecycle-child";
    if (argc == 2 && std::string_view{argv[1]} == query_child)
        return runWrongThreadQueryChild(false);
    if (argc == 2 && std::string_view{argv[1]} == lifecycle_child)
        return runWrongThreadQueryChild(true);

    const auto runChild = [argv](std::string_view argument)
    {
        const std::string command = std::string{"\""} + argv[0] +
            "\" " + std::string{argument};
        const int result = std::system(command.c_str());
        return result != -1 && result != 0;
    };
    assert(runChild(query_child));
    assert(runChild(lifecycle_child));

    NavigationRegion3DDescription description;
    description.region = {19u, 23u};
    description.areas.push_back({{{100'000.0, 8.0, 75'000.0},
                                  {100'064.0, 8.0, 75'000.0},
                                  {100'064.0, 8.0, 75'064.0},
                                  {100'000.0, 8.0, 75'064.0}}});
    const auto encoded = encodeNavigationRegion3D(description);
    assert(encoded);

    auto backend_result = Navigation3DBackend::create();
    assert(backend_result);
    auto backend = *backend_result;

    World world;
    auto& registry = world.registry();
    const auto entity = registry.create();
    lux::ecs::scene_format::ContentBlobRef content;
    content.id.digest[0] = std::byte{1u};
    content.type = lux::ecs::scene_format::ContentTypeId{
        std::string{kNavigationRegion3DContentTypeName}};
    content.schema_version = kNavigationRegion3DSchemaVersion;
    registry.emplace<NavigationRegion3DComponent>(
        entity, NavigationRegion3DComponent{content});

    Schedule schedule{world};
    auto installed = schedule.addSystem(std::make_unique<Navigation3DSystem>(
        backend,
        Navigation3DSystemConfig{
            .maximum_pending_requests = 1u,
            .maximum_pending_completions = 1u,
            .maximum_staging_granules_per_tick = 1u,
            .maximum_retirement_granules_per_tick = 1u}));
    assert(installed);
    auto* system = schedule.get(*installed);
    assert(system != nullptr);

    // Existing authored facts fold into the same deferred reconcile path.
    schedule.tick(0.0f);
    assert(registry.all_of<NavigationRegion3DStatusComponent>(entity));
    assert(registry.get<NavigationRegion3DStatusComponent>(entity).state ==
           ENavigationRegion3DState::WAITING_BACKGROUND);
    schedule.tick(0.0f);
    const auto requests = system->pendingPreparationRequests();
    assert(requests.size() == 1u);
    assert(requests.front().entity == entity);
    const auto request_generation = requests.front().generation;
    const auto first_request_count = requests.size();

    NavigationQueryService queries{*system};
    NavigationPathRequest pending_request;
    pending_request.start = {100'004.0, 8.0, 75'004.0};
    pending_request.destination = {100'060.0, 8.0, 75'060.0};
    pending_request.start_region = description.region;
    pending_request.destination_region = description.region;
    const auto pending = queries.query(pending_request);
    assert(pending.status == ENavigationPathStatus::PENDING);
    assert(pending.missing_regions.size() == 1u);
    assert(pending.missing_regions.front() == description.region);

    // The request window is bounded.  A second authored region remains
    // pending until the adapter admits and consumes the first request.
    const auto second_entity = registry.create();
    auto second_content = content;
    second_content.id.digest[0] = std::byte{2u};
    registry.emplace<NavigationRegion3DComponent>(
        second_entity, NavigationRegion3DComponent{second_content});
    schedule.tick(0.0f);
    schedule.tick(0.0f);
    assert(system->pendingPreparationRequests().size() == 1u);
    assert(system->snapshot().queue_backpressure > 0u);
    system->consumePreparationRequests(first_request_count);
    schedule.tick(0.0f);
    const auto second_request = system->pendingPreparationRequests();
    assert(second_request.size() == 1u);
    assert(second_request.front().entity == second_entity);
    system->consumePreparationRequests(second_request.size());
    assert(system->pendingPreparationRequests().empty());

    auto prepared = prepareNavigationRegion3D(*encoded, request_generation);
    assert(prepared);
    assert(system->acceptPrepared(
        entity, request_generation, std::move(*prepared)));
    assert(system->status(entity)->state == ENavigationRegion3DState::STAGING);

    schedule.tick(0.0f);
    assert(registry.get<NavigationRegion3DStatusComponent>(entity).state ==
           ENavigationRegion3DState::ACTIVE);
    assert(backend->snapshot().active_regions == 1u);
    const auto active_lifecycle = queries.lifecycleSnapshot();
    assert(active_lifecycle.generation == backend->snapshot().generation);
    assert(active_lifecycle.active_regions == 1u);
    assert(active_lifecycle.owner_bytes > 0u);

    NavigationPathRequest path_request;
    path_request.start = {100'004.0, 8.0, 75'004.0};
    path_request.destination = {100'060.0, 8.0, 75'060.0};
    path_request.start_region = description.region;
    path_request.destination_region = description.region;
    assert(queries.query(path_request).status ==
           ENavigationPathStatus::COMPLETE);

    // Full ContentBlobRef identity is authoritative.  A type-only patch with
    // the same digest/schema is invalid and must hide the old backend state
    // instead of being mistaken for a no-op.
    registry.patch<NavigationRegion3DComponent>(
        entity,
        [](NavigationRegion3DComponent& component)
        {
            component.content.type = lux::ecs::scene_format::ContentTypeId{
                std::string{"lux.navigation.unsupported"}};
        });
    schedule.tick(0.0f);
    assert(system->status(entity)->state == ENavigationRegion3DState::FAILED);
    assert(system->status(entity)->failure ==
           ENavigationRegion3DFailureCode::INVALID_REFERENCE);
    assert(backend->snapshot().active_regions == 0u);

    registry.patch<NavigationRegion3DComponent>(
        entity,
        [&](NavigationRegion3DComponent& component)
        { component.content = content; });
    schedule.tick(0.0f);
    schedule.tick(0.0f);
    const auto retry_requests = system->pendingPreparationRequests();
    assert(retry_requests.size() == 1u);
    const auto retry_generation = retry_requests.front().generation;
    system->consumePreparationRequests(retry_requests.size());
    auto retry_prepared =
        prepareNavigationRegion3D(*encoded, retry_generation);
    assert(retry_prepared);
    assert(system->acceptPrepared(
        entity, retry_generation, std::move(*retry_prepared)));
    schedule.tick(0.0f);
    assert(system->status(entity)->state == ENavigationRegion3DState::ACTIVE);

    const auto stale_generation = retry_generation;
    const auto stale_slot = entt::to_entity(entity);
    registry.destroy(entity);
    const auto reused_entity = registry.create();
    assert(entt::to_entity(reused_entity) == stale_slot);
    assert(reused_entity != entity);
    registry.emplace<NavigationRegion3DComponent>(
        reused_entity, NavigationRegion3DComponent{content});
    registry.destroy(second_entity);
    schedule.tick(0.0f);
    assert(backend->snapshot().active_regions == 0u);
    schedule.tick(0.0f);
    assert(backend->snapshot().owned_bytes == 0u);
    auto stale = prepareNavigationRegion3D(*encoded, stale_generation);
    assert(stale);
    assert(
        !system->acceptPrepared(entity, stale_generation, std::move(*stale)));
    assert(system->status(reused_entity));
    assert(system->status(reused_entity)->generation != stale_generation);
    system->consumePreparationRequests(
        system->pendingPreparationRequests().size());
    registry.destroy(reused_entity);
    schedule.tick(0.0f);

    // A multi-layer region is never made queryable until every layer has
    // staged. Both staging and physical retirement consume at most one
    // provider-defined granule in each Schedule tick.
    const auto grid = makeGrid({41u, 43u});
    const auto grid_encoded = encodeNavigationRegion3D(grid);
    assert(grid_encoded);
    auto grid_content = content;
    grid_content.id.digest[0] = std::byte{3u};
    const auto grid_entity = registry.create();
    registry.emplace<NavigationRegion3DComponent>(
        grid_entity, NavigationRegion3DComponent{grid_content});
    schedule.tick(0.0f);
    schedule.tick(0.0f);
    const auto grid_requests = system->pendingPreparationRequests();
    assert(grid_requests.size() == 1u);
    const auto grid_generation = grid_requests.front().generation;
    system->consumePreparationRequests(grid_requests.size());
    auto grid_prepared =
        prepareNavigationRegion3D(*grid_encoded, grid_generation);
    assert(grid_prepared);
    assert(grid_prepared->granuleCount() == 4u);
    assert(system->acceptPrepared(
        grid_entity, grid_generation, std::move(*grid_prepared)));

    NavigationPathRequest grid_query;
    grid_query.start = {300'001.0, 8.0, 200'001.0};
    grid_query.destination = {300'031.0, 8.0, 200'031.0};
    grid_query.start_region = grid.region;
    grid_query.destination_region = grid.region;
    for (std::uint32_t index = 0u; index < 3u; ++index)
    {
        const auto before = system->snapshot().staging_work_items;
        schedule.tick(0.0f);
        const auto after = system->snapshot();
        assert(after.staging_work_items - before <= 1u);
        assert(after.maximum_staging_work_items_per_tick <= 1u);
        assert(queries.query(grid_query).status ==
               ENavigationPathStatus::PENDING);
    }
    const auto before_publish = backend->snapshot().generation;
    schedule.tick(0.0f);
    assert(system->status(grid_entity)->state ==
           ENavigationRegion3DState::ACTIVE);
    assert(backend->snapshot().generation == before_publish + 1u);
    assert(queries.query(grid_query).status ==
           ENavigationPathStatus::COMPLETE);

    registry.destroy(grid_entity);
    schedule.tick(0.0f);
    const auto hidden = queries.query(grid_query);
    assert(hidden.status == ENavigationPathStatus::PENDING);
    assert(hidden.missing_regions ==
           std::vector<NavigationRegionId>{grid.region});
    while (backend->snapshot().owned_bytes != 0u)
    {
        const auto before = system->snapshot().retirement_work_items;
        schedule.tick(0.0f);
        const auto after = system->snapshot();
        assert(after.retirement_work_items - before <= 1u);
        assert(after.maximum_retirement_work_items_per_tick <= 1u);
    }

    // A completion accepted by the external operation after close begins is
    // stale and cannot resurrect ownership. The close driver uses ordinary
    // ticks/barriers until every entry and byte is gone.
    const auto closing_entity = registry.create();
    registry.emplace<NavigationRegion3DComponent>(
        closing_entity, NavigationRegion3DComponent{content});
    for (std::uint32_t index = 0u; index < 3u; ++index)
    {
        const auto extra = registry.create();
        auto extra_content = content;
        extra_content.id.digest[1] = std::byte{
            static_cast<std::uint8_t>(index + 1u)};
        registry.emplace<NavigationRegion3DComponent>(
            extra, NavigationRegion3DComponent{extra_content});
    }
    schedule.tick(0.0f);
    schedule.tick(0.0f);
    const auto closing_requests = system->pendingPreparationRequests();
    assert(closing_requests.size() == 1u);
    const auto closing_generation = closing_requests.front().generation;
    system->consumePreparationRequests(closing_requests.size());
    auto late = prepareNavigationRegion3D(
        *grid_encoded, closing_generation);
    auto backpressured_late = prepareNavigationRegion3D(
        *grid_encoded, closing_generation);
    assert(late && backpressured_late);
    const auto late_granules = late->granuleCount();
    assert(late_granules == 4u);
    schedule.requestClose();
    const auto closing_state = schedule.closeState();
    assert(closing_state.valid && !closing_state.complete &&
           closing_state.owner_work_pending);
    assert(!system->acceptPrepared(
        closing_entity, closing_generation, std::move(*late)));
    // The discard window is bounded by maximum_pending_completions. A second
    // stale material result remains owned by its producer until the first
    // four granules have retired and a slot becomes available.
    assert(!system->acceptPrepared(
        closing_entity,
        closing_generation,
        std::move(*backpressured_late)));
    assert(backpressured_late->valid());
    assert(system->snapshot().owner_bytes > 0u);
    const auto retirement_before_close =
        system->snapshot().retirement_work_items;
    bool backpressured_consumed = false;
    for (std::size_t attempt = 0u;
         attempt < 32u &&
             (!backpressured_consumed || !system->closeComplete());
         ++attempt)
    {
        const auto before = system->snapshot().close_hides;
        const auto retired_before =
            system->snapshot().retirement_work_items;
        schedule.tick(0.0f);
        const auto after = system->snapshot();
        assert(after.close_hides - before <= 1u);
        assert(after.retirement_work_items - retired_before <= 1u);
        assert(after.maximum_close_hides_per_tick <= 1u);
        assert(after.maximum_retirement_work_items_per_tick <= 1u);
        if (!backpressured_consumed)
        {
            assert(!system->acceptPrepared(
                closing_entity,
                closing_generation,
                std::move(*backpressured_late)));
            backpressured_consumed = !backpressured_late->valid();
        }
    }
    assert(backpressured_consumed);
    assert(system->closeComplete());
    assert(system->snapshot().retirement_work_items -
               retirement_before_close == late_granules * 2u);
    assert(schedule.closeState().complete);
    assert(system->snapshot().owner_bytes == 0u);
    assert(queries.lifecycleSnapshot().owner_bytes == 0u);
    assert(backend->snapshot().owned_bytes == 0u);
    const auto removed = schedule.removeSystem(*installed);
    assert(removed);
    return 0;
}
