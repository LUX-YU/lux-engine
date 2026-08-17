#pragma once
/**
 * @file StaticCollider3DPrepareService.hpp
 * @brief Bounded typed background preparation for cooked 3D colliders.
 */

#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/physics3d/systems/Physics3DScene.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/spatial3d/physics/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace lux::runtime::spatial3d
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

    enum class EStaticCollider3DPrepareError : std::uint8_t
    {
        INVALID_REQUEST,
        DECODE_FAILED,
        INVALID_TRANSFORM,
        PREPARE_FAILED
    };

    struct StaticCollider3DPrepareFailure final
    {
        EStaticCollider3DPrepareError error{
            EStaticCollider3DPrepareError::INVALID_REQUEST
        };
        std::string detail;
    };

    template <typename T>
    using StaticCollider3DPrepareExp = lux::cxx::expected<T, StaticCollider3DPrepareFailure>;

    template <typename T>
    using StaticCollider3DSubmitExp = lux::cxx::expected<T, lux::exec::EAsyncSubmitError>;

    template <typename T>
    using StaticCollider3DAssemblyExp = lux::cxx::expected<T, lux::exec::AsyncAssemblyFailure>;

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
        struct StaticCollider3DPrepareReservation;
    } // namespace detail

    /// Keeps the conservative decoded/shape owner budget charged after the
    /// background operation itself has completed.  A successful request moves
    /// this lease through PreparedStaticCollider3D and the ECS binding; only
    /// domain retirement releases it.
    class LUX_ENGINE_RUNTIME_SPATIAL3D_PHYSICS_PUBLIC
        StaticCollider3DPrepareBudgetLease final
    {
    public:
        StaticCollider3DPrepareBudgetLease() noexcept = default;
        StaticCollider3DPrepareBudgetLease(
            StaticCollider3DPrepareBudgetLease&&) noexcept = default;
        StaticCollider3DPrepareBudgetLease& operator=(
            StaticCollider3DPrepareBudgetLease&&) noexcept = default;
        StaticCollider3DPrepareBudgetLease(
            const StaticCollider3DPrepareBudgetLease&) = delete;
        StaticCollider3DPrepareBudgetLease& operator=(
            const StaticCollider3DPrepareBudgetLease&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] std::size_t accountedBytes() const noexcept;
        void markPrepared() noexcept;
        void reset() noexcept;

    private:
        explicit StaticCollider3DPrepareBudgetLease(
            std::shared_ptr<detail::StaticCollider3DPrepareReservation>
                reservation) noexcept
            : reservation_(std::move(reservation))
        {}

        std::shared_ptr<detail::StaticCollider3DPrepareReservation>
            reservation_;

        friend class StaticCollider3DPrepareClient;
        friend class StaticCollider3DPrepareService;
        friend struct PreparedStaticCollider3D;
    };

    struct PreparedStaticCollider3D final
    {
        std::unique_ptr<lux::ecs::Physics3DPreparedStaticBatch> batch;
        StaticCollider3DPrepareBudgetLease budget;
        std::uint64_t request_generation{0u};

        [[nodiscard]] bool valid() const noexcept
        {
            return batch && budget && batch->heightfieldCount() != 0u &&
                request_generation != 0u;
        }
    };

    struct BuildStaticCollider3D final
    {
        using Value = PreparedStaticCollider3D;
        using Error = StaticCollider3DPrepareFailure;

        BuildStaticCollider3D() noexcept = default;
        BuildStaticCollider3D(
            lux::cxx::SharedBytes<> content_value,
            lux::ecs::ResolvedTransform3DComponent transform_value,
            std::uint64_t generation_value) noexcept
            : content(std::move(content_value)),
              transform(std::move(transform_value)),
              request_generation(generation_value)
        {}
        BuildStaticCollider3D(BuildStaticCollider3D&&) noexcept = default;
        BuildStaticCollider3D& operator=(
            BuildStaticCollider3D&&) noexcept = default;
        BuildStaticCollider3D(const BuildStaticCollider3D&) = delete;
        BuildStaticCollider3D& operator=(const BuildStaticCollider3D&) =
            delete;

        lux::cxx::SharedBytes<> content;
        lux::ecs::ResolvedTransform3DComponent transform;
        std::uint64_t request_generation{0u};

    private:
        StaticCollider3DPrepareBudgetLease admission_;

        friend class StaticCollider3DPrepareClient;
        friend class StaticCollider3DPrepareService;
    };

    using StaticCollider3DPrepareSender = lux::exec::AsyncExecuteSender<BuildStaticCollider3D>;

    class LUX_ENGINE_RUNTIME_SPATIAL3D_PHYSICS_PUBLIC
        StaticCollider3DPrepareClient final
    {
    public:
        StaticCollider3DPrepareClient() noexcept = default;

        [[nodiscard]] StaticCollider3DSubmitExp<StaticCollider3DPrepareSender>
        execute(BuildStaticCollider3D request) const noexcept;

        [[nodiscard]] explicit operator bool() const noexcept;

    private:
        friend class StaticCollider3DPrepareService;
        StaticCollider3DPrepareClient(
            std::weak_ptr<detail::StaticCollider3DPrepareControl> control,
            std::uint64_t generation,
            lux::exec::AsyncOperationClient<BuildStaticCollider3D>
                operation) noexcept;

        std::weak_ptr<detail::StaticCollider3DPrepareControl> control_;
        std::uint64_t generation_{0u};
        lux::exec::AsyncOperationClient<BuildStaticCollider3D> operation_;
    };

    class LUX_ENGINE_RUNTIME_SPATIAL3D_PHYSICS_PUBLIC
        StaticCollider3DPrepareService final
    {
    public:
        [[nodiscard]] static StaticCollider3DAssemblyExp<
            StaticCollider3DPrepareService>
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

        [[nodiscard]] StaticCollider3DPrepareClient client() const noexcept;
        [[nodiscard]] StaticCollider3DPrepareServiceSnapshot snapshot()
            const noexcept;
        void close() noexcept;

    private:
        StaticCollider3DPrepareService(
            std::shared_ptr<detail::StaticCollider3DPrepareControl> control,
            lux::exec::AsyncOperationClient<BuildStaticCollider3D>
                operation) noexcept;

        std::shared_ptr<detail::StaticCollider3DPrepareControl> control_;
        lux::exec::AsyncOperationClient<BuildStaticCollider3D> operation_;
        bool closed_{false};
    };
} // namespace lux::runtime::spatial3d
