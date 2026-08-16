#include <lux/engine/navigation/detour3d/NavigationDetour3D.hpp>

#include <array>
#include <barrier>
#include <cassert>
#include <cstddef>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using namespace lux::navigation;
    using namespace lux::navigation::detour3d;

    [[nodiscard]] NavigationRegion3DDescription squareRegion(
        NavigationRegionId region,
        double minimum_x,
        double minimum_z)
    {
        NavigationRegion3DDescription result;
        result.region = region;
        result.areas.push_back({{{minimum_x, 20.0, minimum_z},
                                 {minimum_x + 64.0, 20.0, minimum_z},
                                 {minimum_x + 64.0,
                                  20.0,
                                  minimum_z + 64.0},
                                 {minimum_x, 20.0, minimum_z + 64.0}}});
        return result;
    }

    void stage(NavigationRegion3DLease& lease)
    {
        while (lease.state() == ENavigationRegion3DLeaseState::STAGING)
        {
            const auto step = lease.advancePreparationOne();
            assert(step);
            assert(step->work_items == 1u);
        }
        assert(lease.state() == ENavigationRegion3DLeaseState::READY);
    }

    void retire(NavigationRegion3DLease& lease)
    {
        if (lease.state() != ENavigationRegion3DLeaseState::RETIRING)
            assert(lease.beginRetirement());
        while (lease.state() != ENavigationRegion3DLeaseState::RETIRED)
        {
            const auto step = lease.advanceRetirementOne();
            assert(step);
            assert(step->work_items == 1u);
        }
    }
} // namespace

