#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>

#include <stdexec/execution.hpp>

#include <mutex>
#include <optional>
#include <utility>

namespace lux::process::asset_loading
{
    namespace
    {
        [[nodiscard]] lux::async::ESubmitError mapExecutionError(execution::EExecutionError error) noexcept
        {
            if (error == execution::EExecutionError::CAPACITY_EXCEEDED)
            {
                return lux::async::ESubmitError::QUEUE_FULL;
            }
            const bool admission_closed =
                error == execution::EExecutionError::STOPPING || error == execution::EExecutionError::ALREADY_JOINED;
            if (admission_closed)
            {
                return lux::async::ESubmitError::STOPPING;
            }
            if (error == execution::EExecutionError::ALLOCATION_FAILURE)
            {
                std::terminate(); // Ordinary heap exhaustion is not an explicit read budget failure.
            }
            return lux::async::ESubmitError::UNKNOWN_OPERATION;
        }
    } // namespace

    // Copied ports retain this private admission endpoint, never the public semantic owner.
    // A missing admission is final; already accepted requests need only the bounded counter.
    struct VfsAssetReadEndpoint::Impl final : AssetReadPort::Endpoint, std::enable_shared_from_this<Impl>
    {
        struct Admission final
        {
            asset::AssetVfsView vfs;
            execution::BlockingScheduler blocking;
            execution::ExecutionRuntime& execution;
            std::stop_token stop;
        };

        struct Request final
        {
            std::shared_ptr<Impl> state;
            void* completion_state;
            void (*complete)(void*, Outcome&&) noexcept;

            void finish(Outcome outcome) noexcept
            {
                {
                    std::lock_guard lock{state->mutex};
                    --state->admitted;
                }
                // Read admission ends before continuation; stdexec retains the inputs through callback return.
                complete(completion_state, std::move(outcome));
            }
        };

        Impl(
            asset::AssetVfsView view,
            execution::BlockingScheduler scheduler,
            execution::ExecutionRuntime& execution,
            std::size_t limit,
            std::stop_token stop
        ) noexcept
            : admission(std::in_place, std::move(view), std::move(scheduler), execution, stop), capacity(limit)
        {
        }

        void revoke() noexcept
        {
            // Provider cleanup may re-enter a copied port. Release inputs only after unlocking.
            std::optional<Admission> removed;
            {
                std::lock_guard lock{mutex};
                removed.emplace(std::move(*admission));
                admission.reset();
            }
        }

        lux::async::SubmitResult submit(
            ReadAssetImage operation,
            void* completion_state,
            void (*complete)(void*, Outcome&&) noexcept,
            lux::async::SubmitOptions
        ) noexcept override
        {
            if (operation.id.isNull())
            {
                return lux::cxx::unexpected(lux::async::ESubmitError::PAYLOAD_INVALID);
            }
            std::optional<Admission> accepted;
            {
                std::lock_guard lock{mutex};
                if (!admission || admission->stop.stop_requested())
                {
                    return lux::cxx::unexpected(lux::async::ESubmitError::STOPPING);
                }
                if (admitted == capacity)
                {
                    return lux::cxx::unexpected(lux::async::ESubmitError::QUEUE_FULL);
                }
                accepted.emplace(*admission);
                ++admitted;
            }
            Request request{shared_from_this(), completion_state, complete};
            auto read = stdexec::then(
                stdexec::schedule(accepted->blocking),
                [view = accepted->vfs, operation]() noexcept { return view.open(operation.id, operation.max_bytes); }
            );
            auto delivered = stdexec::then(
                execution::taskResult(std::move(read)),
                [request = std::move(request)](auto result) mutable noexcept
                {
                    using Failure = lux::async::TOperationFailure<asset::EAssetStorageError>;
                    if (result)
                    {
                        request.finish(Outcome{std::move(*result)});
                    }
                    else if (auto* error = result.error().domainFailure())
                    {
                        request.finish(lux::cxx::unexpected(Failure::domain(*error)));
                    }
                    else if (auto* error = result.error().executionFailure())
                    {
                        request.finish(lux::cxx::unexpected(Failure::runtime(mapExecutionError(*error))));
                    }
                    else
                    {
                        request.finish(lux::cxx::unexpected(Failure::runtime(lux::async::ESubmitError::STOPPING)));
                    }
                }
            );
            const auto started = accepted->execution.spawn(
                stdexec::write_env(std::move(delivered), stdexec::prop{stdexec::get_stop_token, accepted->stop})
            );
            if (started)
            {
                return {};
            }
            std::lock_guard lock{mutex};
            --admitted;
            return lux::cxx::unexpected(mapExecutionError(started.error()));
        }

        std::mutex mutex;
        std::optional<Admission> admission;
        const std::size_t capacity;
        std::size_t admitted{};
    };

    VfsAssetReadEndpoint::VfsAssetReadEndpoint(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    VfsAssetReadEndpoint::CreateResult VfsAssetReadEndpoint::create(
        asset::AssetVfsView vfs,
        execution::BlockingScheduler blocking,
        execution::ExecutionRuntime& execution,
        VfsAssetReadEndpointConfig config,
        std::stop_token stop
    ) noexcept
    {
        const bool invalid_dependency = !vfs || !blocking;
        const bool invalid_capacity = config.request_capacity == 0U;
        const bool invalid_input = invalid_dependency || invalid_capacity;
        if (invalid_input)
        {
            return lux::cxx::unexpected(EVfsAssetReadEndpointError::INVALID_ARGUMENT);
        }
        auto impl =
            std::make_shared<Impl>(std::move(vfs), std::move(blocking), execution, config.request_capacity, stop);
        return std::unique_ptr<VfsAssetReadEndpoint>(new VfsAssetReadEndpoint(std::move(impl)));
    }

    VfsAssetReadEndpoint::~VfsAssetReadEndpoint() noexcept
    {
        impl_->revoke();
    }

    AssetReadPort VfsAssetReadEndpoint::port() const noexcept
    {
        return AssetReadPort{impl_};
    }
} // namespace lux::process::asset_loading
