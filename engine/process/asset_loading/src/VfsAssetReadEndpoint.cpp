#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>

#include <lux/engine/process/TaskScope.hpp>

#include <exec/materialize.hpp>
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
                return lux::async::ESubmitError::QUEUE_FULL;
            if (error == EExecutionError::STOPPING || error == EExecutionError::ALREADY_JOINED)
                return lux::async::ESubmitError::STOPPING;
            if (error == EExecutionError::ALLOCATION_FAILURE)
                return lux::async::ESubmitError::BYTE_BUDGET_EXHAUSTED;
            return lux::async::ESubmitError::UNKNOWN_OPERATION;
        }
    } // namespace

    struct VfsAssetReadEndpoint::Impl final
    {
        Impl(asset::AssetVfsView view, BlockingScheduler scheduler, std::size_t requested_capacity) noexcept
            : vfs(std::move(view)), blocking(std::move(scheduler)), capacity(requested_capacity),
              owner_thread(std::this_thread::get_id())
        {
        }

        struct Request final
        {
            std::shared_ptr<VfsAssetReadEndpoint> endpoint;
            void* completion_state{};
            void (*complete)(void*, Outcome&&) noexcept{};

            void finish(Outcome outcome) noexcept
            {
                complete(completion_state, std::move(outcome));
                std::lock_guard lock{endpoint->impl_->mutex};
                --endpoint->impl_->admitted;
            }
        };

        asset::AssetVfsView vfs;
        BlockingScheduler blocking;
        TaskScope tasks;
        std::mutex mutex;
        std::size_t capacity{};
        std::size_t admitted{};
        EEndpointState state{EEndpointState::ACTIVE};
        std::thread::id owner_thread;
    };

    VfsAssetReadEndpoint::VfsAssetReadEndpoint(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
    {
    }

    VfsAssetReadEndpoint::CreateResult VfsAssetReadEndpoint::create(
        asset::AssetVfsView vfs,
        BlockingScheduler blocking,
        VfsAssetReadEndpointConfig config
    ) noexcept
    {
        if (!vfs || !blocking || config.request_capacity == 0U)
            return lux::cxx::unexpected(EVfsAssetReadEndpointError::INVALID_ARGUMENT);
        try
        {
            auto impl = std::make_unique<Impl>(std::move(vfs), std::move(blocking), config.request_capacity);
            return std::shared_ptr<VfsAssetReadEndpoint>(new VfsAssetReadEndpoint(std::move(impl)));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EVfsAssetReadEndpointError::ALLOCATION_FAILURE);
        }
    }

    VfsAssetReadEndpoint::~VfsAssetReadEndpoint()
    {
        if (impl_->state != EEndpointState::JOINED)
            std::terminate();
    }

    AssetReadPort VfsAssetReadEndpoint::port() noexcept
    {
        return AssetReadPort{weak_from_this().lock()};
    }

    lux::async::SubmitResult VfsAssetReadEndpoint::submit(
        ReadAssetImage operation,
        void* completion_state,
        void (*complete)(void*, Outcome&&) noexcept,
        lux::async::SubmitOptions
    ) noexcept
    {
        if (operation.id.isNull())
            return lux::cxx::unexpected(lux::async::ESubmitError::PAYLOAD_INVALID);
        {
            std::lock_guard lock{impl_->mutex};
            if (impl_->state != EEndpointState::ACTIVE)
                return lux::cxx::unexpected(lux::async::ESubmitError::STOPPING);
            if (impl_->admitted == impl_->capacity)
                return lux::cxx::unexpected(lux::async::ESubmitError::QUEUE_FULL);
            ++impl_->admitted;
        }

        std::shared_ptr<Impl::Request> request;
        try
        {
            request = std::make_shared<Impl::Request>(Impl::Request{
                shared_from_this(),
                completion_state,
                complete
            });
            auto read = stdexec::schedule(impl_->blocking) |
                stdexec::then([endpoint = request->endpoint, id = operation.id]() noexcept {
                    return endpoint->impl_->vfs.open(id);
                });
            auto terminal = exec::materialize(std::move(read)) |
                stdexec::then([request]<class Tag, class... Values>(Tag, Values&&... values) noexcept {
                    if constexpr (std::same_as<Tag, stdexec::set_value_t>)
                    {
                        auto result = (std::forward<Values>(values), ...);
                        if (result)
                            request->finish(Outcome{std::move(*result)});
                        else
                            request->finish(lux::cxx::unexpected(
                                lux::async::OperationFailure<asset::EAssetStorageError>::domain(result.error())
                            ));
                    }
                    else if constexpr (std::same_as<Tag, stdexec::set_error_t>)
                    {
                        const auto error = (std::forward<Values>(values), ...);
                        request->finish(lux::cxx::unexpected(
                            lux::async::OperationFailure<asset::EAssetStorageError>::runtime(
                                mapExecutionError(error)
                            )
                        ));
                    }
                    else
                    {
                        request->finish(lux::cxx::unexpected(
                            lux::async::OperationFailure<asset::EAssetStorageError>::runtime(
                                lux::async::ESubmitError::STOPPING
                            )
                        ));
                    }
                });
            const auto started = impl_->tasks.start(std::move(terminal));
            if (started)
                return {};

            std::lock_guard lock{impl_->mutex};
            --impl_->admitted;
            return lux::cxx::unexpected(
                started.error() == ETaskStartError::STOPPING ? lux::async::ESubmitError::STOPPING :
                                                               lux::async::ESubmitError::BYTE_BUDGET_EXHAUSTED
            );
        }
        catch (const std::bad_alloc&)
        {
            std::lock_guard lock{impl_->mutex};
            --impl_->admitted;
            return lux::cxx::unexpected(lux::async::ESubmitError::BYTE_BUDGET_EXHAUSTED);
        }
        catch (...)
        {
            std::lock_guard lock{impl_->mutex};
            --impl_->admitted;
            return lux::cxx::unexpected(lux::async::ESubmitError::UNKNOWN_OPERATION);
        }
    }

    void VfsAssetReadEndpoint::requestStop() noexcept
    {
        {
            std::lock_guard lock{impl_->mutex};
            if (impl_->state != EEndpointState::ACTIVE)
                return;
            impl_->state = EEndpointState::STOPPING;
        }
        impl_->tasks.requestStop();
    }

    lux::cxx::expected<void, EVfsAssetReadEndpointError> VfsAssetReadEndpoint::join() noexcept
    {
        if (impl_->owner_thread != std::this_thread::get_id())
            return lux::cxx::unexpected(EVfsAssetReadEndpointError::WRONG_THREAD);
        {
            std::lock_guard lock{impl_->mutex};
            if (impl_->state == EEndpointState::JOINED)
                return lux::cxx::unexpected(EVfsAssetReadEndpointError::ALREADY_JOINED);
            if (impl_->state != EEndpointState::STOPPING)
                return lux::cxx::unexpected(EVfsAssetReadEndpointError::INVALID_STATE);
        }

        const auto closed = stdexec::sync_wait(impl_->tasks.close());
        if (!closed)
            return lux::cxx::unexpected(EVfsAssetReadEndpointError::INVALID_STATE);
        {
            std::lock_guard lock{impl_->mutex};
            if (impl_->admitted != 0U)
                return lux::cxx::unexpected(EVfsAssetReadEndpointError::INVALID_STATE);
            impl_->state = EEndpointState::JOINED;
        }
        return {};
    }
} // namespace lux::process::asset_loading