int main()
{
    using namespace lux::navigation;
    using namespace lux::navigation::detour3d;

    auto description = squareRegion({7u, 11u}, 1.0e9, -1.0e9);
    const auto encoded = encodeNavigationRegion3D(description);
    assert(encoded);

    using PrepareResult = decltype(prepareNavigationRegion3D(*encoded, 9u));
    std::optional<PrepareResult> worker_result;
    std::thread worker(
        [&]
        { worker_result.emplace(prepareNavigationRegion3D(*encoded, 9u)); });
    worker.join();
    assert(worker_result && *worker_result);
    assert((*worker_result)->region() == description.region);
    assert((*worker_result)->requestGeneration() == 9u);
    assert((*worker_result)->granuleCount() == 1u);

    auto backend_result = Navigation3DBackend::create();
    assert(backend_result);
    auto backend = *backend_result;
    auto lease_result = backend->adoptPrepared(std::move(**worker_result));
    assert(lease_result);
    auto lease = std::move(*lease_result);
    assert(backend->snapshot().staged_regions == 1u);

    NavigationPathRequest request;
    request.start = {1.0e9 + 4.0, 20.0, -1.0e9 + 4.0};
    request.destination = {1.0e9 + 60.0, 20.0, -1.0e9 + 60.0};
    request.start_region = description.region;
    request.destination_region = description.region;
    const auto pending = backend->query(request);
    assert(pending.status == ENavigationPathStatus::PENDING);
    assert(pending.missing_regions ==
           std::vector<NavigationRegionId>{description.region});

    stage(*lease);
    const auto generation_before_publish = backend->snapshot().generation;
    assert(lease->publish());
    const auto active = backend->snapshot();
    assert(active.generation == generation_before_publish + 1u);
    assert(active.active_regions == 1u);
    assert(active.staged_regions == 0u);
    assert(active.owned_bytes > 0u);
    const auto path = backend->query(request);
    assert(path.status == ENavigationPathStatus::COMPLETE);
    assert(path.points.size() >= 2u);

    assert(lease->hide());
    assert(backend->query(request).status == ENavigationPathStatus::PENDING);
    assert(backend->snapshot().retiring_regions == 1u);
    retire(*lease);
    const auto empty = backend->snapshot();
    assert(empty.active_regions == 0u);
    assert(empty.retiring_regions == 0u);
    assert(empty.owned_bytes == 0u);

    auto corrupt = *encoded;
    corrupt.region = {13u, 17u};
    const auto rejected = prepareNavigationRegion3D(std::move(corrupt), 10u);
    assert(!rejected);
    assert(rejected.error().code == ENavigationRegion3DError::INVALID_CONTENT);

    // A known but unpublished region produces an actionable PENDING result;
    // after the lease is relinquished the owner retires it one granule at a
    // time and absence becomes LOCATION_NOT_FOUND.
    auto known_prepared = prepareNavigationRegion3D(*encoded, 11u);
    assert(known_prepared);
    auto known_lease_result =
        backend->adoptPrepared(std::move(*known_prepared));
    assert(known_lease_result);
    auto known_lease = std::move(*known_lease_result);
    NavigationPathRequest inferred_request = request;
    inferred_request.start_region.reset();
    inferred_request.destination_region.reset();
    const auto known_pending = backend->query(inferred_request);
    assert(known_pending.status == ENavigationPathStatus::PENDING);
    assert(known_pending.missing_regions ==
           std::vector<NavigationRegionId>{description.region});
    known_lease->reset();
    assert(backend->snapshot().retiring_regions == 1u);
    const auto owner_retirement = backend->advanceRetirementOne();
    assert(owner_retirement && owner_retirement->work_items == 1u);
    const auto unknown = backend->query(inferred_request);
    assert(unknown.status == ENavigationPathStatus::FAILED);
    assert(unknown.failure == ENavigationPathFailure::LOCATION_NOT_FOUND);
    assert(unknown.missing_regions.empty());

    // Query snapshots may race logical hide. Every result linearizes either
    // before hide (COMPLETE) or after it (actionable PENDING).
    auto concurrent_prepared = prepareNavigationRegion3D(*encoded, 13u);
    assert(concurrent_prepared);
    auto concurrent_result =
        backend->adoptPrepared(std::move(*concurrent_prepared));
    assert(concurrent_result);
    auto concurrent_lease = std::move(*concurrent_result);
    stage(*concurrent_lease);
    assert(concurrent_lease->publish());
    constexpr std::size_t kQueryThreads = 4u;
    std::barrier start_queries{static_cast<std::ptrdiff_t>(kQueryThreads + 1u)};
    std::array<ENavigationPathStatus, kQueryThreads> statuses{};
    std::vector<std::thread> query_threads;
    query_threads.reserve(kQueryThreads);
    for (std::size_t index = 0u; index < kQueryThreads; ++index)
    {
        query_threads.emplace_back(
            [&, index]
            {
                start_queries.arrive_and_wait();
                statuses[index] = backend->query(request).status;
            });
    }
    start_queries.arrive_and_wait();
    assert(concurrent_lease->hide());
    for (auto& query_thread : query_threads)
        query_thread.join();
    for (const auto status : statuses)
    {
        assert(status == ENavigationPathStatus::COMPLETE ||
               status == ENavigationPathStatus::PENDING);
    }
    retire(*concurrent_lease);

    // Two independent regions stitch through a semantic portal. A staged
    // destination is precisely actionable; publication makes the route
    // COMPLETE, and a point budget truncates the same route to PARTIAL.
    auto first = squareRegion({21u, 31u}, 0.0, 0.0);
    auto second = squareRegion({22u, 32u}, 64.0, 0.0);
    auto third = squareRegion({23u, 33u}, 128.0, 0.0);
    NavigationPortal portal;
    portal.id = {71u, 81u};
    portal.first_region = first.region;
    portal.second_region = second.region;
    portal.first_position = {60.0, 20.0, 32.0};
    portal.second_position = {68.0, 20.0, 32.0};
    first.portals.push_back(portal);
    second.portals.push_back(portal);
    NavigationPortal second_portal;
    second_portal.id = {72u, 82u};
    second_portal.first_region = second.region;
    second_portal.second_region = third.region;
    second_portal.first_position = {124.0, 20.0, 32.0};
    second_portal.second_position = {132.0, 20.0, 32.0};
    second.portals.push_back(second_portal);
    third.portals.push_back(second_portal);
    auto first_blob = encodeNavigationRegion3D(first);
    auto second_blob = encodeNavigationRegion3D(second);
    auto third_blob = encodeNavigationRegion3D(third);
    assert(first_blob && second_blob && third_blob);
    auto first_prepared = prepareNavigationRegion3D(*first_blob, 20u);
    auto second_prepared = prepareNavigationRegion3D(*second_blob, 21u);
    auto third_prepared = prepareNavigationRegion3D(*third_blob, 22u);
    assert(first_prepared && second_prepared && third_prepared);
    auto second_lease_result =
        backend->adoptPrepared(std::move(*second_prepared));
    auto first_lease_result =
        backend->adoptPrepared(std::move(*first_prepared));
    auto third_lease_result =
        backend->adoptPrepared(std::move(*third_prepared));
    assert(first_lease_result && second_lease_result && third_lease_result);
    auto first_lease = std::move(*first_lease_result);
    auto second_lease = std::move(*second_lease_result);
    auto third_lease = std::move(*third_lease_result);
    stage(*first_lease);
    assert(first_lease->publish());

    NavigationPathRequest cross;
    cross.start = {4.0, 20.0, 32.0};
    cross.destination = {124.0, 20.0, 32.0};
    cross.start_region = first.region;
    cross.destination_region = second.region;
    const auto cross_pending = backend->query(cross);
    assert(cross_pending.status == ENavigationPathStatus::PENDING);
    assert(cross_pending.missing_regions ==
           std::vector<NavigationRegionId>{second.region});

    stage(*second_lease);
    const auto cross_generation = backend->snapshot().generation;
    assert(second_lease->publish());
    assert(backend->snapshot().generation == cross_generation + 1u);
    const auto complete_cross = backend->query(cross);
    assert(complete_cross.status == ENavigationPathStatus::COMPLETE);
    assert(complete_cross.points.size() >= 4u);
    cross.maximum_path_points = 2u;
    const auto partial_cross = backend->query(cross);
    assert(partial_cross.status == ENavigationPathStatus::PARTIAL);
    assert(partial_cross.failure == ENavigationPathFailure::NONE);
    stage(*third_lease);
    assert(third_lease->publish());
    NavigationPathRequest stitched = cross;
    stitched.destination = {188.0, 20.0, 32.0};
    stitched.destination_region = third.region;
    stitched.maximum_path_points = 32u;
    assert(backend->query(stitched).status ==
           ENavigationPathStatus::COMPLETE);
    assert(second_lease->hide());
    const auto missing_middle = backend->query(stitched);
    assert(missing_middle.status == ENavigationPathStatus::PENDING);
    assert(missing_middle.missing_regions ==
           std::vector<NavigationRegionId>{second.region});
    retire(*second_lease);
    retire(*first_lease);
    retire(*third_lease);

    // A dense region is split into real backend layers. Each owner step
    // installs or removes exactly one layer and the byte ledger reaches zero.
    NavigationRegion3DDescription grid;
    grid.region = {29u, 31u};
    constexpr std::size_t kGridEdge = 32u;
    grid.areas.reserve(kGridEdge * kGridEdge);
    for (std::size_t z = 0u; z < kGridEdge; ++z)
    {
        for (std::size_t x = 0u; x < kGridEdge; ++x)
        {
            const auto x0 = 500'000.0 + static_cast<double>(x) * 2.0;
            const auto z0 = 250'000.0 + static_cast<double>(z) * 2.0;
            grid.areas.push_back({{{x0, 3.0, z0},
                                   {x0 + 2.0, 3.0, z0},
                                   {x0 + 2.0, 3.0, z0 + 2.0},
                                   {x0, 3.0, z0 + 2.0}}});
        }
    }
    const auto grid_encoded = encodeNavigationRegion3D(grid);
    assert(grid_encoded);
    auto grid_prepared = prepareNavigationRegion3D(*grid_encoded, 14u);
    assert(grid_prepared);
    assert(grid_prepared->granuleCount() == 16u);
    auto grid_lease_result = backend->adoptPrepared(std::move(*grid_prepared));
    assert(grid_lease_result);
    auto grid_lease = std::move(*grid_lease_result);
    assert(backend->snapshot().staged_granules == 16u);
    std::uint32_t staged_steps = 0u;
    while (grid_lease->state() == ENavigationRegion3DLeaseState::STAGING)
    {
        const auto step = grid_lease->advancePreparationOne();
        assert(step && step->work_items == 1u);
        ++staged_steps;
        assert(backend->snapshot().staged_granules ==
               16u - staged_steps);
    }
    assert(staged_steps == 16u);
    assert(grid_lease->publish());
    const auto grid_owned_bytes = backend->snapshot().owned_bytes;
    assert(grid_owned_bytes > 0u);
    assert(grid_lease->hide());
    std::uint32_t retired_steps = 0u;
    while (grid_lease->state() != ENavigationRegion3DLeaseState::RETIRED)
    {
        const auto step = grid_lease->advanceRetirementOne();
        assert(step && step->work_items == 1u);
        ++retired_steps;
        assert(backend->snapshot().retiring_granules ==
               16u - retired_steps);
    }
    assert(retired_steps == 16u);
    assert(backend->snapshot().owned_bytes == 0u);
    return 0;
}
