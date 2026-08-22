#pragma once
/**
 * @file StaticCollider3DPrepareService.hpp
 * @brief Bounded Runtime endpoint for cooked Physics3D static colliders.
 */

#include <lux/engine/ecs/physics3d/streaming/StaticCollider3DPreparePort.hpp>
#include <lux/engine/runtime/assets/physics3d/visibility.h>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::runtime::assets::physics3d
{
    inline constexpr std::size_t kStaticCollider3DPrepareQueueCapacity = 16u;
    inline constexpr std::size_t kStaticCollider3DPrepareByteBudget =
        512u * 1024u * 1024u;
    inline constexpr std::size_t kStaticCollider3DPrepareDrainBatch = 4u;

    struct StaticCollider3DPrepareQueueConfig final
    {
        std::size_t capacity{kStaticCollider3DPrepareQueueCapacity};
        std::size_t byte_budget{kStaticCollider3DPrepareByteBudget};
        std::size_t drain_batch{kStaticCollider3DPrepareDrainBatch};
    };

    struct StaticCollider3DPrepareServiceSnapshot final
    {
        std::size_t active_requests{0u};
        std::size_t owned_bytes{0u};
        std::size_t request_high_water{0u};
        std::size_t byte_high_water{0u};
        std::uint64_t rejected_capacity{0u};
        std::uint64_t rejected_bytes{0u};
        bool closing{false};
    };

    namespace detail
    {
        struct StaticCollider3DPrepareControl;
    }

    class LUX_ENGINE_RUNTIME_ASSETS_PHYSICS3D_PUBLIC
    StaticCollider3DPrepareService final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            StaticCollider3DPrepareService,
            lux::exec::AsyncAssemblyFailure>
        addTo(
            lux::exec::AsyncRuntimeBuilder& builder,
            StaticCollider3DPrepareQueueConfig config = {});

        StaticCollider3DPrepareService(
            const StaticCollider3DPrepareService&) = delete;
        StaticCollider3DPrepareService& operator=(
            const StaticCollider3DPrepareService&) = delete;
        StaticCollider3DPrepareService(
            StaticCollider3DPrepareService&& other) noexcept;
        StaticCollider3DPrepareService& operator=(
            StaticCollider3DPrepareService&& other) noexcept;
        ~StaticCollider3DPrepareService();

        [[nodiscard]] lux::ecs::physics3d::streaming::
            StaticCollider3DPrepareClient client() const noexcept;
        [[nodiscard]] StaticCollider3DPrepareServiceSnapshot snapshot()
            const noexcept;
        void close() noexcept;

    private:
        StaticCollider3DPrepareService(
            std::shared_ptr<detail::StaticCollider3DPrepareControl> control,
            std::shared_ptr<lux::ecs::physics3d::streaming::detail::
                StaticCollider3DPrepareState> state,
            lux::async::OperationPort<lux::ecs::physics3d::streaming::
                BuildStaticCollider3D> operation) noexcept;

        std::shared_ptr<detail::StaticCollider3DPrepareControl> control_;
        std::shared_ptr<lux::ecs::physics3d::streaming::detail::
            StaticCollider3DPrepareState> state_;
        lux::async::OperationPort<lux::ecs::physics3d::streaming::
            BuildStaticCollider3D> operation_;
        bool closed_{false};
    };
} // namespace lux::runtime::assets::physics3d
