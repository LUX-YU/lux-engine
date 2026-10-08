#include <lux/engine/navigation/detour3d/NavigationDetour3D.hpp>

#include <algorithm>
#include <cassert>
#include <cstdio>

namespace
{
    using namespace lux::navigation;
    using namespace lux::navigation::detour3d;

    NavigationRegion3DDescription region(std::uint64_t id, double offset)
    {
        NavigationRegion3DDescription result;
        result.region = {0, id};
        result.areas.push_back({{{offset, 0, 0}, {offset, 0, 9}, {offset + 9, 0, 9}, {offset + 9, 0, 0}}});
        return result;
    }

    std::unique_ptr<NavigationRegion3DLease> publish(
        Navigation3DBackend& backend,
        const NavigationRegion3DDescription& description
    )
    {
        auto blob = encodeNavigationRegion3D(description);
        assert(blob);
        auto prepared = prepareNavigationRegion3D(std::move(*blob), 1);
        assert(prepared);
        auto lease = backend.adoptPrepared(std::move(*prepared));
        assert(lease);
        for (int step = 0; (*lease)->state() == ENavigationRegion3DLeaseState::STAGING; ++step)
        {
            assert(step < 16);
            assert((*lease)->advancePreparationOne());
        }
        assert((*lease)->publish());
        return std::move(*lease);
    }
} // namespace

int main()
{
    auto backend = Navigation3DBackend::create();
    assert(backend);
    auto first = region(1, 0);
    auto middle = region(2, 12);
    auto last = region(3, 24);
    first.portals.push_back({{0, 1}, first.region, middle.region, {6, 0, 3}, {15, 0, 3}});
    middle.portals.push_back({{0, 2}, middle.region, last.region, {18, 0, 3}, {27, 0, 3}});
    // A directed back edge creates a cycle without replacing the two-hop forward path.
    last.portals.push_back({{0, 3}, last.region, first.region, {27, 0, 6}, {3, 0, 6}, 1.0f, false});
    auto first_owner = publish(**backend, first);
    auto middle_owner = publish(**backend, middle);
    auto last_owner = publish(**backend, last);
    NavigationPathRequest request{
        .start = {3, 0, 3},
        .destination = {30, 0, 3},
        .start_region = first.region,
        .destination_region = last.region
    };
    auto result = (*backend)->query(request);
    if (result.status != ENavigationPathStatus::COMPLETE)
    {
        std::fprintf(stderr, "route failed: %s\n", result.detail.c_str());
    }
    assert(result.status == ENavigationPathStatus::COMPLETE);
    assert(result.failure == ENavigationPathFailure::NONE);
    assert(result.points.size() >= 6);
    assert(result.missing_regions.empty());

    assert(middle_owner->hide());
    auto pending = (*backend)->query(request);
    assert(pending.status == ENavigationPathStatus::PENDING);
    assert(pending.missing_regions == std::vector<NavigationRegionId>{middle.region});
    for (int step = 0; middle_owner->state() != ENavigationRegion3DLeaseState::RETIRED; ++step)
    {
        assert(step < 16);
        assert(middle_owner->advanceRetirementOne());
    }
    middle_owner = publish(**backend, middle);
    assert((*backend)->query(request).status == ENavigationPathStatus::COMPLETE);

    auto isolated = region(4, 36);
    auto isolated_owner = publish(**backend, isolated);
    request.destination = {39, 0, 3};
    request.destination_region = isolated.region;
    auto disconnected = (*backend)->query(request);
    assert(disconnected.status == ENavigationPathStatus::FAILED);
    assert(disconnected.failure == ENavigationPathFailure::LOCATION_NOT_FOUND);
    request.destination = {6, 0, 3};
    request.destination_region = first.region;
    assert((*backend)->query(request).status == ENavigationPathStatus::COMPLETE);

    isolated_owner.reset();
    last_owner.reset();
    middle_owner.reset();
    first_owner.reset();
    for (int step = 0; (*backend)->snapshot().retiring_regions != 0; ++step)
    {
        assert(step < 32);
        assert((*backend)->advanceRetirementOne());
    }
    assert((*backend)->snapshot().owned_bytes == 0);
    std::puts("PASS real Detour route: multi-hop, directed cycle, missing middle, restore, disconnected and same region"
    );
}
