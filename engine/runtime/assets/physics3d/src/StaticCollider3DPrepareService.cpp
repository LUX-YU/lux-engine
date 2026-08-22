#include <lux/engine/runtime/assets/physics3d/StaticCollider3DPrepareService.hpp>

#include <lux/engine/ecs/physics3d/StaticColliderBatch3DCodec.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>

#include <stdexec/execution.hpp>

#include <Eigen/Core>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

namespace lux::runtime::assets::physics3d::detail
{
    using Operation = lux::ecs::physics3d::streaming::BuildStaticCollider3D;
    using Outcome = lux::async::OperationOutcome<Operation>;
    template <typename T>
    using SubmitExp = lux::cxx::expected<T, lux::async::ESubmitError>;
    struct StaticCollider3DPrepareReservation;

    constexpr std::size_t kPreparedOwnerFixedBytes = 16u * 1024u * 1024u;
    constexpr std::size_t kPreparedOwnerExpansion = 8u;

    [[nodiscard]] std::optional<std::size_t> conservativeOwnerBytes(
        std::size_t encoded_bytes) noexcept
    {
        if (encoded_bytes == 0u ||
            encoded_bytes >
                ((std::numeric_limits<std::size_t>::max)() -
                 kPreparedOwnerFixedBytes) /
                    kPreparedOwnerExpansion)
        {
            return std::nullopt;
        }
        return kPreparedOwnerFixedBytes +
            encoded_bytes * kPreparedOwnerExpansion;
    }

    struct StaticCollider3DPrepareControl final
        : public std::enable_shared_from_this<
              StaticCollider3DPrepareControl>
    {
        explicit StaticCollider3DPrepareControl(
            StaticCollider3DPrepareQueueConfig value) noexcept
            : config(value)
        {}

        [[nodiscard]] SubmitExp<
            std::shared_ptr<StaticCollider3DPrepareReservation>>
        reserve(std::size_t bytes) noexcept;

        [[nodiscard]] bool isClosing() const noexcept
        {
            std::lock_guard lock{mutex};
            return closing;
        }

        void closeAdmission() noexcept
        {
            std::lock_guard lock{mutex};
            if (closing)
                return;
            closing = true;
        }

        void releaseRequest() noexcept
        {
            std::lock_guard lock{mutex};
            if (active_requests == 0u)
                std::abort();
            --active_requests;
        }

        void release(
            std::size_t bytes,
            bool request_slot) noexcept
        {
            std::lock_guard lock{mutex};
            if ((request_slot && active_requests == 0u) ||
                active_bytes < bytes)
            {
                std::abort();
            }
            if (request_slot)
                --active_requests;
            active_bytes -= bytes;
        }

        [[nodiscard]] StaticCollider3DPrepareServiceSnapshot snapshot()
            const noexcept
        {
            std::lock_guard lock{mutex};
            return {
                active_requests,
                active_bytes,
                request_high_water,
                byte_high_water,
                rejected_capacity,
                rejected_bytes,
                closing};
        }

        mutable std::mutex mutex;
        StaticCollider3DPrepareQueueConfig config;
        bool closing{false};
        std::size_t active_requests{0u};
        std::size_t active_bytes{0u};
        std::size_t request_high_water{0u};
        std::size_t byte_high_water{0u};
        std::uint64_t rejected_capacity{0u};
        std::uint64_t rejected_bytes{0u};
    };

    struct StaticCollider3DPrepareReservation final
    {
        StaticCollider3DPrepareReservation(
            std::shared_ptr<StaticCollider3DPrepareControl> owner_value,
            std::size_t bytes_value) noexcept
            : owner(std::move(owner_value)), bytes(bytes_value)
        {}

        ~StaticCollider3DPrepareReservation()
        {
            if (owner)
                owner->release(bytes, request_slot);
        }

        void markPrepared() noexcept
        {
            if (!owner || !request_slot)
                return;
            owner->releaseRequest();
            request_slot = false;
        }

        std::shared_ptr<StaticCollider3DPrepareControl> owner;
        std::size_t bytes{0u};
        bool request_slot{true};
    };

    SubmitExp<
        std::shared_ptr<StaticCollider3DPrepareReservation>>
    StaticCollider3DPrepareControl::reserve(
        std::size_t bytes) noexcept
    {
        std::lock_guard lock{mutex};
        if (closing)
        {
            return lux::cxx::unexpected(
                lux::async::ESubmitError::FEATURE_CLOSING);
        }
        if (active_requests >= config.capacity)
        {
            ++rejected_capacity;
            return lux::cxx::unexpected(
                lux::async::ESubmitError::QUEUE_FULL);
        }
        if (bytes > config.byte_budget ||
            active_bytes > config.byte_budget - bytes)
        {
            ++rejected_bytes;
            return lux::cxx::unexpected(
                lux::async::ESubmitError::BYTE_BUDGET_EXHAUSTED);
        }
        ++active_requests;
        active_bytes += bytes;
        request_high_water = std::max(
            request_high_water, active_requests);
        byte_high_water = std::max(byte_high_water, active_bytes);
        return std::make_shared<StaticCollider3DPrepareReservation>(
            shared_from_this(), bytes);
    }

