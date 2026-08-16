#pragma once
/**
 * @file NavigationDetour3D.hpp
 * @brief Detour-backed 3D navigation implementation with a neutral surface.
 *
 * Third-party handles and storage vocabulary never cross this boundary.  The
 * cooked value is an engine-owned, relocatable region description rather than
 * a dump of the backend ABI.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/navigation/Navigation.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lux::navigation::detour3d
{
    inline constexpr std::uint32_t kNavigationRegion3DSchemaVersion = 1u;
    inline constexpr std::string_view kNavigationRegion3DContentTypeName =
        "lux.navigation.region3d";

    /// Authoring/cooking description for one convex traversable area.  This is
    /// navigation topology, independent of unrelated presentation data.
    struct NavigationTraversableArea3D final
    {
        std::vector<lux::spatial::Position3D> boundary;
        std::uint8_t area_class{0u};
        std::uint16_t traversal_flags{1u};
    };

    struct NavigationRegion3DDescription final
    {
        NavigationRegionId region;
        NavigationAgentConstraints agent;
        float horizontal_resolution{0.3f};
        float vertical_resolution{0.2f};
        std::vector<NavigationTraversableArea3D> areas;
        /// Semantic cross-region connections.  A portal may be authored in
        /// either participating region; duplicate IDs are coalesced when both
        /// sides are resident.
        std::vector<NavigationPortal> portals;
    };

    /// Runtime content input.  `payload` uses the schema above but remains
    /// opaque to generic content and scene infrastructure.
    struct NavigationRegion3DBlob final
    {
        NavigationRegionId region;
        std::uint32_t schema_version{kNavigationRegion3DSchemaVersion};
        lux::cxx::SharedBytes<> payload;

        [[nodiscard]] bool valid() const noexcept
        {
            return region.valid() &&
                   schema_version == kNavigationRegion3DSchemaVersion &&
                   !payload.empty();
        }
    };

    enum class ENavigationRegion3DError : std::uint8_t
    {
        INVALID_REQUEST,
        INVALID_CONTENT,
        UNSUPPORTED_AGENT,
        CAPACITY_EXHAUSTED,
        BUILD_FAILED,
        REGION_CONFLICT,
        STALE_GENERATION
    };

    struct NavigationRegion3DFailure final
    {
        ENavigationRegion3DError code{ENavigationRegion3DError::BUILD_FAILED};
        std::string detail;
    };

    struct NavigationRegion3DStepResult final
    {
        bool complete{false};
        std::uint32_t work_items{0u};
        std::uint64_t bytes{0u};
    };

    [[nodiscard]] LUX_FUNCTION_PUBLIC
        lux::cxx::expected<NavigationRegion3DBlob, NavigationRegion3DFailure>
        encodeNavigationRegion3D(
            const NavigationRegion3DDescription& description) noexcept;

    /// Restore a domain-owned blob from immutable cooked bytes.  Generic
    /// content storage remains unaware of this payload's header.
    [[nodiscard]] LUX_FUNCTION_PUBLIC
        lux::cxx::expected<NavigationRegion3DBlob, NavigationRegion3DFailure>
        navigationRegion3DBlobFromBytes(
            lux::cxx::SharedBytes<> payload) noexcept;

    class PreparedNavigationRegion3D;
    class NavigationRegion3DLease;

    /// Pure CPU preparation entry.  It may run on a background worker and
    /// touches no live scene, registry or scheduler state.
    [[nodiscard]] LUX_FUNCTION_PUBLIC
        lux::cxx::expected<PreparedNavigationRegion3D,
                           NavigationRegion3DFailure>
        prepareNavigationRegion3D(NavigationRegion3DBlob blob,
                                  std::uint64_t request_generation) noexcept;

    class LUX_FUNCTION_PUBLIC PreparedNavigationRegion3D final
    {
      public:
        // Opaque implementation identity shared with the backend owner.  The
        // definition is private to this target; no third-party type appears
        // in the installed header.
        struct Data;

        PreparedNavigationRegion3D() noexcept;
        ~PreparedNavigationRegion3D() noexcept;
        PreparedNavigationRegion3D(PreparedNavigationRegion3D&&) noexcept;
        PreparedNavigationRegion3D&
        operator=(PreparedNavigationRegion3D&&) noexcept;
        PreparedNavigationRegion3D(const PreparedNavigationRegion3D&) = delete;
        PreparedNavigationRegion3D&
        operator=(const PreparedNavigationRegion3D&) = delete;

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] NavigationRegionId region() const noexcept;
        [[nodiscard]] std::uint64_t requestGeneration() const noexcept;
        [[nodiscard]] std::uint64_t ownedBytes() const noexcept;
        [[nodiscard]] std::uint32_t granuleCount() const noexcept;
        /// Releases at most one not-yet-adopted backend layer. This is the
        /// stale/cancel path used when a main-thread completion can no longer
        /// enter live storage; callers must drive it to completion before
        /// destroying a material prepared batch.
        [[nodiscard]] lux::cxx::expected<NavigationRegion3DStepResult,
                                         NavigationRegion3DFailure>
        advanceRetirementOne() noexcept;

      private:
        explicit PreparedNavigationRegion3D(
            std::shared_ptr<Data> data) noexcept;
        std::shared_ptr<Data> data_;

        friend LUX_FUNCTION_PUBLIC
            lux::cxx::expected<PreparedNavigationRegion3D,
                               NavigationRegion3DFailure>
        prepareNavigationRegion3D(NavigationRegion3DBlob,
                                  std::uint64_t) noexcept;
        friend class Navigation3DBackend;
        friend class NavigationRegion3DLease;
    };

    struct Navigation3DBackendConfig final
    {
        std::uint32_t maximum_resident_regions{4096u};
        float maximum_relative_extent{10'000'000.0f};
    };

    struct Navigation3DBackendSnapshot final
    {
        std::uint64_t generation{0u};
        std::uint32_t staged_regions{0u};
        std::uint32_t active_regions{0u};
        std::uint32_t retiring_regions{0u};
        std::uint32_t staged_granules{0u};
        std::uint32_t active_granules{0u};
        std::uint32_t retiring_granules{0u};
        std::uint64_t owned_bytes{0u};
    };

    /// Scene-local owner.  Query is thread-safe; adoption/publication and
    /// retirement remain on the owning main thread.
    class LUX_FUNCTION_PUBLIC Navigation3DBackend final
        : public std::enable_shared_from_this<Navigation3DBackend>
    {
      public:
        ~Navigation3DBackend() noexcept;
        Navigation3DBackend(const Navigation3DBackend&) = delete;
        Navigation3DBackend& operator=(const Navigation3DBackend&) = delete;

        [[nodiscard]] static lux::cxx::expected<
            std::shared_ptr<Navigation3DBackend>,
            NavigationRegion3DFailure>
        create(Navigation3DBackendConfig config = {}) noexcept;

        [[nodiscard]] lux::cxx::expected<
            std::unique_ptr<NavigationRegion3DLease>,
            NavigationRegion3DFailure>
        adoptPrepared(PreparedNavigationRegion3D&& prepared) noexcept;

        [[nodiscard]] NavigationPathResult
        query(const NavigationPathRequest& request) const noexcept;

        [[nodiscard]] Navigation3DBackendSnapshot snapshot() const noexcept;

        /// Advances at most one orphaned retirement granule.  Leases normally
        /// drive their own region; this owner-level seam makes a relinquished
        /// lease safe without doing bulk work in its destructor.
        [[nodiscard]] lux::cxx::expected<NavigationRegion3DStepResult,
                                         NavigationRegion3DFailure>
        advanceRetirementOne() noexcept;

      private:
        struct Control;
        explicit Navigation3DBackend(std::shared_ptr<Control> control) noexcept;
        std::shared_ptr<Control> control_;

        friend class NavigationRegion3DLease;
    };

    enum class ENavigationRegion3DLeaseState : std::uint8_t
    {
        STAGING,
        READY,
        ACTIVE,
        RETIRING,
        RETIRED
    };

    class LUX_FUNCTION_PUBLIC NavigationRegion3DLease final
    {
      public:
        ~NavigationRegion3DLease() noexcept;
        NavigationRegion3DLease(NavigationRegion3DLease&&) noexcept;
        NavigationRegion3DLease& operator=(NavigationRegion3DLease&&) noexcept;
        NavigationRegion3DLease(const NavigationRegion3DLease&) = delete;
        NavigationRegion3DLease&
        operator=(const NavigationRegion3DLease&) = delete;

        [[nodiscard]] NavigationRegionId region() const noexcept;
        [[nodiscard]] ENavigationRegion3DLeaseState state() const noexcept;
        [[nodiscard]] lux::cxx::expected<NavigationRegion3DStepResult,
                                         NavigationRegion3DFailure>
        advancePreparationOne() noexcept;
        [[nodiscard]] lux::cxx::expected<void, NavigationRegion3DFailure>
        publish() noexcept;
        [[nodiscard]] lux::cxx::expected<void, NavigationRegion3DFailure>
        hide() noexcept;
        /// Logically hides ACTIVE content or cancels unpublished content, then
        /// transfers it to the bounded retirement queue.
        [[nodiscard]] lux::cxx::expected<void, NavigationRegion3DFailure>
        beginRetirement() noexcept;
        [[nodiscard]] lux::cxx::expected<NavigationRegion3DStepResult,
                                         NavigationRegion3DFailure>
        advanceRetirementOne() noexcept;
        void reset() noexcept;

      private:
        NavigationRegion3DLease(
            std::weak_ptr<Navigation3DBackend::Control> control,
            std::shared_ptr<PreparedNavigationRegion3D::Data> data) noexcept;

        std::weak_ptr<Navigation3DBackend::Control> control_;
        std::shared_ptr<PreparedNavigationRegion3D::Data> data_;
        ENavigationRegion3DLeaseState state_{
            ENavigationRegion3DLeaseState::STAGING};

        friend class Navigation3DBackend;
    };
} // namespace lux::navigation::detour3d
