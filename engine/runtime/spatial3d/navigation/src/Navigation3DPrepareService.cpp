#include <lux/engine/runtime/spatial3d/navigation/Navigation3DPrepareService.hpp>

#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>

#include <stdexec/execution.hpp>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <utility>

namespace lux::runtime::spatial3d::detail
{
    struct Navigation3DPrepareControl final
        : public std::enable_shared_from_this<Navigation3DPrepareControl>
    {
        explicit Navigation3DPrepareControl(
            Navigation3DPrepareQueueConfig value) noexcept
            : config(value)
        {}

        [[nodiscard]] lux::cxx::expected<
            std::shared_ptr<Navigation3DPrepareReservation>,
            lux::exec::EAsyncSubmitError>
        reserve(std::uint64_t client_generation, std::size_t bytes) noexcept;

        [[nodiscard]] bool accepts(std::uint64_t client_generation) const
            noexcept
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

        void release(std::size_t bytes) noexcept
        {
            std::lock_guard lock{mutex};
            if (active_requests == 0u || active_bytes < bytes)
                std::abort();
            --active_requests;
            active_bytes -= bytes;
        }

        mutable std::mutex mutex;
        Navigation3DPrepareQueueConfig config;
        bool closing{false};
        std::uint64_t generation{1u};
        std::size_t active_requests{0u};
        std::size_t active_bytes{0u};
    };

    struct Navigation3DPrepareReservation final
    {
        Navigation3DPrepareReservation(
            std::shared_ptr<Navigation3DPrepareControl> owner_value,
            std::size_t bytes_value) noexcept
            : owner(std::move(owner_value)), bytes(bytes_value)
        {}

        ~Navigation3DPrepareReservation()
        {
            if (owner)
                owner->release(bytes);
        }

        std::shared_ptr<Navigation3DPrepareControl> owner;
        std::size_t bytes{0u};
    };

    lux::cxx::expected<std::shared_ptr<Navigation3DPrepareReservation>,
                       lux::exec::EAsyncSubmitError>
    Navigation3DPrepareControl::reserve(
        std::uint64_t client_generation, std::size_t bytes) noexcept
    {
        std::lock_guard lock{mutex};
        if (closing || generation != client_generation)
        {
            return lux::cxx::unexpected(
                lux::exec::EAsyncSubmitError::FEATURE_CLOSING);
        }
        if (active_requests >= config.capacity)
        {
            return lux::cxx::unexpected(
                lux::exec::EAsyncSubmitError::QUEUE_FULL);
        }
        if (bytes > config.byte_budget ||
            active_bytes > config.byte_budget - bytes)
        {
            return lux::cxx::unexpected(
                lux::exec::EAsyncSubmitError::BYTE_BUDGET_EXHAUSTED);
        }
        ++active_requests;
        active_bytes += bytes;
        return std::make_shared<Navigation3DPrepareReservation>(
            shared_from_this(), bytes);
    }
} // namespace lux::runtime::spatial3d::detail

namespace lux::runtime::spatial3d
{
    namespace ex = stdexec;

    namespace
    {
        using PrepareResult = lux::cxx::expected<
            lux::navigation::detour3d::PreparedNavigationRegion3D,
            lux::navigation::detour3d::NavigationRegion3DFailure>;

        struct PendingPrepare final
        {
            explicit PendingPrepare(
                lux::exec::AsyncOperationCompletion<BuildNavigationRegion3D>
                    value,
                std::shared_ptr<detail::Navigation3DPrepareReservation>
                    admission_value) noexcept
                : completion(std::move(value)),
                  admission(std::move(admission_value))
            {
            }

            void settle(PrepareResult result) noexcept
            {
                if (settled.exchange(true, std::memory_order_acq_rel))
                    return;
                admission.reset();
                if (result)
                {
                    completion.complete(std::move(*result));
                    return;
                }
                completion.complete(lux::cxx::unexpected(
                    lux::exec::AsyncFailure<
                        lux::navigation::detour3d::NavigationRegion3DFailure>::
                        domain(std::move(result.error()))));
            }

            void stop() noexcept
            {
                if (settled.exchange(true, std::memory_order_acq_rel))
                    return;
                admission.reset();
                completion.failRuntime(lux::exec::EAsyncSubmitError::STOPPING);
            }

            std::atomic<bool> settled{false};
            lux::exec::AsyncOperationCompletion<BuildNavigationRegion3D>
                completion;
            std::shared_ptr<detail::Navigation3DPrepareReservation> admission;
        };
    } // namespace

    Navigation3DPrepareClient::Navigation3DPrepareClient(
        std::weak_ptr<detail::Navigation3DPrepareControl> control,
        std::uint64_t generation,
        lux::exec::AsyncOperationClient<BuildNavigationRegion3D>
            operation) noexcept
        : control_(std::move(control)), generation_(generation),
          operation_(std::move(operation))
    {
    }

    Navigation3DPrepareClient::operator bool() const noexcept
    {
        const auto control = control_.lock();
        return control && control->accepts(generation_) &&
               static_cast<bool>(operation_);
    }

