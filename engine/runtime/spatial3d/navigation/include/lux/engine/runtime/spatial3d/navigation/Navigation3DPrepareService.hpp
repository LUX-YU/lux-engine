#pragma once
/**
 * @file Navigation3DPrepareService.hpp
 * @brief Bounded typed background preparation for 3D navigation regions.
 */

#include <lux/engine/navigation/detour3d/NavigationDetour3D.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/spatial3d/navigation/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace lux::runtime::spatial3d
{
    template <typename T>
    using Navigation3DSubmitExp =
        lux::cxx::expected<T, lux::exec::EAsyncSubmitError>;

    template <typename T>
    using Navigation3DAssemblyExp =
        lux::cxx::expected<T, lux::exec::AsyncAssemblyFailure>;

    inline constexpr std::size_t kNavigation3DPrepareQueueCapacity = 16u;
    inline constexpr std::size_t kNavigation3DPrepareByteBudget =
        256u * 1024u * 1024u;
    inline constexpr std::size_t kNavigation3DPrepareDrainBatch = 4u;

    struct Navigation3DPrepareQueueConfig final
    {
        std::size_t capacity{kNavigation3DPrepareQueueCapacity};
        std::size_t byte_budget{kNavigation3DPrepareByteBudget};
        std::size_t drain_batch{kNavigation3DPrepareDrainBatch};
    };

    namespace detail
    {
        struct Navigation3DPrepareControl;
        struct Navigation3DPrepareReservation;
    } // namespace detail

    struct BuildNavigationRegion3D final
    {
        using Value = lux::navigation::detour3d::PreparedNavigationRegion3D;
        using Error = lux::navigation::detour3d::NavigationRegion3DFailure;

        BuildNavigationRegion3D() noexcept = default;
        BuildNavigationRegion3D(
            lux::navigation::detour3d::NavigationRegion3DBlob blob_value,
            std::uint64_t generation_value) noexcept
            : blob(std::move(blob_value)),
              request_generation(generation_value)
        {}
        BuildNavigationRegion3D(BuildNavigationRegion3D&&) noexcept = default;
        BuildNavigationRegion3D& operator=(BuildNavigationRegion3D&&) noexcept =
            default;
        BuildNavigationRegion3D(const BuildNavigationRegion3D&) = delete;
        BuildNavigationRegion3D& operator=(const BuildNavigationRegion3D&) =
            delete;

        lux::navigation::detour3d::NavigationRegion3DBlob blob;
        std::uint64_t request_generation{0u};

      private:
        std::shared_ptr<detail::Navigation3DPrepareReservation> admission_;

        friend class Navigation3DPrepareClient;
        friend class Navigation3DPrepareService;
    };

    using Navigation3DPrepareSender = lux::exec::AsyncExecuteSender<BuildNavigationRegion3D>;

    class LUX_ENGINE_RUNTIME_SPATIAL3D_NAVIGATION_PUBLIC
        Navigation3DPrepareClient final
    {
      public:
        Navigation3DPrepareClient() noexcept = default;

        /// Reserves the service-wide queued+running count and byte budget
        /// before an AsyncRuntime sender can be created.  The reservation is
        /// carried by the typed request until its terminal completion, so a
        /// drained endpoint queue cannot make background work unbounded.
        [[nodiscard]] Navigation3DSubmitExp<Navigation3DPrepareSender>
        execute(BuildNavigationRegion3D request) const noexcept;

        [[nodiscard]] explicit operator bool() const noexcept;

      private:
        friend class Navigation3DPrepareService;
        Navigation3DPrepareClient(
            std::weak_ptr<detail::Navigation3DPrepareControl> control,
            std::uint64_t generation,
            lux::exec::AsyncOperationClient<BuildNavigationRegion3D>
                operation) noexcept;

        std::weak_ptr<detail::Navigation3DPrepareControl> control_;
        std::uint64_t generation_{0u};
        lux::exec::AsyncOperationClient<BuildNavigationRegion3D> operation_;
    };

    class LUX_ENGINE_RUNTIME_SPATIAL3D_NAVIGATION_PUBLIC
        Navigation3DPrepareService final
    {
      public:
        [[nodiscard]] static Navigation3DAssemblyExp<Navigation3DPrepareService>
        addTo(lux::exec::AsyncRuntimeBuilder& builder,
              Navigation3DPrepareQueueConfig config = {});

        Navigation3DPrepareService(const Navigation3DPrepareService&) = delete;
        Navigation3DPrepareService&
        operator=(const Navigation3DPrepareService&) = delete;
        Navigation3DPrepareService(Navigation3DPrepareService&& other) noexcept;
        Navigation3DPrepareService&
        operator=(Navigation3DPrepareService&& other) noexcept;
        ~Navigation3DPrepareService();

        [[nodiscard]] Navigation3DPrepareClient client() const noexcept;
        void close() noexcept;

      private:
        Navigation3DPrepareService(
            std::shared_ptr<detail::Navigation3DPrepareControl> control,
            lux::exec::AsyncOperationClient<BuildNavigationRegion3D>
                operation) noexcept;

        std::shared_ptr<detail::Navigation3DPrepareControl> control_;
        lux::exec::AsyncOperationClient<BuildNavigationRegion3D> operation_;
        bool closed_{false};
    };
} // namespace lux::runtime::spatial3d