    struct ForwardCompletion final
    {
        std::shared_ptr<StaticCollider3DPrepareReservation> admission;
        void* state{nullptr};
        void (*complete)(void*, Outcome&&) noexcept{nullptr};
    };

    class AdmissionEndpoint final
        : public lux::async::detail::OperationEndpoint<Operation>
    {
    public:
        AdmissionEndpoint(
            std::shared_ptr<StaticCollider3DPrepareControl> control,
            lux::async::OperationPort<Operation> operation) noexcept
            : control_(std::move(control)), operation_(std::move(operation))
        {}

        [[nodiscard]] lux::async::SubmitResult submit(
            Operation operation,
            void* completion_state,
            void (*complete)(void*, Outcome&&) noexcept,
            lux::async::SubmitOptions) noexcept override
        {
            if (operation.content.empty() ||
                operation.request_generation == 0u)
            {
                return reject(
                    completion_state,
                    complete,
                    lux::async::ESubmitError::PAYLOAD_INVALID);
            }
            const auto accounted =
                conservativeOwnerBytes(operation.content.size());
            if (!accounted)
            {
                return reject(
                    completion_state,
                    complete,
                    lux::async::ESubmitError::BYTE_BUDGET_EXHAUSTED);
            }
            auto admission = control_->reserve(*accounted);
            if (!admission)
            {
                return reject(
                    completion_state, complete, admission.error());
            }
            auto forward = std::unique_ptr<ForwardCompletion>{
                new (std::nothrow) ForwardCompletion{
                    std::move(*admission), completion_state, complete}};
            if (!forward)
            {
                return reject(
                    completion_state,
                    complete,
                    lux::async::ESubmitError::QUEUE_FULL);
            }
            auto* const callback_state = forward.release();
            return operation_.submit(
                std::move(operation),
                callback_state,
                +[](void* value, Outcome&& outcome) noexcept
                {
                    auto owner = std::unique_ptr<ForwardCompletion>{
                        static_cast<ForwardCompletion*>(value)};
                    if (outcome)
                    {
                        auto budget = lux::ecs::physics3d::streaming::
                            StaticCollider3DPrepareBudgetLease{
                                std::shared_ptr<void>{owner->admission},
                                owner->admission->bytes,
                                +[](void* reservation) noexcept
                                {
                                    static_cast<
                                        StaticCollider3DPrepareReservation*>(
                                            reservation)->markPrepared();
                                }};
                        budget.markPrepared();
                        outcome->budget = std::move(budget);
                        owner->admission.reset();
                    }
                    owner->complete(owner->state, std::move(outcome));
                },
                lux::async::SubmitOptions{
                    .accounted_bytes = *accounted});
        }

    private:
        [[nodiscard]] static lux::async::SubmitResult reject(
            void* completion_state,
            void (*complete)(void*, Outcome&&) noexcept,
            lux::async::ESubmitError error) noexcept
        {
            complete(
                completion_state,
                lux::cxx::unexpected(
                    lux::async::OperationFailure<Operation::Error>::runtime(
                        error)));
            return lux::cxx::unexpected(error);
        }

        std::shared_ptr<StaticCollider3DPrepareControl> control_;
        lux::async::OperationPort<Operation> operation_;
    };
} // namespace lux::runtime::assets::physics3d::detail

namespace lux::runtime::assets::physics3d
{
    namespace ex = stdexec;
    using lux::ecs::physics3d::streaming::BuildStaticCollider3D;
    using lux::ecs::physics3d::streaming::EStaticCollider3DPrepareError;
    using lux::ecs::physics3d::streaming::PreparedStaticCollider3D;
    using lux::ecs::physics3d::streaming::StaticCollider3DPrepareBudgetLease;
    using lux::ecs::physics3d::streaming::StaticCollider3DPrepareExp;
    using lux::ecs::physics3d::streaming::StaticCollider3DPrepareFailure;

    namespace
    {
        using PrepareResult = lux::cxx::expected<PreparedStaticCollider3D, StaticCollider3DPrepareFailure>;

        [[nodiscard]] StaticCollider3DPrepareFailure failure(
            EStaticCollider3DPrepareError error,
            std::string detail) noexcept
        {
            return {error, std::move(detail)};
        }

