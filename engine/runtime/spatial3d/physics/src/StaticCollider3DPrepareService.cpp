#include <lux/engine/runtime/spatial3d/physics/StaticCollider3DPrepareService.hpp>

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
#include <optional>
#include <utility>

namespace lux::runtime::spatial3d::detail
{
    struct StaticCollider3DPrepareControl final
        : public std::enable_shared_from_this<
              StaticCollider3DPrepareControl>
    {
        explicit StaticCollider3DPrepareControl(
            StaticCollider3DPrepareQueueConfig value) noexcept
            : config(value)
        {}

        [[nodiscard]] StaticCollider3DSubmitExp<
            std::shared_ptr<StaticCollider3DPrepareReservation>>
        reserve(
            std::uint64_t client_generation,
            std::size_t bytes) noexcept;

        [[nodiscard]] bool accepts(
            std::uint64_t client_generation) const noexcept
        {
            std::lock_guard lock{mutex};
            return !closing && generation == client_generation;
        }

        [[nodiscard]] std::uint64_t currentGeneration() const noexcept
        {
            std::lock_guard lock{mutex};
            return !closing ? generation : 0u;
        }

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
            ++generation;
            if (generation == 0u)
                ++generation;
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
        std::uint64_t generation{1u};
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

    StaticCollider3DSubmitExp<
        std::shared_ptr<StaticCollider3DPrepareReservation>>
    StaticCollider3DPrepareControl::reserve(
        std::uint64_t client_generation,
        std::size_t bytes) noexcept
    {
        std::lock_guard lock{mutex};
        if (closing || generation != client_generation)
        {
            return lux::cxx::unexpected(
                lux::exec::EAsyncSubmitError::FEATURE_CLOSING);
        }
        if (active_requests >= config.capacity)
        {
            ++rejected_capacity;
            return lux::cxx::unexpected(
                lux::exec::EAsyncSubmitError::QUEUE_FULL);
        }
        if (bytes > config.byte_budget ||
            active_bytes > config.byte_budget - bytes)
        {
            ++rejected_bytes;
            return lux::cxx::unexpected(
                lux::exec::EAsyncSubmitError::BYTE_BUDGET_EXHAUSTED);
        }
        ++active_requests;
        active_bytes += bytes;
        request_high_water = std::max(
            request_high_water, active_requests);
        byte_high_water = std::max(byte_high_water, active_bytes);
        return std::make_shared<StaticCollider3DPrepareReservation>(
            shared_from_this(), bytes);
    }
} // namespace lux::runtime::spatial3d::detail

namespace lux::runtime::spatial3d
{
    namespace ex = stdexec;

    namespace
    {
        // Field records and backend shape objects dominate tiny-heightfield
        // batches where encoded-byte expansion alone is not conservative.
        constexpr std::size_t kPreparedOwnerFixedBytes = 16u * 1024u * 1024u;
        constexpr std::size_t kPreparedOwnerExpansion = 8u;

        using PrepareResult = lux::cxx::expected<PreparedStaticCollider3D, StaticCollider3DPrepareFailure>;

        [[nodiscard]] std::optional<std::size_t> conservativeOwnerBytes(
            std::size_t encoded_bytes) noexcept
        {
            if (encoded_bytes == 0u ||
                encoded_bytes >
                    (std::numeric_limits<std::size_t>::max() -
                     kPreparedOwnerFixedBytes) /
                        kPreparedOwnerExpansion)
            {
                return std::nullopt;
            }
            return kPreparedOwnerFixedBytes +
                encoded_bytes * kPreparedOwnerExpansion;
        }

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
            PendingPrepare(
                lux::exec::AsyncOperationCompletion<BuildStaticCollider3D>
                    value,
                StaticCollider3DPrepareBudgetLease admission_value)
                noexcept
                : completion(std::move(value)),
                  admission(std::move(admission_value))
            {}

            void settle(PrepareResult result) noexcept
            {
                if (settled.exchange(true, std::memory_order_acq_rel))
                    return;
                if (result)
                {
                    admission.markPrepared();
                    result->budget = std::move(admission);
                    completion.complete(std::move(*result));
                    return;
                }
                admission.reset();
                completion.complete(lux::cxx::unexpected(
                    lux::exec::AsyncFailure<
                        StaticCollider3DPrepareFailure>::domain(
                            std::move(result.error()))));
            }

            void stop() noexcept
            {
                if (settled.exchange(true, std::memory_order_acq_rel))
                    return;
                admission.reset();
                completion.failRuntime(
                    lux::exec::EAsyncSubmitError::STOPPING);
            }

