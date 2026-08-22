#pragma once
/**
 * @file Navigation3DPrepareService.hpp
 * @brief Bounded Runtime endpoint for 3D navigation region preparation.
 */

#include <lux/engine/ecs/navigation/streaming/Navigation3DPreparePort.hpp>
#include <lux/engine/runtime/assets/navigation/visibility.h>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <memory>

namespace lux::runtime::assets::navigation
{
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
    }

    class LUX_ENGINE_RUNTIME_ASSETS_NAVIGATION_PUBLIC
    Navigation3DPrepareService final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            Navigation3DPrepareService,
            lux::exec::AsyncAssemblyFailure>
        addTo(
            lux::exec::AsyncRuntimeBuilder& builder,
            Navigation3DPrepareQueueConfig config = {});

        Navigation3DPrepareService(const Navigation3DPrepareService&) = delete;
        Navigation3DPrepareService& operator=(
            const Navigation3DPrepareService&) = delete;
        Navigation3DPrepareService(Navigation3DPrepareService&& other) noexcept;
        Navigation3DPrepareService& operator=(
            Navigation3DPrepareService&& other) noexcept;
        ~Navigation3DPrepareService();

        [[nodiscard]] lux::ecs::navigation::streaming::
            Navigation3DPrepareClient client() const noexcept;
        void close() noexcept;

    private:
        Navigation3DPrepareService(
            std::shared_ptr<detail::Navigation3DPrepareControl> control,
            std::shared_ptr<lux::ecs::navigation::streaming::detail::
                Navigation3DPrepareState> state,
            lux::async::OperationPort<lux::ecs::navigation::streaming::
                BuildNavigationRegion3D> operation) noexcept;

        std::shared_ptr<detail::Navigation3DPrepareControl> control_;
        std::shared_ptr<lux::ecs::navigation::streaming::detail::
            Navigation3DPrepareState> state_;
        lux::async::OperationPort<lux::ecs::navigation::streaming::
            BuildNavigationRegion3D> operation_;
        bool closed_{false};
    };
} // namespace lux::runtime::assets::navigation