        [[nodiscard]] StaticCollider3DPrepareExp<
            lux::ecs::StaticHeightfieldBatch3D>
        materializeBatch(
            lux::physics3d::StaticColliderBatch3DBlobV1 blob,
            const lux::ecs::ResolvedTransform3DComponent& transform)
        {
            constexpr float tolerance = 1.0e-4f;
            if (!lux::math::isFinite(transform.position) ||
                !transform.linear.allFinite())
            {
                return lux::cxx::unexpected(failure(
                    EStaticCollider3DPrepareError::INVALID_TRANSFORM,
                    "static collider transform is not finite"));
            }
            const auto diagonal = transform.linear.diagonal();
            Eigen::Matrix3f off_diagonal = transform.linear;
            off_diagonal.diagonal().setZero();
            if (off_diagonal.cwiseAbs().maxCoeff() > tolerance ||
                diagonal.minCoeff() <= 0.0f ||
                std::abs(diagonal.x() - diagonal.z()) > tolerance)
            {
                return lux::cxx::unexpected(failure(
                    EStaticCollider3DPrepareError::INVALID_TRANSFORM,
                    "heightfields require positive axis-aligned scale with "
                    "matching X/Z scale"));
            }

            lux::ecs::StaticHeightfieldBatch3D result;
            result.heightfields.reserve(blob.heightfields.size());
            const auto linear = transform.linear.cast<double>();
            for (auto& source : blob.heightfields)
            {
                const Eigen::Vector3d local{
                    source.local_origin.x,
                    source.local_origin.y,
                    source.local_origin.z};
                const auto delta = linear * local;
                lux::ecs::StaticHeightfield3D heightfield;
                heightfield.origin = {
                    transform.position.x + delta.x(),
                    transform.position.y + delta.y(),
                    transform.position.z + delta.z()};
                heightfield.sample_edge = source.sample_edge;
                heightfield.sample_spacing =
                    source.sample_spacing * diagonal.x();
                heightfield.height_min = source.height_min * diagonal.y();
                heightfield.height_max = source.height_max * diagonal.y();
                heightfield.samples = std::move(source.samples);
                if (!lux::math::isFinite(heightfield.origin) ||
                    !std::isfinite(heightfield.sample_spacing) ||
                    !std::isfinite(heightfield.height_min) ||
                    !std::isfinite(heightfield.height_max) ||
                    !(heightfield.sample_spacing > 0.0f) ||
                    !(heightfield.height_max > heightfield.height_min))
                {
                    return lux::cxx::unexpected(failure(
                        EStaticCollider3DPrepareError::INVALID_TRANSFORM,
                        "static collider transform produced invalid bounds"));
                }
                result.heightfields.push_back(std::move(heightfield));
            }
            return result;
        }

        [[nodiscard]] PrepareResult prepare(
            BuildStaticCollider3D request) noexcept
        {
            if (request.content.empty() || request.request_generation == 0u)
            {
                return lux::cxx::unexpected(failure(
                    EStaticCollider3DPrepareError::INVALID_REQUEST,
                    "static collider preparation request is invalid"));
            }
            auto decoded =
                lux::physics3d::decodeStaticColliderBatch3DBlob(
                    request.content.view());
            if (!decoded)
            {
                return lux::cxx::unexpected(failure(
                    EStaticCollider3DPrepareError::DECODE_FAILED,
                    std::move(decoded.error().detail)));
            }
            auto batch = materializeBatch(
                std::move(*decoded), request.transform);
            if (!batch)
                return lux::cxx::unexpected(std::move(batch.error()));
            auto prepared = lux::ecs::preparePhysics3DStaticBatch(
                std::move(*batch));
            if (!prepared)
            {
                return lux::cxx::unexpected(failure(
                    EStaticCollider3DPrepareError::PREPARE_FAILED,
                    std::move(prepared.error())));
            }
            PreparedStaticCollider3D result;
            result.batch = std::move(*prepared);
            result.request_generation = request.request_generation;
            return std::move(result);
        }

        struct PendingPrepare final
        {
            explicit PendingPrepare(
                lux::exec::AsyncOperationCompletion<BuildStaticCollider3D>
                    value) noexcept
                : completion(std::move(value))
            {}

            void settle(PrepareResult result) noexcept
            {
                if (settled.exchange(true, std::memory_order_acq_rel))
                    return;
                if (result)
                {
                    completion.complete(std::move(*result));
                    return;
                }
                completion.complete(lux::cxx::unexpected(
                    lux::async::OperationFailure<
                        StaticCollider3DPrepareFailure>::domain(
                            std::move(result.error()))));
            }

