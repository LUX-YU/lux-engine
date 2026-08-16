#include "AsyncFileControlFactory.hpp"

#include <lux/engine/runtime/execution/detail/AsioConfig.hpp>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/post.hpp>
#if defined(ASIO_HAS_FILE)
#include <asio/random_access_file.hpp>
#include <asio/read_at.hpp>
#include <asio/write_at.hpp>
#endif

#include <exec/start_detached.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <cerrno>
#include <fstream>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(ASIO_HAS_FILE)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lux::exec::detail
{
    namespace
    {
        namespace ex = stdexec;
        using FileWorkGuard = asio::executor_work_guard<
            asio::io_context::executor_type>;

        class ActiveCounterGuard final
        {
        public:
            explicit ActiveCounterGuard(
                std::atomic<std::size_t>& counter) noexcept
                : counter_(&counter)
            {
                counter_->fetch_add(1u, std::memory_order_relaxed);
            }
            ~ActiveCounterGuard()
            {
                counter_->fetch_sub(1u, std::memory_order_release);
            }

            ActiveCounterGuard(const ActiveCounterGuard&) = delete;
            ActiveCounterGuard& operator=(const ActiveCounterGuard&) = delete;

        private:
            std::atomic<std::size_t>* counter_{nullptr};
        };

        [[nodiscard]] AsyncFileFailure failure(
            EAsyncFileError error,
            std::error_code system_error = {}) noexcept
        {
            return AsyncFileFailure{error, system_error};
        }

        [[nodiscard]] std::error_code lastSystemError() noexcept
        {
#if defined(_WIN32)
            return std::error_code(
                static_cast<int>(::GetLastError()),
                std::system_category());
#else
            return std::error_code(errno, std::generic_category());
#endif
        }

#if defined(ASIO_HAS_FILE)
#if defined(_WIN32)
        using NativeFile = HANDLE;
        // INVALID_HANDLE_VALUE is defined through an integer-to-pointer cast on
        // Windows, so it is not a constant expression in conforming C++ mode.
        inline const NativeFile kInvalidFile = INVALID_HANDLE_VALUE;

        void closeNative(NativeFile file) noexcept
        {
            if (file != kInvalidFile)
                (void)::CloseHandle(file);
        }
#else
        using NativeFile = int;
        inline constexpr NativeFile kInvalidFile = -1;

        void closeNative(NativeFile file) noexcept
        {
            if (file != kInvalidFile)
                (void)::close(file);
        }
#endif
#endif

        class AsyncFileControlImpl final
            : public AsyncFileControl,
              public std::enable_shared_from_this<AsyncFileControlImpl>
        {
        public:
            AsyncFileControlImpl(
                asio::io_context& coordinator,
                ::experimental::execution::static_thread_pool& blocking_io)
                noexcept
                : coordinator_(coordinator)
                , blocking_io_(blocking_io)
            {}

            [[nodiscard]] bool tryRead(
                std::filesystem::path path,
                AsyncFileReadOptions options,
                std::shared_ptr<AsyncFileRequestState> state,
                ReadCompletion completion) noexcept override
            {
                if (!accepting_.load(std::memory_order_acquire) ||
                    !state || !completion)
                    return false;

                const auto id = next_id_.fetch_add(
                    1u,
                    std::memory_order_relaxed);
                state->id.store(id, std::memory_order_release);
                pending_state_allocations_.fetch_add(
                    1u,
                    std::memory_order_relaxed);
                active_pending_operations_.fetch_add(
                    1u,
                    std::memory_order_relaxed);
                auto pending = std::make_shared<PendingRead>(
                    coordinator_,
                    active_pending_operations_,
                    id,
                    std::move(path),
                    options,
                    std::move(state),
                    std::move(completion));
                auto self = shared_from_this();
                auto work = ex::schedule(blocking_io_.get_scheduler())
                    | ex::then(
                        [self, pending]() noexcept
                        {
                            self->prepareRead(pending);
                        })
                    | ex::upon_stopped(
                        [self, pending]() noexcept
                        {
                            self->postStopped(pending);
                        });
                ::experimental::execution::start_detached(std::move(work));
                return true;
            }

            [[nodiscard]] bool tryWrite(
                std::filesystem::path path,
                AsyncFileBuffer bytes,
                std::shared_ptr<AsyncFileRequestState> state,
                WriteCompletion completion) noexcept override
            {
                if (!accepting_.load(std::memory_order_acquire) ||
                    !state || !completion)
                    return false;

                const auto id = next_id_.fetch_add(
                    1u,
                    std::memory_order_relaxed);
                state->id.store(id, std::memory_order_release);
                pending_state_allocations_.fetch_add(
                    1u,
                    std::memory_order_relaxed);
                active_pending_operations_.fetch_add(
                    1u,
                    std::memory_order_relaxed);
                auto pending = std::make_shared<PendingWrite>(
                    coordinator_,
                    active_pending_operations_,
                    id,
                    std::move(path),
                    std::move(bytes),
                    std::move(state),
                    std::move(completion));
                auto self = shared_from_this();
                auto work = ex::schedule(blocking_io_.get_scheduler())
                    | ex::then(
                        [self, pending]() noexcept
                        {
                            self->prepareWrite(pending);
                        })
                    | ex::upon_stopped(
                        [self, pending]() noexcept
                        {
                            self->postStopped(pending);
                        });
                ::experimental::execution::start_detached(std::move(work));
                return true;
            }

            void cancel(
                const std::shared_ptr<AsyncFileRequestState>& state)
                noexcept override
            {
                if (!state)
                    return;
                state->cancelled.store(true, std::memory_order_release);
                const auto id = state->id.load(std::memory_order_acquire);
                if (id == 0u)
                    return;
                auto self = shared_from_this();
                asio::post(
                    coordinator_,
                    [self, id]() noexcept
                    {
                        self->cancelOnCoordinator(id);
                    });
            }

            void closeAdmissionAndCancel() noexcept override
            {
                if (!accepting_.exchange(false, std::memory_order_acq_rel))
                    return;
                auto self = shared_from_this();
                asio::post(
                    coordinator_,
                    [self]() noexcept
                    {
#if defined(ASIO_HAS_FILE)
                        for (auto& [id, request] : self->requests_)
                        {
                            (void)id;
                            request->cancel();
                        }
#endif
                    });
            }

            [[nodiscard]] bool nativeAsyncAvailable() const noexcept override
            {
#if defined(ASIO_HAS_FILE)
                return true;
#else
                return false;
#endif
            }

            void recordRequestStateAllocation() noexcept override
            {
                request_state_allocations_.fetch_add(
                    1u,
                    std::memory_order_relaxed);
            }

            [[nodiscard]] AsyncFileStatistics statistics()
                const noexcept override
            {
                return {
                    .request_state_allocations =
                        request_state_allocations_.load(
                            std::memory_order_relaxed),
                    .pending_state_allocations =
                        pending_state_allocations_.load(
                            std::memory_order_relaxed),
                    .native_state_allocations =
                        native_state_allocations_.load(
                            std::memory_order_relaxed),
                    .active_pending_operations =
                        active_pending_operations_.load(
                            std::memory_order_acquire),
#if defined(ASIO_HAS_FILE)
                    .active_native_requests = active_native_requests_.load(
                        std::memory_order_relaxed),
#else
                    .active_native_requests = 0u,
#endif
                    .blocking_io_running =
                        blocking_io_running_.load(
                            std::memory_order_acquire)
                };
            }

        private:
            struct PendingRead final
            {
                PendingRead(
                    asio::io_context& coordinator,
                    std::atomic<std::size_t>& active_pending,
                    std::uint64_t id_value,
                    std::filesystem::path path_value,
                    AsyncFileReadOptions options_value,
                    std::shared_ptr<AsyncFileRequestState> state_value,
                    ReadCompletion completion_value) noexcept
                    : active_pending_(&active_pending)
                    , work_guard(asio::make_work_guard(coordinator))
                    , id(id_value)
                    , path(std::move(path_value))
                    , options(options_value)
                    , state(std::move(state_value))
                    , completion(std::move(completion_value))
                {}

                ~PendingRead()
                {
                    active_pending_->fetch_sub(
                        1u,
                        std::memory_order_release);
                }

                std::atomic<std::size_t>* active_pending_{nullptr};
                std::optional<FileWorkGuard> work_guard;
                std::uint64_t id{0u};
                std::filesystem::path path;
                AsyncFileReadOptions options;
                std::shared_ptr<AsyncFileRequestState> state;
                ReadCompletion completion;
            };

            struct PendingWrite final
            {
                PendingWrite(
                    asio::io_context& coordinator,
                    std::atomic<std::size_t>& active_pending,
                    std::uint64_t id_value,
                    std::filesystem::path path_value,
                    AsyncFileBuffer bytes_value,
                    std::shared_ptr<AsyncFileRequestState> state_value,
                    WriteCompletion completion_value) noexcept
                    : active_pending_(&active_pending)
                    , work_guard(asio::make_work_guard(coordinator))
                    , id(id_value)
                    , path(std::move(path_value))
                    , bytes(std::move(bytes_value))
                    , state(std::move(state_value))
                    , completion(std::move(completion_value))
                {}

                ~PendingWrite()
                {
                    active_pending_->fetch_sub(
                        1u,
                        std::memory_order_release);
                }

                std::atomic<std::size_t>* active_pending_{nullptr};
                std::optional<FileWorkGuard> work_guard;
                std::uint64_t id{0u};
                std::filesystem::path path;
                AsyncFileBuffer bytes;
                std::shared_ptr<AsyncFileRequestState> state;
                WriteCompletion completion;
            };

#if defined(ASIO_HAS_FILE)
            struct RequestBase
            {
                virtual ~RequestBase() = default;
                virtual void cancel() noexcept = 0;
            };

            struct ReadRequest final : RequestBase
            {
                ReadRequest(
                    asio::io_context& coordinator,
                    std::shared_ptr<PendingRead> pending_value) noexcept
                    : file(coordinator)
                    , pending(std::move(pending_value))
                {}

                void cancel() noexcept override
                {
                    std::error_code error;
                    file.cancel(error);
                }

                asio::random_access_file file;
                std::shared_ptr<PendingRead> pending;
                AsyncFileBuffer bytes;
            };

            struct WriteRequest final : RequestBase
            {
                WriteRequest(
                    asio::io_context& coordinator,
                    std::shared_ptr<PendingWrite> pending_value) noexcept
                    : file(coordinator)
                    , pending(std::move(pending_value))
                {}

                void cancel() noexcept override
                {
                    std::error_code error;
                    file.cancel(error);
                }

                asio::random_access_file file;
                std::shared_ptr<PendingWrite> pending;
            };
#endif

            template <class Pending>
            void postStopped(const std::shared_ptr<Pending>& pending) noexcept
            {
                auto self = shared_from_this();
                asio::post(
                    coordinator_,
                    [self, pending]() noexcept
                    {
                        self->finishPendingStopped(pending);
                    });
            }

            void finishPendingStopped(
                const std::shared_ptr<PendingRead>& pending) noexcept
            {
                pending->state->id.store(0u, std::memory_order_release);
                pending->work_guard.reset();
                if (pending->completion)
                    pending->completion(AsyncFileBuffer{}, true);
            }

            void finishPendingStopped(
                const std::shared_ptr<PendingWrite>& pending) noexcept
            {
                pending->state->id.store(0u, std::memory_order_release);
                pending->work_guard.reset();
                if (pending->completion)
                    pending->completion(std::size_t{0u}, true);
            }

            void prepareRead(const std::shared_ptr<PendingRead>& pending)
                noexcept
            {
                ActiveCounterGuard running{blocking_io_running_};
                if (pending->state->cancelled.load(std::memory_order_acquire) ||
                    !accepting_.load(std::memory_order_acquire))
                {
                    postStopped(pending);
                    return;
                }

#if defined(ASIO_HAS_FILE)
                std::error_code open_error;
                std::uint64_t size = 0u;
                const auto native = openRead(pending->path, size, open_error);
                if (native == kInvalidFile)
                {
                    postReadFailure(
                        pending,
                        EAsyncFileError::OPEN_FAILED,
                        open_error);
                    return;
                }
                if (size > pending->options.max_bytes ||
                    size > (std::numeric_limits<std::size_t>::max)())
                {
                    closeNative(native);
                    postReadFailure(
                        pending,
                        EAsyncFileError::FILE_TOO_LARGE,
                        {});
                    return;
                }
                auto self = shared_from_this();
                asio::post(
                    coordinator_,
                    [self, pending, native, size]() mutable noexcept
                    {
                        self->beginNativeRead(pending, native, size);
                    });
#else
                std::ifstream input(pending->path, std::ios::binary);
                if (!input)
                {
                    postReadFailure(
                        pending,
                        EAsyncFileError::OPEN_FAILED,
                        lastSystemError());
                    return;
                }
                input.seekg(0, std::ios::end);
                const auto end = input.tellg();
                if (end < 0)
                {
                    postReadFailure(
                        pending,
                        EAsyncFileError::SIZE_FAILED,
                        lastSystemError());
                    return;
                }
                const auto size = static_cast<std::uint64_t>(end);
                if (size > pending->options.max_bytes ||
                    size > (std::numeric_limits<std::size_t>::max)())
                {
                    postReadFailure(
                        pending,
                        EAsyncFileError::FILE_TOO_LARGE,
                        {});
                    return;
                }
                input.seekg(0, std::ios::beg);
                AsyncFileBuffer bytes(static_cast<std::size_t>(size));
                if (!bytes.empty())
                    input.read(
                        reinterpret_cast<char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
                if (!input && !bytes.empty())
                {
                    postReadFailure(
                        pending,
                        EAsyncFileError::READ_FAILED,
                        lastSystemError());
                    return;
                }
                postFallbackRead(pending, std::move(bytes));
#endif
            }

            void prepareWrite(const std::shared_ptr<PendingWrite>& pending)
                noexcept
            {
                ActiveCounterGuard running{blocking_io_running_};
                if (pending->state->cancelled.load(std::memory_order_acquire) ||
                    !accepting_.load(std::memory_order_acquire))
                {
                    postStopped(pending);
                    return;
                }

#if defined(ASIO_HAS_FILE)
                std::error_code open_error;
                const auto native = openWrite(pending->path, open_error);
                if (native == kInvalidFile)
                {
                    postWriteFailure(
                        pending,
                        EAsyncFileError::OPEN_FAILED,
                        open_error);
                    return;
                }
                auto self = shared_from_this();
                asio::post(
                    coordinator_,
                    [self, pending, native]() mutable noexcept
                    {
                        self->beginNativeWrite(pending, native);
                    });
#else
                std::ofstream output(
                    pending->path,
                    std::ios::binary | std::ios::trunc);
                if (!output)
                {
                    postWriteFailure(
                        pending,
                        EAsyncFileError::OPEN_FAILED,
                        lastSystemError());
                    return;
                }
                if (!pending->bytes.empty())
                    output.write(
                        reinterpret_cast<const char*>(pending->bytes.data()),
                        static_cast<std::streamsize>(pending->bytes.size()));
                output.flush();
                if (!output)
                {
                    postWriteFailure(
                        pending,
                        EAsyncFileError::WRITE_FAILED,
                        lastSystemError());
                    return;
                }
                postFallbackWrite(pending, pending->bytes.size());
#endif
            }

#if defined(ASIO_HAS_FILE)
            [[nodiscard]] NativeFile openRead(
                const std::filesystem::path& path,
                std::uint64_t& size,
                std::error_code& error) noexcept
            {
#if defined(_WIN32)
                const auto file = ::CreateFileW(
                    path.c_str(),
                    GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED |
                        FILE_FLAG_SEQUENTIAL_SCAN,
                    nullptr);
                if (file == kInvalidFile)
                {
                    error = lastSystemError();
                    return kInvalidFile;
                }
                LARGE_INTEGER value{};
                if (!::GetFileSizeEx(file, &value) || value.QuadPart < 0)
                {
                    error = lastSystemError();
                    closeNative(file);
                    return kInvalidFile;
                }
                size = static_cast<std::uint64_t>(value.QuadPart);
                return file;
#else
                const auto file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
                if (file == kInvalidFile)
                {
                    error = lastSystemError();
                    return kInvalidFile;
                }
                struct stat info{};
                if (::fstat(file, &info) != 0 || info.st_size < 0)
                {
                    error = lastSystemError();
                    closeNative(file);
                    return kInvalidFile;
                }
                size = static_cast<std::uint64_t>(info.st_size);
                return file;
#endif
            }

            [[nodiscard]] NativeFile openWrite(
                const std::filesystem::path& path,
                std::error_code& error) noexcept
            {
#if defined(_WIN32)
                const auto file = ::CreateFileW(
                    path.c_str(),
                    GENERIC_WRITE,
                    FILE_SHARE_READ,
                    nullptr,
                    CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                    nullptr);
                if (file == kInvalidFile)
                    error = lastSystemError();
                return file;
#else
                const auto file = ::open(
                    path.c_str(),
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                    0666);
                if (file == kInvalidFile)
                    error = lastSystemError();
                return file;
#endif
            }

            void beginNativeRead(
                const std::shared_ptr<PendingRead>& pending,
                NativeFile native,
                std::uint64_t size) noexcept
            {
                if (pending->state->cancelled.load(std::memory_order_acquire) ||
                    !accepting_.load(std::memory_order_acquire))
                {
                    closeNative(native);
                    finishPendingStopped(pending);
                    return;
                }
                auto request = std::make_unique<ReadRequest>(
                    coordinator_,
                    pending);
                native_state_allocations_.fetch_add(
                    1u,
                    std::memory_order_relaxed);
                std::error_code assign_error;
                request->file.assign(native, assign_error);
                if (assign_error)
                {
                    closeNative(native);
                    postReadFailure(
                        pending,
                        EAsyncFileError::OPEN_FAILED,
                        assign_error);
                    return;
                }
                request->bytes.resize(static_cast<std::size_t>(size));
                const auto id = pending->id;
                auto* request_ptr = request.get();
                requests_.emplace(id, std::move(request));
                active_native_requests_.fetch_add(
                    1u,
                    std::memory_order_relaxed);
                if (request_ptr->bytes.empty())
                {
                    finishNativeRead(id, {}, 0u);
                    return;
                }
                auto self = shared_from_this();
                asio::async_read_at(
                    request_ptr->file,
                    0u,
                    asio::buffer(request_ptr->bytes),
                    [self, id](
                        const std::error_code& error,
                        std::size_t transferred) noexcept
                    {
                        self->finishNativeRead(id, error, transferred);
                    });
            }

            void beginNativeWrite(
                const std::shared_ptr<PendingWrite>& pending,
                NativeFile native) noexcept
            {
                if (pending->state->cancelled.load(std::memory_order_acquire) ||
                    !accepting_.load(std::memory_order_acquire))
                {
                    closeNative(native);
                    finishPendingStopped(pending);
                    return;
                }
                auto request = std::make_unique<WriteRequest>(
                    coordinator_,
                    pending);
                native_state_allocations_.fetch_add(
                    1u,
                    std::memory_order_relaxed);
                std::error_code assign_error;
                request->file.assign(native, assign_error);
                if (assign_error)
                {
                    closeNative(native);
                    postWriteFailure(
                        pending,
                        EAsyncFileError::OPEN_FAILED,
                        assign_error);
                    return;
                }
                const auto id = pending->id;
                auto* request_ptr = request.get();
                requests_.emplace(id, std::move(request));
                active_native_requests_.fetch_add(
                    1u,
                    std::memory_order_relaxed);
                if (pending->bytes.empty())
                {
                    finishNativeWrite(id, {}, 0u);
                    return;
                }
                auto self = shared_from_this();
                asio::async_write_at(
                    request_ptr->file,
                    0u,
                    asio::buffer(pending->bytes),
                    [self, id](
                        const std::error_code& error,
                        std::size_t transferred) noexcept
                    {
                        self->finishNativeWrite(id, error, transferred);
                    });
            }

            void finishNativeRead(
                std::uint64_t id,
                const std::error_code& error,
                std::size_t transferred) noexcept
            {
                const auto found = requests_.find(id);
                if (found == requests_.end())
                    return;
                auto request = std::unique_ptr<ReadRequest>(
                    static_cast<ReadRequest*>(found->second.release()));
                requests_.erase(found);
                active_native_requests_.fetch_sub(
                    1u,
                    std::memory_order_relaxed);
                auto pending = std::move(request->pending);
                pending->state->id.store(0u, std::memory_order_release);
                pending->work_guard.reset();
                const auto stopped =
                    pending->state->cancelled.load(std::memory_order_acquire) ||
                    error == asio::error::operation_aborted;
                if (stopped)
                {
                    pending->completion(AsyncFileBuffer{}, true);
                    return;
                }
                if (error || transferred != request->bytes.size())
                {
                    pending->completion(
                        lux::cxx::unexpected(failure(
                            EAsyncFileError::READ_FAILED,
                            error)),
                        false);
                    return;
                }
                pending->completion(std::move(request->bytes), false);
            }

            void finishNativeWrite(
                std::uint64_t id,
                const std::error_code& error,
                std::size_t transferred) noexcept
            {
                const auto found = requests_.find(id);
                if (found == requests_.end())
                    return;
                auto request = std::unique_ptr<WriteRequest>(
                    static_cast<WriteRequest*>(found->second.release()));
                requests_.erase(found);
                active_native_requests_.fetch_sub(
                    1u,
                    std::memory_order_relaxed);
                auto pending = std::move(request->pending);
                pending->state->id.store(0u, std::memory_order_release);
                pending->work_guard.reset();
                const auto stopped =
                    pending->state->cancelled.load(std::memory_order_acquire) ||
                    error == asio::error::operation_aborted;
                if (stopped)
                {
                    pending->completion(std::size_t{0u}, true);
                    return;
                }
                if (error || transferred != pending->bytes.size())
                {
                    pending->completion(
                        lux::cxx::unexpected(failure(
                            EAsyncFileError::WRITE_FAILED,
                            error)),
                        false);
                    return;
                }
                pending->completion(transferred, false);
            }

            void cancelOnCoordinator(std::uint64_t id) noexcept
            {
                const auto found = requests_.find(id);
                if (found != requests_.end())
                    found->second->cancel();
            }
#else
            void cancelOnCoordinator(std::uint64_t) noexcept {}
#endif

            void postReadFailure(
                const std::shared_ptr<PendingRead>& pending,
                EAsyncFileError error,
                std::error_code system_error) noexcept
            {
                auto self = shared_from_this();
                asio::post(
                    coordinator_,
                    [self, pending, error, system_error]() noexcept
                    {
                        pending->state->id.store(
                            0u,
                            std::memory_order_release);
                        pending->work_guard.reset();
                        if (pending->state->cancelled.load(
                                std::memory_order_acquire))
                        {
                            pending->completion(AsyncFileBuffer{}, true);
                            return;
                        }
                        pending->completion(
                            lux::cxx::unexpected(failure(
                                error,
                                system_error)),
                            false);
                    });
            }

            void postWriteFailure(
                const std::shared_ptr<PendingWrite>& pending,
                EAsyncFileError error,
                std::error_code system_error) noexcept
            {
                auto self = shared_from_this();
                asio::post(
                    coordinator_,
                    [self, pending, error, system_error]() noexcept
                    {
                        pending->state->id.store(
                            0u,
                            std::memory_order_release);
                        pending->work_guard.reset();
                        if (pending->state->cancelled.load(
                                std::memory_order_acquire))
                        {
                            pending->completion(std::size_t{0u}, true);
                            return;
                        }
                        pending->completion(
                            lux::cxx::unexpected(failure(
                                error,
                                system_error)),
                            false);
                    });
            }

            void postFallbackRead(
                const std::shared_ptr<PendingRead>& pending,
                AsyncFileBuffer bytes) noexcept
            {
                auto self = shared_from_this();
                asio::post(
                    coordinator_,
                    [self, pending, bytes = std::move(bytes)]() mutable noexcept
                    {
                        pending->state->id.store(
                            0u,
                            std::memory_order_release);
                        pending->work_guard.reset();
                        const auto stopped = pending->state->cancelled.load(
                            std::memory_order_acquire);
                        if (stopped)
                            pending->completion(AsyncFileBuffer{}, true);
                        else
                            pending->completion(std::move(bytes), false);
                    });
            }

            void postFallbackWrite(
                const std::shared_ptr<PendingWrite>& pending,
                std::size_t bytes) noexcept
            {
                auto self = shared_from_this();
                asio::post(
                    coordinator_,
                    [self, pending, bytes]() noexcept
                    {
                        pending->state->id.store(
                            0u,
                            std::memory_order_release);
                        pending->work_guard.reset();
                        const auto stopped = pending->state->cancelled.load(
                            std::memory_order_acquire);
                        if (stopped)
                            pending->completion(std::size_t{0u}, true);
                        else
                            pending->completion(bytes, false);
                    });
            }

            asio::io_context& coordinator_;
            ::experimental::execution::static_thread_pool& blocking_io_;
            std::atomic<bool> accepting_{true};
            std::atomic<std::uint64_t> next_id_{1u};
            std::atomic<std::uint64_t> request_state_allocations_{0u};
            std::atomic<std::uint64_t> pending_state_allocations_{0u};
            std::atomic<std::uint64_t> native_state_allocations_{0u};
            std::atomic<std::size_t> active_pending_operations_{0u};
            std::atomic<std::size_t> blocking_io_running_{0u};
#if defined(ASIO_HAS_FILE)
            std::atomic<std::size_t> active_native_requests_{0u};
            std::unordered_map<
                std::uint64_t,
                std::unique_ptr<RequestBase>> requests_;
#endif
        };
    }

    std::shared_ptr<AsyncFileControl> makeAsyncFileControl(
        asio::io_context& coordinator,
        ::experimental::execution::static_thread_pool& blocking_io)
    {
        return std::make_shared<AsyncFileControlImpl>(
            coordinator,
            blocking_io);
    }
}
