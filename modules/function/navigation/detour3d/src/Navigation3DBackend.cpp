#include <lux/engine/navigation/detour3d/detail/NavigationDetour3DInternal.hpp>
#include <lux/engine/math/RelativePosition.hpp>

#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::navigation::detour3d
{
    namespace
    {
        constexpr std::uint32_t kMaximumPathNodes = 2048u;
        constexpr std::uint32_t kMaximumPathPoints = 65'536u;

        using RegionData = PreparedNavigationRegion3D::Data;
        using RegionMap = std::unordered_map<NavigationRegionId, std::shared_ptr<RegionData>, detail::RegionIdHash>;

        struct QueryPinBatch final
        {
            ~QueryPinBatch() noexcept
            {
                for (const auto& region : regions)
                {
                    const auto previous = region->query_readers.fetch_sub(1u, std::memory_order_release);
                    if (previous == 0u)
                        std::abort();
                }
            }

            std::vector<std::shared_ptr<RegionData>> regions;
        };

        [[nodiscard]] std::uint32_t remainingStagingGranules(const RegionData& data) noexcept
        {
            return static_cast<std::uint32_t>(data.layers.size() - data.staging_cursor);
        }

        [[nodiscard]] std::uint32_t remainingRetirementGranules(const RegionData& data) noexcept
        {
            return static_cast<std::uint32_t>(data.layers.size() - data.retirement_cursor);
        }

        [[nodiscard]] NavigationPathResult queryRegion(
            const RegionData& region,
            const lux::math::Position3d& start_position,
            const lux::math::Position3d& destination_position,
            const NavigationPathRequest& request,
            float maximum_relative_extent) noexcept
        {
            NavigationPathResult result;
            if (!detail::sameAgent(region.agent, request.agent))
            {
                result.failure = ENavigationPathFailure::UNSUPPORTED_AGENT;
                result.detail = "navigation region was built for another agent";
                return result;
            }
            const auto start = lux::math::relativeFloat(start_position, region.origin, maximum_relative_extent);
            const auto destination =
                lux::math::relativeFloat(destination_position, region.origin, maximum_relative_extent);
            if (!start || !destination)
            {
                result.failure = ENavigationPathFailure::INVALID_REQUEST;
                result.detail = "navigation request exceeds the backend origin extent";
                return result;
            }

            std::lock_guard query_lock{region.query_mutex};
            dtQueryFilter filter;
            filter.setIncludeFlags(0xffffu);
            const float extents[3]{
                request.nearest_horizontal_extent,
                request.nearest_vertical_extent,
                request.nearest_horizontal_extent};
            dtPolyRef start_reference = 0u;
            dtPolyRef destination_reference = 0u;
            float nearest_start[3]{};
            float nearest_destination[3]{};
            const bool has_nearest_start = !dtStatusFailed(
                region.query->findNearestPoly(start->data(), extents, &filter, &start_reference, nearest_start)
            );
            const bool has_nearest_destination = has_nearest_start && !dtStatusFailed(
                region.query->findNearestPoly(
                    destination->data(),
                    extents,
                    &filter,
                    &destination_reference,
                    nearest_destination
                )
            );
            const bool has_valid_references = has_nearest_destination &&
                start_reference != 0u && destination_reference != 0u;
            if (!has_valid_references)
            {
                result.failure = ENavigationPathFailure::LOCATION_NOT_FOUND;
                result.detail = "navigation endpoint is outside traversable content";
                return result;
            }

            std::vector<dtPolyRef> route(request.maximum_path_nodes);
            int route_count = 0;
            const auto route_status = region.query->findPath(
                start_reference,
                destination_reference,
                nearest_start,
                nearest_destination,
                &filter,
                route.data(),
                &route_count,
                static_cast<int>(route.size()));
            if (dtStatusFailed(route_status) || route_count <= 0)
            {
                result.failure = ENavigationPathFailure::BACKEND_FAILURE;
                result.detail = "navigation search failed";
                return result;
            }

            std::vector<float> point_values(static_cast<std::size_t>(request.maximum_path_points) * 3u);
            int point_count = 0;
            const auto straight_status = region.query->findStraightPath(
                nearest_start,
                nearest_destination,
                route.data(),
                route_count,
                point_values.data(),
                nullptr,
                nullptr,
                &point_count,
                static_cast<int>(request.maximum_path_points));
            if (dtStatusFailed(straight_status) || point_count <= 0)
            {
                result.failure = ENavigationPathFailure::BACKEND_FAILURE;
                result.detail = "navigation path reconstruction failed";
                return result;
            }
            result.points.reserve(static_cast<std::size_t>(point_count));
            for (int index = 0; index < point_count; ++index)
            {
                result.points.push_back(
                    {region.origin.x + point_values[index * 3],
                     region.origin.y + point_values[index * 3 + 1],
                     region.origin.z + point_values[index * 3 + 2]});
            }
            const bool partial = dtStatusDetail(route_status, DT_PARTIAL_RESULT) ||
                                 route[static_cast<std::size_t>(route_count - 1)] != destination_reference;
            result.status = partial ? ENavigationPathStatus::PARTIAL : ENavigationPathStatus::COMPLETE;
            result.failure = ENavigationPathFailure::NONE;
            return result;
        }

        struct PortalTraversal final
        {
            NavigationRegionId from;
            NavigationRegionId to;
            lux::math::Position3d from_position;
            lux::math::Position3d to_position;
        };

        struct PortalPredecessor final
        {
            bool present{false};
            PortalTraversal traversal;
        };

        [[nodiscard]] bool samePortal(const NavigationPortal& left, const NavigationPortal& right) noexcept
        {
            return left.id == right.id && left.first_region == right.first_region &&
                   left.second_region == right.second_region && left.first_position == right.first_position &&
                   left.second_position == right.second_position &&
                   left.traversal_cost_scale == right.traversal_cost_scale && left.bidirectional == right.bidirectional;
        }

        [[nodiscard]] bool portalLess(const NavigationPortal& left, const NavigationPortal& right) noexcept
        {
            if (left.id.high != right.id.high)
                return left.id.high < right.id.high;
            if (left.id.low != right.id.low)
                return left.id.low < right.id.low;
            if (left.first_region != right.first_region)
                return left.first_region < right.first_region;
            return left.second_region < right.second_region;
        }

        [[nodiscard]] std::optional<std::vector<PortalTraversal>> findPortalRoute(
            NavigationRegionId start,
            NavigationRegionId destination,
            const std::vector<NavigationPortal>& portals,
            const RegionMap* required_active) noexcept
        {
            std::unordered_map<NavigationRegionId, PortalPredecessor, detail::RegionIdHash> predecessors;
            std::deque<NavigationRegionId> frontier;
            predecessors.emplace(start, PortalPredecessor{});
            frontier.push_back(start);
            while (!frontier.empty() && !predecessors.contains(destination))
            {
                const auto current = frontier.front();
                frontier.pop_front();
                for (const auto& portal : portals)
                {
                    PortalTraversal traversal;
                    if (portal.first_region == current)
                    {
                        traversal = {current, portal.second_region, portal.first_position, portal.second_position};
                    }
                    else if (portal.second_region == current && portal.bidirectional)
                    {
                        traversal = {current, portal.first_region, portal.second_position, portal.first_position};
                    }
                    else
                    {
                        continue;
                    }
                    if (required_active && !required_active->contains(traversal.to))
                    {
                        continue;
                    }
                    if (predecessors.contains(traversal.to))
                        continue;
                    predecessors.emplace(traversal.to, PortalPredecessor{true, traversal});
                    frontier.push_back(traversal.to);
                }
            }
            if (!predecessors.contains(destination))
                return std::nullopt;

            std::vector<PortalTraversal> result;
            for (auto current = destination; current != start;)
            {
                const auto found = predecessors.find(current);
                if (found == predecessors.end() || !found->second.present)
                    std::abort();
                result.push_back(found->second.traversal);
                current = found->second.traversal.from;
            }
            std::ranges::reverse(result);
            return result;
        }

        void appendPoints(
            NavigationPathResult& destination,
            std::vector<lux::math::Position3d>&& source,
            std::uint32_t maximum_points) noexcept
        {
            for (auto& point : source)
            {
                if (!destination.points.empty() && destination.points.back() == point)
                {
                    continue;
                }
                if (destination.points.size() >= maximum_points)
                {
                    destination.status = ENavigationPathStatus::PARTIAL;
                    return;
                }
                destination.points.push_back(std::move(point));
            }
        }

        [[nodiscard]] lux::cxx::expected<NavigationRegion3DStepResult, NavigationRegion3DFailure>
        retireOne(RegionData& data) noexcept
        {
            if (data.retirement_cursor >= data.layers.size())
                return NavigationRegion3DStepResult{true, 0u, 0u};
            if (data.query_readers.load(std::memory_order_acquire) != 0u)
                return NavigationRegion3DStepResult{false, 0u, 0u};
            std::unique_lock query_lock{data.query_mutex, std::try_to_lock};
            if (!query_lock.owns_lock())
                return NavigationRegion3DStepResult{false, 0u, 0u};
            const auto index = data.layers.size() - data.retirement_cursor - 1u;
            auto& layer = data.layers[index];
            const auto bytes = static_cast<std::uint64_t>(layer.size);
            if (layer.installed())
            {
                const auto status = data.navigation->removeTile(layer.reference, nullptr, nullptr);
                if (dtStatusFailed(status))
                {
                    return lux::cxx::unexpected(detail::fail(
                        ENavigationRegion3DError::BUILD_FAILED,
                        "navigation backend rejected one layer retirement"));
                }
                layer.reference = 0u;
            }
            else
            {
                layer.data.reset();
            }
            if (data.resident_bytes < bytes)
                std::abort();
            data.resident_bytes -= bytes;
            ++data.retirement_cursor;
            const bool complete = data.retirement_cursor == data.layers.size();
            if (complete)
            {
                data.query.reset();
                data.navigation.reset();
            }
            return NavigationRegion3DStepResult{complete, 1u, bytes};
        }
    } // namespace

    using detail::contains;
    using detail::fail;
    using detail::finitePositive;

    PreparedNavigationRegion3D::PreparedNavigationRegion3D() noexcept = default;
    PreparedNavigationRegion3D::~PreparedNavigationRegion3D() noexcept = default;
    PreparedNavigationRegion3D::PreparedNavigationRegion3D(PreparedNavigationRegion3D&&) noexcept = default;
    PreparedNavigationRegion3D& PreparedNavigationRegion3D::operator=(PreparedNavigationRegion3D&&) noexcept = default;

    PreparedNavigationRegion3D::PreparedNavigationRegion3D(std::shared_ptr<Data> data) noexcept : data_(std::move(data))
    {
    }

    bool PreparedNavigationRegion3D::valid() const noexcept
    {
        return data_ && data_->region.valid() && data_->navigation && data_->query && !data_->layers.empty() &&
               data_->resident_bytes == data_->owned_bytes;
    }

    NavigationRegionId PreparedNavigationRegion3D::region() const noexcept
    {
        return data_ ? data_->region : NavigationRegionId{};
    }

    std::uint64_t PreparedNavigationRegion3D::requestGeneration() const noexcept
    {
        return data_ ? data_->request_generation : 0u;
    }

    std::uint64_t PreparedNavigationRegion3D::ownedBytes() const noexcept
    {
        return data_ ? data_->owned_bytes : 0u;
    }

    std::uint32_t PreparedNavigationRegion3D::granuleCount() const noexcept
    {
        return data_ ? static_cast<std::uint32_t>(data_->layers.size()) : 0u;
    }

    lux::cxx::expected<NavigationRegion3DStepResult, NavigationRegion3DFailure>
    PreparedNavigationRegion3D::advanceRetirementOne() noexcept
    {
        if (!data_)
        {
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::INVALID_REQUEST, "navigation prepared retirement has no owner"));
        }
        if (data_->resident_bytes == 0u)
            return NavigationRegion3DStepResult{true, 0u, 0u};
        return retireOne(*data_);
    }

    Navigation3DBackend::Navigation3DBackend(std::shared_ptr<Control> control) noexcept : control_(std::move(control))
    {
    }

    Navigation3DBackend::~Navigation3DBackend() noexcept
    {
        if (!control_)
            return;
        std::unique_lock lock{control_->mutex};
        const bool has_staged_data = !control_->staged.empty();
        const bool has_active_data = !control_->active.empty();
        const bool has_retiring_data = !control_->retiring.empty();
        const bool has_owned_bytes = control_->owned_bytes != 0u;
        const bool has_resident_data = has_staged_data || has_active_data || has_retiring_data || has_owned_bytes;
        if (has_resident_data)
        {
            // Normal shutdown must use the bounded owner driver. Silently
            // freeing a whole scene here would reintroduce the close tail this
            // owner contract exists to prevent.
            std::abort();
        }
        control_->alive = false;
    }

    lux::cxx::expected<std::shared_ptr<Navigation3DBackend>, NavigationRegion3DFailure>
    Navigation3DBackend::create(Navigation3DBackendConfig config) noexcept
    {
        if (config.maximum_resident_regions == 0u || !finitePositive(config.maximum_relative_extent))
        {
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::INVALID_REQUEST, "navigation backend configuration is invalid"));
        }
        return std::shared_ptr<Navigation3DBackend>{new Navigation3DBackend{std::make_shared<Control>(config)}};
    }

    lux::cxx::expected<std::unique_ptr<NavigationRegion3DLease>, NavigationRegion3DFailure>
    Navigation3DBackend::adoptPrepared(PreparedNavigationRegion3D&& prepared) noexcept
    {
        if (!control_ || !prepared.valid() || !control_->isOwner())
        {
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::INVALID_REQUEST, "prepared navigation region cannot be adopted here"));
        }
        std::unique_lock lock{control_->mutex};
        const auto region = prepared.data_->region;
        const auto count = control_->staged.size() + control_->active.size() + control_->retiring.size();
        if (!control_->alive)
        {
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::STALE_GENERATION, "navigation backend is closed"));
        }
        if (control_->staged.contains(region) || control_->active.contains(region) ||
            control_->retiring.contains(region))
        {
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::REGION_CONFLICT, "navigation region is already resident"));
        }
        const auto compatibleWith = [&prepared](const RegionMap& regions) {
            for (const auto& [_, resident] : regions)
            {
                const auto connects = [](const NavigationPortal& portal, NavigationRegionId region) noexcept {
                    return portal.first_region == region || portal.second_region == region;
                };
                for (const auto& incoming : prepared.data_->portals)
                {
                    if (connects(incoming, resident->region) &&
                        !detail::sameAgent(prepared.data_->agent, resident->agent))
                    {
                        return false;
                    }
                    const auto duplicate = std::ranges::find_if(
                        resident->portals,
                        [&incoming](const NavigationPortal& candidate) noexcept {
                            return candidate.id == incoming.id;
                        });
                    if (duplicate != resident->portals.end() && !samePortal(*duplicate, incoming))
                    {
                        return false;
                    }
                }
                for (const auto& existing : resident->portals)
                {
                    if (connects(existing, prepared.data_->region) &&
                        !detail::sameAgent(prepared.data_->agent, resident->agent))
                    {
                        return false;
                    }
                }
            }
            return true;
        };
        if (!compatibleWith(control_->staged) || !compatibleWith(control_->active) ||
            !compatibleWith(control_->retiring))
        {
            return lux::cxx::unexpected(fail(
                ENavigationRegion3DError::INVALID_CONTENT,
                "navigation portal has a conflicting contract or "
                "agent constraint"));
        }
        if (count >= control_->config.maximum_resident_regions)
        {
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::CAPACITY_EXHAUSTED, "navigation region capacity is exhausted"));
        }
        auto data = std::move(prepared.data_);
        const auto inserted = control_->staged.emplace(region, data);
        if (!inserted.second)
        {
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::REGION_CONFLICT, "navigation region became resident during adoption"));
        }
        control_->owned_bytes += data->resident_bytes;
        return std::unique_ptr<NavigationRegion3DLease>{new NavigationRegion3DLease{control_, std::move(data)}};
    }

    NavigationPathResult Navigation3DBackend::query(const NavigationPathRequest& request) const noexcept
    {
        NavigationPathResult result;
        if (!control_ || !valid(request))
        {
            result.failure = ENavigationPathFailure::INVALID_REQUEST;
            result.detail = "navigation request is invalid";
            return result;
        }
        if (request.maximum_path_nodes > kMaximumPathNodes || request.maximum_path_points > kMaximumPathPoints)
        {
            result.failure = ENavigationPathFailure::SEARCH_CAPACITY_EXHAUSTED;
            result.detail = "navigation query capacity is too large";
            return result;
        }

        RegionMap active;
        RegionMap known;
        QueryPinBatch query_pins;
        std::uint64_t generation = 0u;
        {
            std::shared_lock lock{control_->mutex};
            generation = control_->generation;
            active = control_->active;
            query_pins.regions.reserve(active.size());
            for (const auto& [_, region] : active)
            {
                region->query_readers.fetch_add(1u, std::memory_order_relaxed);
                query_pins.regions.push_back(region);
            }
            known = control_->staged;
            known.insert(control_->retiring.begin(), control_->retiring.end());
        }
        result.generation = generation;

        std::vector<NavigationPortal> portals;
        bool conflicting_portals = false;
        const auto collectPortals = [&portals, &conflicting_portals](const RegionMap& regions) {
            for (const auto& [_, region] : regions)
            {
                for (const auto& portal : region->portals)
                {
                    const auto duplicate =
                        std::ranges::find_if(portals, [&portal](const NavigationPortal& candidate) noexcept {
                            return candidate.id == portal.id;
                        });
                    if (duplicate == portals.end())
                        portals.push_back(portal);
                    else if (!samePortal(*duplicate, portal))
                        conflicting_portals = true;
                }
            }
        };
        collectPortals(active);
        collectPortals(known);
        if (conflicting_portals)
        {
            result.failure = ENavigationPathFailure::BACKEND_FAILURE;
            result.detail = "resident navigation regions disagree on a portal contract";
            return result;
        }
        std::ranges::sort(portals, portalLess);

        const auto findContaining =
            [&request](const RegionMap& regions, const lux::math::Position3d& point) -> std::shared_ptr<RegionData> {
            std::shared_ptr<RegionData> found;
            for (const auto& [_, candidate] : regions)
            {
                const bool matches_agent = detail::sameAgent(candidate->agent, request.agent);
                const bool contains_point = contains(point, candidate->bounds_min, candidate->bounds_max);
                const bool is_better_match = !found || candidate->region < found->region;
                const bool is_candidate = matches_agent && contains_point && is_better_match;
                if (is_candidate)
                {
                    found = candidate;
                }
            }
            return found;
        };
        const auto findActive = [&active](NavigationRegionId id) -> std::shared_ptr<RegionData> {
            const auto found = active.find(id);
            return found == active.end() ? std::shared_ptr<RegionData>{} : found->second;
        };

        std::shared_ptr<RegionData> start_region =
            request.start_region ? findActive(*request.start_region) : findContaining(active, request.start);
        std::shared_ptr<RegionData> destination_region = request.destination_region
                                                             ? findActive(*request.destination_region)
                                                             : findContaining(active, request.destination);

        const auto addMissing = [&result](NavigationRegionId id) {
            if (std::ranges::find(result.missing_regions, id) == result.missing_regions.end())
            {
                result.missing_regions.push_back(id);
            }
        };
        if (!start_region)
        {
            if (request.start_region)
                addMissing(*request.start_region);
            else if (const auto pending = findContaining(known, request.start))
                addMissing(pending->region);
        }
        if (!destination_region)
        {
            if (request.destination_region)
                addMissing(*request.destination_region);
            else if (const auto pending = findContaining(known, request.destination))
                addMissing(pending->region);
        }
        if (!result.missing_regions.empty())
        {
            std::ranges::sort(result.missing_regions);
            result.status = ENavigationPathStatus::PENDING;
            result.detail = "required navigation regions are not active";
            return result;
        }
        if (!start_region || !destination_region)
        {
            result.failure = ENavigationPathFailure::LOCATION_NOT_FOUND;
            result.detail = "no known navigation region covers the request";
            return result;
        }

        if (start_region->region == destination_region->region)
        {
            result = queryRegion(
                *start_region,
                request.start,
                request.destination,
                request,
                control_->config.maximum_relative_extent);
            result.generation = generation;
            return result;
        }

        auto traversals = findPortalRoute(start_region->region, destination_region->region, portals, &active);
        if (!traversals)
        {
            auto known_route = findPortalRoute(start_region->region, destination_region->region, portals, nullptr);
            if (known_route)
            {
                for (const auto& traversal : *known_route)
                    if (!active.contains(traversal.to))
                        addMissing(traversal.to);
                std::ranges::sort(result.missing_regions);
                result.missing_regions.erase(
                    std::unique(result.missing_regions.begin(), result.missing_regions.end()),
                    result.missing_regions.end());
                if (!result.missing_regions.empty())
                {
                    result.status = ENavigationPathStatus::PENDING;
                    result.detail = "the selected portal route crosses navigation "
                                    "regions that are not active";
                    return result;
                }
            }
            result.failure = ENavigationPathFailure::LOCATION_NOT_FOUND;
            result.detail = "known navigation regions have no connecting portal";
            return result;
        }

        result.failure = ENavigationPathFailure::NONE;
        result.status = ENavigationPathStatus::COMPLETE;
        auto position = request.start;
        for (const auto& traversal : *traversals)
        {
            const auto found = active.find(traversal.from);
            if (found == active.end())
                std::abort();
            auto segment = queryRegion(
                *found->second,
                position,
                traversal.from_position,
                request,
                control_->config.maximum_relative_extent);
            appendPoints(result, std::move(segment.points), request.maximum_path_points);
            if (segment.status == ENavigationPathStatus::FAILED)
            {
                result.status = segment.status;
                result.failure = segment.failure;
                result.detail = std::move(segment.detail);
                result.generation = generation;
                return result;
            }
            if (segment.status == ENavigationPathStatus::PARTIAL || result.status == ENavigationPathStatus::PARTIAL)
            {
                result.status = ENavigationPathStatus::PARTIAL;
                result.detail = "navigation path reaches only part of a portal route";
                result.generation = generation;
                return result;
            }
            position = traversal.to_position;
        }

        auto final_segment = queryRegion(
            *destination_region,
            position,
            request.destination,
            request,
            control_->config.maximum_relative_extent);
        appendPoints(result, std::move(final_segment.points), request.maximum_path_points);
        if (final_segment.status == ENavigationPathStatus::FAILED)
        {
            result.status = final_segment.status;
            result.failure = final_segment.failure;
            result.detail = std::move(final_segment.detail);
        }
        else if (
            final_segment.status == ENavigationPathStatus::PARTIAL || result.status == ENavigationPathStatus::PARTIAL)
        {
            result.status = ENavigationPathStatus::PARTIAL;
            result.failure = ENavigationPathFailure::NONE;
            result.detail = "navigation path is partial";
        }
        result.generation = generation;
        return result;
    }

    Navigation3DBackendSnapshot Navigation3DBackend::snapshot() const noexcept
    {
        if (!control_)
            return {};
        std::shared_lock lock{control_->mutex};
        Navigation3DBackendSnapshot result;
        result.generation = control_->generation;
        result.staged_regions = static_cast<std::uint32_t>(control_->staged.size());
        result.active_regions = static_cast<std::uint32_t>(control_->active.size());
        result.retiring_regions = static_cast<std::uint32_t>(control_->retiring.size());
        result.owned_bytes = control_->owned_bytes;
        for (const auto& [_, data] : control_->staged)
            result.staged_granules += remainingStagingGranules(*data);
        for (const auto& [_, data] : control_->active)
            result.active_granules += static_cast<std::uint32_t>(data->layers.size());
        for (const auto& [_, data] : control_->retiring)
            result.retiring_granules += remainingRetirementGranules(*data);
        return result;
    }

    lux::cxx::expected<NavigationRegion3DStepResult, NavigationRegion3DFailure>
    Navigation3DBackend::advanceRetirementOne() noexcept
    {
        if (!control_ || !control_->isOwner())
        {
            return lux::cxx::unexpected(fail(
                ENavigationRegion3DError::INVALID_REQUEST,
                "navigation retirement requires the backend owner thread"));
        }
        std::unique_lock lock{control_->mutex};
        if (control_->retiring.empty())
            return NavigationRegion3DStepResult{true, 0u, 0u};
        auto lowest = control_->retiring.begin();
        auto selected = control_->retiring.end();
        for (auto candidate = control_->retiring.begin(); candidate != control_->retiring.end(); ++candidate)
        {
            if (candidate->first < lowest->first)
                lowest = candidate;
            const bool has_retirement_boundary = control_->retirement_after.has_value();
            const bool is_after_boundary = has_retirement_boundary &&
                candidate->first > *control_->retirement_after;
            const bool is_earlier_candidate = selected == control_->retiring.end() ||
                candidate->first < selected->first;
            const bool should_select = is_after_boundary && is_earlier_candidate;
            if (should_select)
            {
                selected = candidate;
            }
        }
        if (selected == control_->retiring.end())
            selected = lowest;
        control_->retirement_after = selected->first;
        auto step = retireOne(*selected->second);
        if (!step)
            return lux::cxx::unexpected(std::move(step.error()));
        if (control_->owned_bytes < step->bytes)
            std::abort();
        control_->owned_bytes -= step->bytes;
        if (step->complete)
            control_->retiring.erase(selected);
        if (control_->retiring.empty())
            control_->retirement_after.reset();
        step->complete = control_->retiring.empty();
        return step;
    }

    NavigationRegion3DLease::NavigationRegion3DLease(
        std::weak_ptr<Navigation3DBackend::Control> control,
        std::shared_ptr<PreparedNavigationRegion3D::Data> data) noexcept
        : control_(std::move(control)), data_(std::move(data))
    {
    }

    NavigationRegion3DLease::~NavigationRegion3DLease() noexcept
    {
        reset();
    }

    NavigationRegion3DLease::NavigationRegion3DLease(NavigationRegion3DLease&& other) noexcept = default;
    NavigationRegion3DLease& NavigationRegion3DLease::operator=(NavigationRegion3DLease&& other) noexcept
    {
        if (this == &other)
            return *this;
        reset();
        control_ = std::move(other.control_);
        data_ = std::move(other.data_);
        state_ = other.state_;
        other.state_ = ENavigationRegion3DLeaseState::RETIRED;
        return *this;
    }

    NavigationRegionId NavigationRegion3DLease::region() const noexcept
    {
        return data_ ? data_->region : NavigationRegionId{};
    }

    ENavigationRegion3DLeaseState NavigationRegion3DLease::state() const noexcept
    {
        return data_ ? state_ : ENavigationRegion3DLeaseState::RETIRED;
    }

    lux::cxx::expected<NavigationRegion3DStepResult, NavigationRegion3DFailure>
    NavigationRegion3DLease::advancePreparationOne() noexcept
    {
        const auto control = control_.lock();
        const bool is_missing_control = control == nullptr;
        const bool is_missing_data = data_ == nullptr;
        const bool is_not_owner = !is_missing_control && !control->isOwner();
        const bool is_invalid_state = state_ != ENavigationRegion3DLeaseState::STAGING;
        const bool is_invalid_request = is_missing_control || is_missing_data || is_not_owner || is_invalid_state;
        if (is_invalid_request)
        {
            return lux::cxx::unexpected(fail(
                ENavigationRegion3DError::INVALID_REQUEST,
                "navigation region cannot advance preparation in this state"));
        }
        std::unique_lock lock{control->mutex};
        const auto found = control->staged.find(data_->region);
        if (!control->alive || found == control->staged.end() || found->second != data_)
        {
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::STALE_GENERATION, "navigation region preparation is stale"));
        }
        if (data_->staging_cursor >= data_->layers.size())
        {
            state_ = ENavigationRegion3DLeaseState::READY;
            return NavigationRegion3DStepResult{true, 0u, 0u};
        }
        auto& layer = data_->layers[data_->staging_cursor];
        if (!layer.data || layer.installed())
            std::abort();
        dtTileRef reference = 0u;
        const auto status = data_->navigation->addTile(layer.data.get(), layer.size, DT_TILE_FREE_DATA, 0u, &reference);
        if (dtStatusFailed(status) || reference == 0u)
        {
            if (dtStatusDetail(status, DT_OUT_OF_MEMORY))
                std::abort();
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::BUILD_FAILED, "navigation backend rejected one prepared layer"));
        }
        layer.data.release();
        layer.reference = reference;
        ++data_->staging_cursor;
        const bool complete = data_->staging_cursor == data_->layers.size();
        if (complete)
            state_ = ENavigationRegion3DLeaseState::READY;
        return NavigationRegion3DStepResult{complete, 1u, static_cast<std::uint64_t>(layer.size)};
    }

    lux::cxx::expected<void, NavigationRegion3DFailure> NavigationRegion3DLease::publish() noexcept
    {
        const auto control = control_.lock();
        const bool is_missing_state = !control || !data_;
        if (is_missing_state)
        {
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::INVALID_REQUEST, "navigation region cannot be published in this state"));
        }
        const bool is_invalid_owner = !control->isOwner();
        const bool is_invalid_state = state_ != ENavigationRegion3DLeaseState::READY;
        if (is_invalid_owner || is_invalid_state)
        {
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::INVALID_REQUEST, "navigation region cannot be published in this state"));
        }
        std::unique_lock lock{control->mutex};
        const auto found = control->staged.find(data_->region);
        const bool is_missing_staged_entry = found == control->staged.end();
        const bool is_wrong_staged_entry = is_missing_staged_entry || found->second != data_;
        const bool is_incomplete_staging = data_->staging_cursor != data_->layers.size();
        const bool is_stale_publication = !control->alive || is_wrong_staged_entry || is_incomplete_staging;
        if (is_stale_publication)
        {
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::STALE_GENERATION, "navigation region publication is stale"));
        }
        const auto inserted = control->active.emplace(data_->region, data_);
        if (!inserted.second)
        {
            return lux::cxx::unexpected(fail(
                ENavigationRegion3DError::REGION_CONFLICT,
                "navigation region publication conflicts with active "
                "content"));
        }
        control->staged.erase(found);
        ++control->generation;
        state_ = ENavigationRegion3DLeaseState::ACTIVE;
        return {};
    }

    lux::cxx::expected<void, NavigationRegion3DFailure> NavigationRegion3DLease::hide() noexcept
    {
        if (state_ != ENavigationRegion3DLeaseState::ACTIVE)
        {
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::INVALID_REQUEST, "navigation region cannot be hidden in this state"));
        }
        return beginRetirement();
    }

    lux::cxx::expected<void, NavigationRegion3DFailure> NavigationRegion3DLease::beginRetirement() noexcept
    {
        if (!data_ || state_ == ENavigationRegion3DLeaseState::RETIRED)
            return {};
        if (state_ == ENavigationRegion3DLeaseState::RETIRING)
            return {};
        const auto control = control_.lock();
        if (!control || !control->isOwner())
        {
            return lux::cxx::unexpected(fail(
                ENavigationRegion3DError::INVALID_REQUEST,
                "navigation retirement requires the backend owner thread"));
        }
        std::unique_lock lock{control->mutex};
        auto& source = state_ == ENavigationRegion3DLeaseState::ACTIVE ? control->active : control->staged;
        const auto found = source.find(data_->region);
        if (!control->alive || found == source.end() || found->second != data_)
        {
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::STALE_GENERATION, "navigation region retirement is stale"));
        }
        const auto inserted = control->retiring.emplace(data_->region, data_);
        if (!inserted.second)
        {
            return lux::cxx::unexpected(fail(
                ENavigationRegion3DError::REGION_CONFLICT,
                "navigation region retirement conflicts with pending "
                "content"));
        }
        const bool was_active = state_ == ENavigationRegion3DLeaseState::ACTIVE;
        source.erase(found);
        if (was_active)
            ++control->generation;
        state_ = ENavigationRegion3DLeaseState::RETIRING;
        return {};
    }

    lux::cxx::expected<NavigationRegion3DStepResult, NavigationRegion3DFailure>
    NavigationRegion3DLease::advanceRetirementOne() noexcept
    {
        if (!data_ || state_ == ENavigationRegion3DLeaseState::RETIRED)
            return NavigationRegion3DStepResult{true, 0u, 0u};
        if (state_ != ENavigationRegion3DLeaseState::RETIRING)
        {
            return lux::cxx::unexpected(fail(
                ENavigationRegion3DError::INVALID_REQUEST,
                "navigation region must begin retirement before advancing"));
        }
        const auto control = control_.lock();
        if (!control)
        {
            data_.reset();
            state_ = ENavigationRegion3DLeaseState::RETIRED;
            return NavigationRegion3DStepResult{true, 0u, 0u};
        }
        if (!control->isOwner())
        {
            return lux::cxx::unexpected(fail(
                ENavigationRegion3DError::INVALID_REQUEST,
                "navigation retirement requires the backend owner thread"));
        }
        std::unique_lock lock{control->mutex};
        const auto found = control->retiring.find(data_->region);
        if (!control->alive || found == control->retiring.end() || found->second != data_)
        {
            return lux::cxx::unexpected(
                fail(ENavigationRegion3DError::STALE_GENERATION, "navigation ownership ledger is inconsistent"));
        }
        auto step = retireOne(*data_);
        if (!step)
            return lux::cxx::unexpected(std::move(step.error()));
        if (control->owned_bytes < step->bytes)
            std::abort();
        control->owned_bytes -= step->bytes;
        if (step->complete)
        {
            control->retiring.erase(found);
            data_.reset();
            state_ = ENavigationRegion3DLeaseState::RETIRED;
        }
        return step;
    }

    void NavigationRegion3DLease::reset() noexcept
    {
        if (!data_)
            return;
        if (control_.expired())
        {
            data_.reset();
            state_ = ENavigationRegion3DLeaseState::RETIRED;
            return;
        }
        if (state_ != ENavigationRegion3DLeaseState::RETIRING && !beginRetirement())
        {
            std::abort();
        }
        // The backend retains the region until its owner-level retirement
        // driver has consumed every layer.  Dropping a lease never loops over
        // or synchronously destroys a region-sized allocation.
        data_.reset();
        state_ = ENavigationRegion3DLeaseState::RETIRED;
    }
} // namespace lux::navigation::detour3d
