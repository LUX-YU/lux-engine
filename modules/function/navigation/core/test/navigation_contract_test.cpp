#include <lux/engine/navigation/Navigation.hpp>

#include <cassert>

int
main()
{
    using namespace lux::navigation;

    NavigationPathRequest request;
    request.start = {1.0e12, 4.0, -1.0e12};
    request.destination = {1.0e12 + 0.001, 4.0, -1.0e12 + 0.001};
    request.start_region = NavigationRegionId{1u, 2u};
    request.destination_region = request.start_region;
    assert(valid(request));

    request.maximum_path_points = 0u;
    assert(!valid(request));

    NavigationPortal portal;
    portal.id = {3u, 4u};
    portal.first_region = {1u, 2u};
    portal.second_region = {5u, 6u};
    assert(portal.id.valid());
    assert(portal.first_region != portal.second_region);
    return 0;
}
