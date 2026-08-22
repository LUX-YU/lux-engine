#pragma once
/**
 * @file Navigation3DPreparePort.hpp
 * @brief Typed background preparation capability for Navigation3D streaming.
 */

#include <lux/engine/core/async/OperationPort.hpp>
#include <lux/engine/navigation/detour3d/NavigationDetour3D.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace lux::ecs::navigation::streaming
{
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
    };

    namespace detail
    {
        struct Navigation3DPrepareState final
        {
            std::atomic<bool> closing{false};
        };
    } // namespace detail

    class Navigation3DPrepareClient final
    {
    public:
        Navigation3DPrepareClient() noexcept = default;

        Navigation3DPrepareClient(
            std::weak_ptr<detail::Navigation3DPrepareState> state,
            lux::async::OperationPort<BuildNavigationRegion3D> operation)
            noexcept
            : state_(std::move(state)), operation_(std::move(operation))
        {}

        [[nodiscard]] const lux::async::OperationPort<
            BuildNavigationRegion3D>& operation() const noexcept
        {
            return operation_;
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            const auto state = state_.lock();
            return state &&
                !state->closing.load(std::memory_order_acquire) &&
                static_cast<bool>(operation_);
        }

    private:
        std::weak_ptr<detail::Navigation3DPrepareState> state_;
        lux::async::OperationPort<BuildNavigationRegion3D> operation_;
    };
} // namespace lux::ecs::navigation::streaming