    lux::cxx::expected<Navigation3DPrepareSender,
                       lux::exec::EAsyncSubmitError>
    Navigation3DPrepareClient::execute(
        BuildNavigationRegion3D request) const noexcept
    {
        const auto control = control_.lock();
        if (!control || !operation_)
        {
            return lux::cxx::unexpected(
                lux::exec::EAsyncSubmitError::UNKNOWN_OPERATION);
        }
        const auto bytes = request.blob.payload.size();
        auto admission = control->reserve(generation_, bytes);
        if (!admission)
            return lux::cxx::unexpected(admission.error());
        request.admission_ = std::move(*admission);
        return lux::exec::execute(
            operation_,
            std::move(request),
            lux::exec::AsyncSubmitOptions{.accounted_bytes = bytes});
    }

    lux::cxx::expected<Navigation3DPrepareService,
                       lux::exec::AsyncAssemblyFailure>
    Navigation3DPrepareService::addTo(lux::exec::AsyncRuntimeBuilder& builder,
                                      Navigation3DPrepareQueueConfig config)
    {
        auto control =
            std::make_shared<detail::Navigation3DPrepareControl>(config);
        auto operation = builder.addOperation<BuildNavigationRegion3D>(
            [control](
                BuildNavigationRegion3D&& request,
                lux::exec::AsyncOperationContext& context,
                lux::exec::AsyncOperationCompletion<BuildNavigationRegion3D>&&
                    completion) noexcept
            {
                auto pending = std::make_shared<PendingPrepare>(
                    std::move(completion), std::move(request.admission_));
                if (!pending->admission)
                {
                    pending->settle(lux::cxx::unexpected(
                        lux::navigation::detour3d::NavigationRegion3DFailure{
                            lux::navigation::detour3d::
                                ENavigationRegion3DError::INVALID_REQUEST,
                            "navigation preparation admission is missing"}));
                    return;
                }
                if (control->isClosing())
                {
                    pending->stop();
                    return;
                }
                if (!request.blob.valid() || request.request_generation == 0u)
                {
                    pending->settle(lux::cxx::unexpected(
                        lux::navigation::detour3d::NavigationRegion3DFailure{
                            lux::navigation::detour3d::
                                ENavigationRegion3DError::INVALID_REQUEST,
                            "navigation preparation request is invalid"}));
                    return;
                }
                auto work =
                    ex::schedule(
                        lux::exec::backgroundCpuScheduler(context.runtime())) |
                    ex::then(
                        [request = std::move(
                             request)]() mutable noexcept -> PrepareResult
                        {
                            return lux::navigation::detour3d::
                                prepareNavigationRegion3D(
                                    std::move(request.blob),
                                    request.request_generation);
                        }) |
                    ex::continues_on(
                        lux::exec::mainThreadScheduler(context.runtime())) |
                    ex::then([pending](PrepareResult result) noexcept
                             { pending->settle(std::move(result)); }) |
                    ex::upon_stopped([pending]() noexcept { pending->stop(); });
                if (!lux::exec::spawn(context.scope(), std::move(work)))
                    pending->stop();
            },
            {},
            lux::exec::AsyncOperationQueueConfig{
                config.capacity, config.byte_budget, config.drain_batch});
        if (!operation)
            return lux::cxx::unexpected(operation.error());
        return Navigation3DPrepareService{std::move(control),
                                          std::move(*operation)};
    }

    Navigation3DPrepareService::Navigation3DPrepareService(
        std::shared_ptr<detail::Navigation3DPrepareControl> control,
        lux::exec::AsyncOperationClient<BuildNavigationRegion3D>
            operation) noexcept
        : control_(std::move(control)), operation_(std::move(operation))
    {
    }

    Navigation3DPrepareService::Navigation3DPrepareService(
        Navigation3DPrepareService&& other) noexcept
        : control_(std::move(other.control_)),
          operation_(std::move(other.operation_)),
          closed_(std::exchange(other.closed_, true))
    {}

    Navigation3DPrepareService& Navigation3DPrepareService::operator=(
        Navigation3DPrepareService&& other) noexcept
    {
        if (this == &other)
            return *this;
        close();
        control_ = std::move(other.control_);
        operation_ = std::move(other.operation_);
        closed_ = std::exchange(other.closed_, true);
        return *this;
    }

    Navigation3DPrepareService::~Navigation3DPrepareService()
    {
        close();
    }

    Navigation3DPrepareClient
    Navigation3DPrepareService::client() const noexcept
    {
        return !closed_ && control_
                   ? Navigation3DPrepareClient{
                         control_, control_->currentGeneration(), operation_}
                   : Navigation3DPrepareClient{};
    }

    void Navigation3DPrepareService::close() noexcept
    {
        if (closed_)
            return;
        closed_ = true;
        if (control_)
            control_->closeAdmission();
        operation_ = {};
    }
} // namespace lux::runtime::spatial3d
