#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>

#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/process/PortSender.hpp>

#include <stdexec/execution.hpp>

#include <atomic>
#include <exception>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

namespace lux::process::asset_loading
{
    namespace
    {
        enum class EEndpointState : std::uint8_t
        {
            ACTIVE,
            STOPPING,
            JOINED,
        };

        [[nodiscard]] lux::async::ESubmitError mapExecutionError(EExecutionError error) noexcept
        {
            if (error == EExecutionError::CAPACITY_EXCEEDED)
            {
                return lux::async::ESubmitError::QUEUE_FULL;
            }
            if (error == EExecutionError::STOPPING || error == EExecutionError::ALREADY_JOINED)
            {
                return lux::async::ESubmitError::STOPPING;
            }
            if (error == EExecutionError::ALLOCATION_FAILURE)
            {
                return lux::async::ESubmitError::BYTE_BUDGET_EXHAUSTED;
            }
            return lux::async::ESubmitError::UNKNOWN_OPERATION;
        }
    } // namespace

    struct VfsAssetReadEndpoint::Impl final
    {
        Impl(
            asset::AssetVfsView view,
            BlockingScheduler scheduler,
            TaskScope& scope,
            std::size_t requested_capacity
        ) noexcept
            : vfs(std::move(view)), blocking(std::move(scheduler)), tasks(scope), capacity(requested_capacity),
              owner_thread(std::this_thread::get_id())
        {}

        struct Request final
        {
            std::shared_ptr<VfsAssetReadEndpoint> endpoint;
            void* completion_state{};
            void (*complete)(void*, Outcome&&) noexcept {};

            void finish(Outcome outcome) noexcept
            {
                complete(completion_state, std::move(outcome));
                std::lock_guard lock{endpoint->impl_->mutex};
                --endpoint->impl_->admitted;
                endpoint->impl_->tasks.execution().wake();
            }
        };

        asset::AssetVfsView vfs;
        BlockingScheduler blocking;
        TaskScope& tasks;
        std::mutex mutex;
        std::size_t capacity{};
        std::size_t admitted{};
        EEndpointState state{EEndpointState::ACTIVE};
        std::thread::id owner_thread;
    };

    VfsAssetReadEndpoint::VfsAssetReadEndpoint(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    VfsAssetReadEndpoint::CreateResult VfsAssetReadEndpoint::create(
        asset::AssetVfsView vfs,
        BlockingScheduler blocking,
        TaskScope& tasks,
        VfsAssetReadEndpointConfig config
    ) noexcept
    {
        if (!vfs || !blocking || config.request_capacity == 0U)
        {
            return lux::cxx::unexpected(EVfsAssetReadEndpointError::INVALID_ARGUMENT);
        }
        auto impl = std::make_unique<Impl>(std::move(vfs), std::move(blocking), tasks, config.request_capacity);
        return std::shared_ptr<VfsAssetReadEndpoint>(new VfsAssetReadEndpoint(std::move(impl)));
    }

    VfsAssetReadEndpoint::~VfsAssetReadEndpoint() = default;

    AssetReadPort VfsAssetReadEndpoint::port() noexcept
    {
        return AssetReadPort{weak_from_this().lock()};
    }

    lux::async::SubmitResult VfsAssetReadEndpoint::submit(
        ReadAssetImage operation,
        void* completion_state,
        void (*complete)(void*, Outcome&&) noexcept,
        lux::async::SubmitOptions options
    ) noexcept
    {
        if (operation.id.isNull())
        {
            return lux::cxx::unexpected(lux::async::ESubmitError::PAYLOAD_INVALID);
        }
        {
            std::lock_guard lock{impl_->mutex};
            if (impl_->state != EEndpointState::ACTIVE)
            {
                return lux::cxx::unexpected(lux::async::ESubmitError::STOPPING);
            }
            if (impl_->admitted == impl_->capacity)
            {
                return lux::cxx::unexpected(lux::async::ESubmitError::QUEUE_FULL);
            }
            ++impl_->admitted;
        }

        auto request = std::make_shared<Impl::Request>(Impl::Request{shared_from_this(), completion_state, complete});
        const auto started = impl_->tasks.submit(
            {"Read asset image", "asset", correlatedTask(options.correlation)},
            [scheduler = impl_->blocking, endpoint = request->endpoint, id = operation.id](TaskReporter) noexcept {
                return stdexec::then(stdexec::schedule(scheduler), [endpoint, id]() noexcept {
                    return endpoint->impl_->vfs.open(id);
                });
            },
            [request](auto&& result) noexcept {
                using Failure = lux::async::TOperationFailure<asset::EAssetStorageError>;
                if (result)
                    request->finish(Outcome{std::move(*result)});
                else if (auto* error = result.error().domainFailure())
                    request->finish(lux::cxx::unexpected(Failure::domain(*error)));
                else if (auto* error = result.error().executionFailure())
                    request->finish(lux::cxx::unexpected(Failure::runtime(mapExecutionError(*error))));
                else
                    request->finish(lux::cxx::unexpected(Failure::runtime(lux::async::ESubmitError::STOPPING)));
            }
        );
        if (started)
            return {};
        std::lock_guard lock{impl_->mutex};
        --impl_->admitted;
        return lux::cxx::unexpected(mapExecutionError(started.error()));
    }

    void VfsAssetReadEndpoint::requestStop() noexcept
    {
        {
            std::lock_guard lock{impl_->mutex};
            if (impl_->state != EEndpointState::ACTIVE)
            {
                return;
            }
            impl_->state = EEndpointState::STOPPING;
        }
    }

    lux::cxx::expected<void, EVfsAssetReadEndpointError> VfsAssetReadEndpoint::join() noexcept
    {
        if (impl_->owner_thread != std::this_thread::get_id())
        {
            return lux::cxx::unexpected(EVfsAssetReadEndpointError::WRONG_THREAD);
        }
        {
            std::lock_guard lock{impl_->mutex};
            if (impl_->state == EEndpointState::JOINED)
            {
                return lux::cxx::unexpected(EVfsAssetReadEndpointError::ALREADY_JOINED);
            }
            if (impl_->state != EEndpointState::STOPPING)
            {
                return lux::cxx::unexpected(EVfsAssetReadEndpointError::INVALID_STATE);
            }
        }

        {
            std::lock_guard lock{impl_->mutex};
            if (impl_->admitted != 0U)
            {
                return lux::cxx::unexpected(EVfsAssetReadEndpointError::BUSY);
            }
            impl_->state = EEndpointState::JOINED;
        }
        return {};
    }
} // namespace lux::process::asset_loading
