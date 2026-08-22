#pragma once
/**
 * @file AsyncRuntimeBuilder.hpp
 * @brief Freeze-time registration for AsyncRuntime typed operations.
 */

#include <lux/engine/runtime/execution/AsyncOperation.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::exec
{
    class AsyncOperationBundle;
    enum class EAsyncAssemblyError : std::uint8_t
    {
        DUPLICATE_OPERATION,
        MISSING_DEPENDENCY,
        TYPE_COLLISION,
        INVALID_HANDLER,
        INVALID_QUEUE
    };

    struct AsyncAssemblyFailure final
    {
        EAsyncAssemblyError code{EAsyncAssemblyError::INVALID_HANDLER};
        lux::cxx::TypeToken operation{};
        lux::cxx::TypeToken dependency{};
    };

    struct AsyncOperationRegistrationOptions final
    {
        std::vector<lux::cxx::TypeToken> prerequisites;
        std::shared_ptr<const void> module_lease;
    };

    namespace detail
    {
        struct AsyncRegistration final
        {
            lux::cxx::TypeToken type{};
            std::vector<lux::cxx::TypeToken> prerequisites;
            std::shared_ptr<void> handler_state;
            std::shared_ptr<const void> module_lease;
            std::shared_ptr<AsyncEndpointBase> endpoint;
            void (*drain)(
                void*,
                AsyncEndpointBase&,
                AsyncOperationContext&,
                std::size_t) noexcept{nullptr};
            void (*rejectAll)(
                AsyncEndpointBase&,
                lux::async::ESubmitError) noexcept{nullptr};
        };
    }

    class AsyncRuntimePlan final
    {
    public:
        AsyncRuntimePlan() noexcept = default;
        ~AsyncRuntimePlan() = default;

        AsyncRuntimePlan(const AsyncRuntimePlan&) = delete;
        AsyncRuntimePlan& operator=(const AsyncRuntimePlan&) = delete;
        AsyncRuntimePlan(AsyncRuntimePlan&&) noexcept = default;
        AsyncRuntimePlan& operator=(AsyncRuntimePlan&&) noexcept = default;

    private:
        friend class AsyncRuntime;
        friend class AsyncRuntimeBuilder;

        std::vector<detail::AsyncRegistration> registrations_;
    };

    /// Move-only registration bundle used for runtime operation bundle installation.
    /// Validation against the live registry happens on the coordinator.
    class AsyncOperationBundle final
    {
    public:
        AsyncOperationBundle() noexcept = default;
        ~AsyncOperationBundle() = default;

        AsyncOperationBundle(const AsyncOperationBundle&) = delete;
        AsyncOperationBundle& operator=(const AsyncOperationBundle&) = delete;
        AsyncOperationBundle(AsyncOperationBundle&&) noexcept = default;
        AsyncOperationBundle& operator=(AsyncOperationBundle&&) noexcept = default;

        [[nodiscard]] bool empty() const noexcept
        {
            return registrations_.empty();
        }

    private:
        friend class AsyncRuntimeBuilder;
        friend std::vector<detail::AsyncRegistration>
        detailTakeOperationRegistrations(AsyncOperationBundle&&) noexcept;

        std::vector<detail::AsyncRegistration> registrations_;
    };

    inline std::vector<detail::AsyncRegistration>
    detailTakeOperationRegistrations(AsyncOperationBundle&& package) noexcept
    {
        return std::move(package.registrations_);
    }

    class AsyncRuntimeBuilder final
    {
    public:
        AsyncRuntimeBuilder() = default;

        template <AsyncOperation Operation, class Handler>
        [[nodiscard]] lux::cxx::expected<
            lux::async::OperationPort<Operation>,
            AsyncAssemblyFailure>
        addOperation(
            Handler handler,
            AsyncOperationRegistrationOptions options = {},
            AsyncOperationQueueConfig queue = {})
        {
            using Completion = detail::AsyncCompletion<Operation>;
            using StoredHandler = std::decay_t<Handler>;
            static_assert(
                std::is_nothrow_invocable_v<
                    StoredHandler&,
                    Operation&&,
                    AsyncOperationContext&,
                    Completion&&>,
                "Async operation handler must be noexcept and accept "
                "(Operation&&, AsyncOperationContext&, AsyncCompletion&&)");

            constexpr lux::cxx::TypeToken token =
                lux::async::operationType<Operation>();
            if (!queue.valid())
            {
                return lux::cxx::unexpected(AsyncAssemblyFailure{
                    EAsyncAssemblyError::INVALID_QUEUE,
                    token,
                    {}});
            }
            for (const auto& registration : registrations_)
            {
                if (registration.type.hash() != token.hash())
                    continue;
                return lux::cxx::unexpected(AsyncAssemblyFailure{
                    registration.type.name() == token.name()
                        ? EAsyncAssemblyError::DUPLICATE_OPERATION
                        : EAsyncAssemblyError::TYPE_COLLISION,
                    token,
                    registration.type});
            }

            auto state = std::make_shared<StoredHandler>(std::move(handler));
            auto endpoint =
                std::make_shared<detail::OperationEndpoint<Operation>>(queue);
            detail::AsyncRegistration registration;
            registration.type = token;
            registration.prerequisites = std::move(options.prerequisites);
            registration.handler_state = std::move(state);
            registration.module_lease = std::move(options.module_lease);
            registration.endpoint = endpoint;
            registration.drain = +[](
                void* opaque,
                detail::AsyncEndpointBase& erased_endpoint,
                AsyncOperationContext& context,
                std::size_t budget) noexcept
            {
                auto& typed_handler = *static_cast<StoredHandler*>(opaque);
                auto& typed_endpoint = static_cast<
                    detail::OperationEndpoint<Operation>&>(erased_endpoint);
                typename detail::OperationEndpoint<Operation>::Queued queued;
                for (std::size_t i = 0u;
                     i < budget && typed_endpoint.tryTake(queued);
                     ++i)
                {
                    const auto handler_start = detail::asyncSteadyNowNs();
                    typed_endpoint.recordQueueWait(
                        handler_start - queued.enqueued_ns
                    );
                    auto completion = queued.takeCompletion();
                    typed_handler(
                        std::move(*queued.operation),
                        context,
                        std::move(completion));
                    typed_endpoint.recordHandler(
                        detail::asyncSteadyNowNs() - handler_start
                    );
                    queued = {};
                }
            };
            registration.rejectAll = +[](
                detail::AsyncEndpointBase& erased_endpoint,
                lux::async::ESubmitError error) noexcept
            {
                static_cast<detail::OperationEndpoint<Operation>&>(
                    erased_endpoint).rejectAll(error);
            };
            registrations_.push_back(std::move(registration));
            return lux::async::OperationPort<Operation>{std::move(endpoint)};
        }

        [[nodiscard]] lux::cxx::expected<
            AsyncRuntimePlan,
            AsyncAssemblyFailure>
        compile() &&
        {
            for (const auto& registration : registrations_)
            {
                for (const auto& dependency : registration.prerequisites)
                {
                    bool found = false;
                    for (const auto& candidate : registrations_)
                    {
                        if (candidate.type == dependency)
                        {
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        return lux::cxx::unexpected(AsyncAssemblyFailure{
                            EAsyncAssemblyError::MISSING_DEPENDENCY,
                            registration.type,
                            dependency});
                    }
                }
            }

            AsyncRuntimePlan plan;
            plan.registrations_ = std::move(registrations_);
            return plan;
        }

        /// Freeze a package whose prerequisites may be supplied by the live
        /// runtime. Duplicate/collision checks already happened in addOperation;
        /// missing dependencies are validated atomically during install.
        [[nodiscard]] AsyncOperationBundle compileOperations() && noexcept
        {
            AsyncOperationBundle package;
            package.registrations_ = std::move(registrations_);
            return package;
        }

    private:
        std::vector<detail::AsyncRegistration> registrations_;
    };
}