            void stop() noexcept
            {
                if (settled.exchange(true, std::memory_order_acq_rel))
                    return;
                completion.failRuntime(
                    lux::async::ESubmitError::STOPPING);
            }

            std::atomic<bool> settled{false};
            lux::exec::AsyncOperationCompletion<BuildStaticCollider3D>
                completion;
        };
    } // namespace

    lux::cxx::expected<
        StaticCollider3DPrepareService,
        lux::exec::AsyncAssemblyFailure>
    StaticCollider3DPrepareService::addTo(
        lux::exec::AsyncRuntimeBuilder& builder,
        StaticCollider3DPrepareQueueConfig config)
    {
        auto control =
            std::make_shared<detail::StaticCollider3DPrepareControl>(config);
        auto state = std::make_shared<lux::ecs::physics3d::streaming::detail::
            StaticCollider3DPrepareState>();
        auto operation = builder.addOperation<BuildStaticCollider3D>(
            [control](
                BuildStaticCollider3D&& request,
                lux::exec::AsyncOperationContext& context,
                lux::exec::AsyncOperationCompletion<BuildStaticCollider3D>&&
                    completion) noexcept
            {
                auto pending = std::make_shared<PendingPrepare>(
                    std::move(completion));
                if (control->isClosing())
                {
                    pending->stop();
                    return;
                }
                auto work =
                    ex::schedule(
                        lux::exec::backgroundCpuScheduler(
                            context.runtime())) |
                    ex::then(
                        [request = std::move(request)]() mutable noexcept
                            -> PrepareResult
                        {
                            return prepare(std::move(request));
                        }) |
                    ex::continues_on(
                        lux::exec::mainThreadScheduler(context.runtime())) |
                    ex::then(
                        [pending](PrepareResult result) noexcept
                        {
                            pending->settle(std::move(result));
                        }) |
                    ex::upon_stopped(
                        [pending]() noexcept
                        {
                            pending->stop();
                        });
                if (!lux::exec::spawn(context.scope(), std::move(work)))
                    pending->stop();
            },
            {},
            lux::exec::AsyncOperationQueueConfig{
                config.capacity,
                config.byte_budget,
                config.drain_batch});
        if (!operation)
            return lux::cxx::unexpected(operation.error());
        auto admitted_operation = lux::async::OperationPort<
            BuildStaticCollider3D>{
                std::make_shared<detail::AdmissionEndpoint>(
                    control, std::move(*operation))};
        return StaticCollider3DPrepareService{
            std::move(control),
            std::move(state),
            std::move(admitted_operation)};
    }

    StaticCollider3DPrepareService::StaticCollider3DPrepareService(
        std::shared_ptr<detail::StaticCollider3DPrepareControl> control,
        std::shared_ptr<lux::ecs::physics3d::streaming::detail::
            StaticCollider3DPrepareState> state,
        lux::async::OperationPort<BuildStaticCollider3D>
            operation) noexcept
        : control_(std::move(control)), state_(std::move(state)),
          operation_(std::move(operation))
    {}

    StaticCollider3DPrepareService::StaticCollider3DPrepareService(
        StaticCollider3DPrepareService&& other) noexcept
        : control_(std::move(other.control_)),
          state_(std::move(other.state_)),
          operation_(std::move(other.operation_)),
          closed_(std::exchange(other.closed_, true))
    {}

    StaticCollider3DPrepareService&
    StaticCollider3DPrepareService::operator=(
        StaticCollider3DPrepareService&& other) noexcept
    {
        if (this == &other)
            return *this;
        close();
        control_ = std::move(other.control_);
        state_ = std::move(other.state_);
        operation_ = std::move(other.operation_);
        closed_ = std::exchange(other.closed_, true);
        return *this;
    }

    StaticCollider3DPrepareService::~StaticCollider3DPrepareService()
    {
        close();
    }

    lux::ecs::physics3d::streaming::StaticCollider3DPrepareClient
    StaticCollider3DPrepareService::client() const noexcept
    {
        return !closed_ && state_
            ? lux::ecs::physics3d::streaming::
                StaticCollider3DPrepareClient{state_, operation_}
            : lux::ecs::physics3d::streaming::
                StaticCollider3DPrepareClient{};
    }

    StaticCollider3DPrepareServiceSnapshot
    StaticCollider3DPrepareService::snapshot() const noexcept
    {
        return control_ ? control_->snapshot()
                        : StaticCollider3DPrepareServiceSnapshot{};
    }

    void StaticCollider3DPrepareService::close() noexcept
    {
        if (closed_)
            return;
        closed_ = true;
        if (state_)
            state_->closing.store(true, std::memory_order_release);
        if (control_)
            control_->closeAdmission();
        operation_ = {};
    }
} // namespace lux::runtime::assets::physics3d
