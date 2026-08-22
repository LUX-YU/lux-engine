#include <lux/engine/runtime/assets/navigation/Navigation3DPrepareService.hpp>

#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>

#include <stdexec/execution.hpp>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <new>
#include <utility>

namespace lux::runtime::assets::navigation::detail
{
    using Operation =
        lux::ecs::navigation::streaming::BuildNavigationRegion3D;
    using Outcome = lux::async::OperationOutcome<Operation>;

    struct Navigation3DPrepareControl final
        : public std::enable_shared_from_this<Navigation3DPrepareControl>
    {
        explicit Navigation3DPrepareControl(
            Navigation3DPrepareQueueConfig value) noexcept
            : config(value)
        {}

        void closeAdmission() noexcept
        {
            std::lock_guard lock{mutex};
            closing = true;
        }

        [[nodiscard]] bool isClosing() const noexcept
        {
            std::lock_guard lock{mutex};
            return closing;
        }

        void release(std::size_t bytes) noexcept
        {
            std::lock_guard lock{mutex};
            if (active_requests == 0u || active_bytes < bytes)
                std::abort();
            --active_requests;
            active_bytes -= bytes;
        }

        struct Reservation final
        {
            Reservation(
                std::shared_ptr<Navigation3DPrepareControl> owner_value,
                std::size_t bytes_value) noexcept
                : owner(std::move(owner_value)), bytes(bytes_value)
            {}

            ~Reservation()
            {
                if (owner)
                    owner->release(bytes);
            }

            std::shared_ptr<Navigation3DPrepareControl> owner;
            std::size_t bytes{0u};
        };

        [[nodiscard]] lux::cxx::expected<
            std::shared_ptr<Reservation>,
            lux::async::ESubmitError>
        reserve(std::size_t bytes) noexcept
        {
            std::lock_guard lock{mutex};
            if (closing)
            {
                return lux::cxx::unexpected(
                    lux::async::ESubmitError::FEATURE_CLOSING);
            }
            if (active_requests >= config.capacity)
            {
                return lux::cxx::unexpected(
                    lux::async::ESubmitError::QUEUE_FULL);
            }
            if (bytes > config.byte_budget ||
                active_bytes > config.byte_budget - bytes)
            {
                return lux::cxx::unexpected(
                    lux::async::ESubmitError::BYTE_BUDGET_EXHAUSTED);
            }
            ++active_requests;
            active_bytes += bytes;
            return std::make_shared<Reservation>(shared_from_this(), bytes);
        }

        mutable std::mutex mutex;
        Navigation3DPrepareQueueConfig config;
        bool closing{false};
        std::size_t active_requests{0u};
        std::size_t active_bytes{0u};
    };

    struct ForwardCompletion final
    {
        std::shared_ptr<Navigation3DPrepareControl::Reservation> admission;
        void* state{nullptr};
        void (*complete)(void*, Outcome&&) noexcept{nullptr};
    };

    class AdmissionEndpoint final
        : public lux::async::detail::OperationEndpoint<Operation>
    {
    public:
        AdmissionEndpoint(
            std::shared_ptr<Navigation3DPrepareControl> control,
            lux::async::OperationPort<Operation> operation) noexcept
            : control_(std::move(control)), operation_(std::move(operation))
        {}

        [[nodiscard]] lux::async::SubmitResult submit(
            Operation operation,
            void* completion_state,
            void (*complete)(void*, Outcome&&) noexcept,
            lux::async::SubmitOptions) noexcept override
        {
            const auto bytes = operation.blob.payload.size();
            auto admission = control_->reserve(bytes);
            if (!admission)
            {
                const auto error = admission.error();
                complete(
                    completion_state,
                    lux::cxx::unexpected(
                        lux::async::OperationFailure<Operation::Error>::
                            runtime(error)));
                return lux::cxx::unexpected(error);
            }

            auto forward = std::unique_ptr<ForwardCompletion>{
                new (std::nothrow) ForwardCompletion{
                    std::move(*admission), completion_state, complete}};
            if (!forward)
            {
                const auto error = lux::async::ESubmitError::QUEUE_FULL;
                complete(
                    completion_state,
                    lux::cxx::unexpected(
                        lux::async::OperationFailure<Operation::Error>::
                            runtime(error)));
                return lux::cxx::unexpected(error);
            }

            auto* const callback_state = forward.release();
            return operation_.submit(
                std::move(operation),
                callback_state,
                +[](void* value, Outcome&& outcome) noexcept
                {
                    auto owner = std::unique_ptr<ForwardCompletion>{
                        static_cast<ForwardCompletion*>(value)};
                    owner->complete(owner->state, std::move(outcome));
                },
                lux::async::SubmitOptions{.accounted_bytes = bytes});
        }

    private:
        std::shared_ptr<Navigation3DPrepareControl> control_;
        lux::async::OperationPort<Operation> operation_;
    };
} // namespace lux::runtime::assets::navigation::detail

