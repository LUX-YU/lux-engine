#pragma once

#include <lux/engine/navigation/detour3d/NavigationDetour3D.hpp>

#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::navigation::detour3d::detail
{
    struct RegionIdHash final
    {
        [[nodiscard]] std::size_t
        operator()(NavigationRegionId value) const noexcept
        {
            const auto mixed =
                value.high ^ (value.low + 0x9e3779b97f4a7c15ull +
                              (value.high << 6u) + (value.high >> 2u));
            return static_cast<std::size_t>(mixed);
        }
    };

    struct NavigationOwner final
    {
        void operator()(dtNavMesh* value) const noexcept
        {
            dtFreeNavMesh(value);
        }
    };

    struct QueryOwner final
    {
        void operator()(dtNavMeshQuery* value) const noexcept
        {
            dtFreeNavMeshQuery(value);
        }
    };

    struct NavigationDataOwner final
    {
        void operator()(unsigned char* value) const noexcept
        {
            dtFree(value);
        }
    };

    struct PreparedLayer final
    {
        std::unique_ptr<unsigned char, NavigationDataOwner> data;
        int size{0};
        dtTileRef reference{0u};

        [[nodiscard]] bool installed() const noexcept
        {
            return reference != 0u;
        }
    };

    [[nodiscard]] inline NavigationRegion3DFailure
    fail(ENavigationRegion3DError code, std::string detail)
    {
        return NavigationRegion3DFailure{code, std::move(detail)};
    }

    [[nodiscard]] inline bool finitePositive(float value) noexcept
    {
        return std::isfinite(value) && value > 0.0f;
    }

    [[nodiscard]] inline bool
    sameAgent(const NavigationAgentConstraints& left,
              const NavigationAgentConstraints& right) noexcept
    {
        return left == right;
    }

    [[nodiscard]] inline bool
    contains(const lux::spatial::Position3D& point,
             const lux::spatial::Position3D& minimum,
             const lux::spatial::Position3D& maximum) noexcept
    {
        return point.x >= minimum.x && point.x <= maximum.x &&
               point.y >= minimum.y && point.y <= maximum.y &&
               point.z >= minimum.z && point.z <= maximum.z;
    }
} // namespace lux::navigation::detour3d::detail

namespace lux::navigation::detour3d
{
    struct PreparedNavigationRegion3D::Data final
    {
        NavigationRegionId region;
        NavigationAgentConstraints agent;
        lux::spatial::Position3D origin;
        lux::spatial::Position3D bounds_min;
        lux::spatial::Position3D bounds_max;
        std::uint64_t request_generation{0u};
        std::uint64_t owned_bytes{0u};
        std::uint64_t resident_bytes{0u};
        std::unique_ptr<dtNavMesh, detail::NavigationOwner> navigation;
        std::unique_ptr<dtNavMeshQuery, detail::QueryOwner> query;
        std::vector<detail::PreparedLayer> layers;
        std::vector<NavigationPortal> portals;
        std::size_t staging_cursor{0u};
        std::size_t retirement_cursor{0u};
        std::atomic<std::uint32_t> query_readers{0u};
        mutable std::mutex query_mutex;
    };

    struct Navigation3DBackend::Control final
    {
        explicit Control(Navigation3DBackendConfig value) noexcept
            : config(value), owner_thread(std::this_thread::get_id())
        {
        }

        [[nodiscard]] bool isOwner() const noexcept
        {
            return owner_thread == std::this_thread::get_id();
        }

        Navigation3DBackendConfig config;
        std::thread::id owner_thread;
        mutable std::shared_mutex mutex;
        bool alive{true};
        std::uint64_t generation{0u};
        std::uint64_t owned_bytes{0u};
        std::unordered_map<NavigationRegionId,
                           std::shared_ptr<PreparedNavigationRegion3D::Data>,
                           detail::RegionIdHash>
            staged;
        std::unordered_map<NavigationRegionId,
                           std::shared_ptr<PreparedNavigationRegion3D::Data>,
                           detail::RegionIdHash>
            active;
        std::unordered_map<NavigationRegionId,
                           std::shared_ptr<PreparedNavigationRegion3D::Data>,
                           detail::RegionIdHash>
            retiring;
        std::optional<NavigationRegionId> retirement_after;
    };
} // namespace lux::navigation::detour3d