            std::atomic<bool> settled{false};
            lux::exec::AsyncOperationCompletion<BuildStaticCollider3D>
                completion;
            StaticCollider3DPrepareBudgetLease admission;
        };
    } // namespace

    StaticCollider3DPrepareBudgetLease::operator bool() const noexcept
    {
        return static_cast<bool>(reservation_);
    }

    std::size_t
    StaticCollider3DPrepareBudgetLease::accountedBytes() const noexcept
    {
        return reservation_ ? reservation_->bytes : 0u;
    }

    void StaticCollider3DPrepareBudgetLease::markPrepared() noexcept
    {
        if (reservation_)
            reservation_->markPrepared();
    }

    void StaticCollider3DPrepareBudgetLease::reset() noexcept
    {
        reservation_.reset();
    }

    StaticCollider3DPrepareClient::StaticCollider3DPrepareClient(
        std::weak_ptr<detail::StaticCollider3DPrepareControl> control,
        std::uint64_t generation,
        lux::exec::AsyncOperationClient<BuildStaticCollider3D>
            operation) noexcept
        : control_(std::move(control)), generation_(generation),
          operation_(std::move(operation))
    {}

    StaticCollider3DPrepareClient::operator bool() const noexcept
    {
        const auto control = control_.lock();
        return control && control->accepts(generation_) &&
            static_cast<bool>(operation_);
    }

    StaticCollider3DSubmitExp<StaticCollider3DPrepareSender>
    StaticCollider3DPrepareClient::execute(
        BuildStaticCollider3D request) const noexcept
    {
        const auto control = control_.lock();
        if (!control || !operation_)
        {
            return lux::cxx::unexpected(
                lux::exec::EAsyncSubmitError::UNKNOWN_OPERATION);
        }
        if (request.content.empty() || request.request_generation == 0u)
        {
            return lux::cxx::unexpected(
                lux::exec::EAsyncSubmitError::PAYLOAD_INVALID);
        }
        const auto accounted = conservativeOwnerBytes(
            request.content.size());
        if (!accounted)
        {
            return lux::cxx::unexpected(
                lux::exec::EAsyncSubmitError::BYTE_BUDGET_EXHAUSTED);
        }
        auto admission = control->reserve(generation_, *accounted);
        if (!admission)
            return lux::cxx::unexpected(admission.error());
        request.admission_ = StaticCollider3DPrepareBudgetLease{
            std::move(*admission)};
        return lux::exec::execute(
            operation_,
            std::move(request),
            lux::exec::AsyncSubmitOptions{.accounted_bytes = *accounted});
    }

    StaticCollider3DAssemblyExp<StaticCollider3DPrepareService>
    StaticCollider3DPrepareService::addTo(
        lux::exec::AsyncRuntimeBuilder& builder,
        StaticCollider3DPrepareQueueConfig config)
    {
        auto control =
            std::make_shared<detail::StaticCollider3DPrepareControl>(config);
        auto operation = builder.addOperation<BuildStaticCollider3D>(
            [control](
                BuildStaticCollider3D&& request,
                lux::exec::AsyncOperationContext& context,
                lux::exec::AsyncOperationCompletion<BuildStaticCollider3D>&&
                    completion) noexcept
            {
                auto pending = std::make_shared<PendingPrepare>(
                    std::move(completion),
                    std::move(request.admission_));
                if (!pending->admission)
                {
                    pending->settle(lux::cxx::unexpected(failure(
                        EStaticCollider3DPrepareError::INVALID_REQUEST,
                        "static collider preparation admission is missing")));
                    return;
                }
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
        return StaticCollider3DPrepareService{
            std::move(control), std::move(*operation)};
    }

    StaticCollider3DPrepareService::StaticCollider3DPrepareService(
        std::shared_ptr<detail::StaticCollider3DPrepareControl> control,
        lux::exec::AsyncOperationClient<BuildStaticCollider3D>
            operation) noexcept
        : control_(std::move(control)), operation_(std::move(operation))
    {}

    StaticCollider3DPrepareService::StaticCollider3DPrepareService(
        StaticCollider3DPrepareService&& other) noexcept
        : control_(std::move(other.control_)),
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
        operation_ = std::move(other.operation_);
        closed_ = std::exchange(other.closed_, true);
        return *this;
    }

    StaticCollider3DPrepareService::~StaticCollider3DPrepareService()
    {
        close();
    }

    StaticCollider3DPrepareClient
    StaticCollider3DPrepareService::client() const noexcept
    {
        return !closed_ && control_
            ? StaticCollider3DPrepareClient{
                  control_, control_->currentGeneration(), operation_}
            : StaticCollider3DPrepareClient{};
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
        if (control_)
            control_->closeAdmission();
        operation_ = {};
    }
} // namespace lux::runtime::spatial3d