namespace lux::runtime::assets::navigation
{
    namespace ex = stdexec;
    using Operation =
        lux::ecs::navigation::streaming::BuildNavigationRegion3D;

    namespace
    {
        using NavigationPrepareFailure =
            lux::navigation::detour3d::NavigationRegion3DFailure;
        template <typename T>
        using NavigationPrepareExp =
            lux::cxx::expected<T, NavigationPrepareFailure>;
        using PrepareResult = NavigationPrepareExp<
            lux::navigation::detour3d::PreparedNavigationRegion3D>;

        struct PendingPrepare final
        {
            explicit PendingPrepare(
                lux::exec::AsyncOperationCompletion<Operation> value) noexcept
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
                    lux::async::OperationFailure<Operation::Error>::domain(
                        std::move(result.error()))));
            }

            void stop() noexcept
            {
                if (settled.exchange(true, std::memory_order_acq_rel))
                    return;
                completion.failRuntime(lux::async::ESubmitError::STOPPING);
            }

            std::atomic<bool> settled{false};
            lux::exec::AsyncOperationCompletion<Operation> completion;
        };
    } // namespace

    lux::cxx::expected<
        Navigation3DPrepareService,
        lux::exec::AsyncAssemblyFailure>
    Navigation3DPrepareService::addTo(
        lux::exec::AsyncRuntimeBuilder& builder,
        Navigation3DPrepareQueueConfig config)
    {
        auto control =
            std::make_shared<detail::Navigation3DPrepareControl>(config);
        auto state = std::make_shared<lux::ecs::navigation::streaming::detail::
            Navigation3DPrepareState>();
        auto endpoint = builder.addOperation<Operation>(
            [control](
                Operation&& request,
                lux::exec::AsyncOperationContext& context,
                lux::exec::AsyncOperationCompletion<Operation>&& completion)
                noexcept
            {
                auto pending = std::make_shared<PendingPrepare>(
                    std::move(completion));
                if (control->isClosing())
                {
                    pending->stop();
                    return;
                }
                if (!request.blob.valid() ||
                    request.request_generation == 0u)
                {
                    pending->settle(lux::cxx::unexpected(
                        NavigationPrepareFailure{
                            lux::navigation::detour3d::
                                ENavigationRegion3DError::INVALID_REQUEST,
                            "navigation preparation request is invalid"}));
                    return;
                }
                auto work =
                    ex::schedule(
                        lux::exec::backgroundCpuScheduler(context.runtime())) |
                    ex::then(
                        [request = std::move(request)]() mutable noexcept
                            -> PrepareResult
                        {
                            return lux::navigation::detour3d::
                                prepareNavigationRegion3D(
                                    std::move(request.blob),
                                    request.request_generation);
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
                config.capacity, config.byte_budget, config.drain_batch});
        if (!endpoint)
            return lux::cxx::unexpected(endpoint.error());

        auto operation = lux::async::OperationPort<Operation>{
            std::make_shared<detail::AdmissionEndpoint>(
                control, std::move(*endpoint))};
        return Navigation3DPrepareService{
            std::move(control), std::move(state), std::move(operation)};
    }

    Navigation3DPrepareService::Navigation3DPrepareService(
        std::shared_ptr<detail::Navigation3DPrepareControl> control,
        std::shared_ptr<lux::ecs::navigation::streaming::detail::
            Navigation3DPrepareState> state,
        lux::async::OperationPort<Operation> operation) noexcept
        : control_(std::move(control)), state_(std::move(state)),
          operation_(std::move(operation))
    {}

    Navigation3DPrepareService::Navigation3DPrepareService(
        Navigation3DPrepareService&& other) noexcept
        : control_(std::move(other.control_)), state_(std::move(other.state_)),
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
        state_ = std::move(other.state_);
        operation_ = std::move(other.operation_);
        closed_ = std::exchange(other.closed_, true);
        return *this;
    }

    Navigation3DPrepareService::~Navigation3DPrepareService()
    {
        close();
    }

    lux::ecs::navigation::streaming::Navigation3DPrepareClient
    Navigation3DPrepareService::client() const noexcept
    {
        return !closed_ && state_
            ? lux::ecs::navigation::streaming::Navigation3DPrepareClient{
                  state_, operation_}
            : lux::ecs::navigation::streaming::Navigation3DPrepareClient{};
    }

    void Navigation3DPrepareService::close() noexcept
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
} // namespace lux::runtime::assets::navigation
