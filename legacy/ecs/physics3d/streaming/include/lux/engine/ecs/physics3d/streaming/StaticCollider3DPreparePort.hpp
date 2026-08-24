#pragma once
/**
 * @file StaticCollider3DPreparePort.hpp
 * @brief Typed preparation capability for Physics3D static streaming.
 */

#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/core/async/OperationPort.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/physics3d/systems/Physics3DScene.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace lux::ecs::physics3d::streaming
{
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
            EStaticCollider3DPrepareError::INVALID_REQUEST};
        std::string detail;
    };

    template <typename T>
    using StaticCollider3DPrepareExp =
        lux::cxx::expected<T, StaticCollider3DPrepareFailure>;

    /// Type-erased Runtime budget owner. A successful preparation keeps this
    /// value in its ECS binding until bounded domain retirement releases it.
    class StaticCollider3DPrepareBudgetLease final
    {
    public:
        using MarkPreparedFn = void (*)(void*) noexcept;

        StaticCollider3DPrepareBudgetLease() noexcept = default;
        StaticCollider3DPrepareBudgetLease(
            std::shared_ptr<void> owner,
            std::size_t accounted_bytes,
            MarkPreparedFn mark_prepared) noexcept
            : owner_(std::move(owner)), accounted_bytes_(accounted_bytes),
              mark_prepared_(mark_prepared)
        {}

        StaticCollider3DPrepareBudgetLease(
            StaticCollider3DPrepareBudgetLease&&) noexcept = default;
        StaticCollider3DPrepareBudgetLease& operator=(
            StaticCollider3DPrepareBudgetLease&&) noexcept = default;
        StaticCollider3DPrepareBudgetLease(
            const StaticCollider3DPrepareBudgetLease&) = delete;
        StaticCollider3DPrepareBudgetLease& operator=(
            const StaticCollider3DPrepareBudgetLease&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return static_cast<bool>(owner_);
        }

        [[nodiscard]] std::size_t accountedBytes() const noexcept
        {
            return owner_ ? accounted_bytes_ : 0u;
        }

        void markPrepared() noexcept
        {
            if (owner_ && mark_prepared_)
                mark_prepared_(owner_.get());
        }

        void reset() noexcept
        {
            owner_.reset();
            accounted_bytes_ = 0u;
            mark_prepared_ = nullptr;
        }

    private:
        std::shared_ptr<void> owner_;
        std::size_t accounted_bytes_{0u};
        MarkPreparedFn mark_prepared_{nullptr};
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

        lux::cxx::SharedBytes<> content;
        lux::ecs::ResolvedTransform3DComponent transform;
        std::uint64_t request_generation{0u};
    };

    namespace detail
    {
        struct StaticCollider3DPrepareState final
        {
            std::atomic<bool> closing{false};
        };
    } // namespace detail

    class StaticCollider3DPrepareClient final
    {
    public:
        StaticCollider3DPrepareClient() noexcept = default;

        StaticCollider3DPrepareClient(
            std::weak_ptr<detail::StaticCollider3DPrepareState> state,
            lux::async::OperationPort<BuildStaticCollider3D> operation)
            noexcept
            : state_(std::move(state)), operation_(std::move(operation))
        {}

        [[nodiscard]] const lux::async::OperationPort<
            BuildStaticCollider3D>& operation() const noexcept
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
        std::weak_ptr<detail::StaticCollider3DPrepareState> state_;
        lux::async::OperationPort<BuildStaticCollider3D> operation_;
    };
} // namespace lux::ecs::physics3d::streaming
