#pragma once
/**
 * @file AsyncFileService.hpp
 * @brief Owned-buffer file IO senders backed by AsyncRuntime.
 *
 * Windows uses overlapped handles and the runtime's Asio/IOCP context. Linux
 * uses Asio's file service when io_uring is available. Platforms without a
 * native asynchronous regular-file service use the deliberately small
 * BlockingIoExecutor. Opening and metadata queries always stay off the
 * coordinator thread.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>

#include <stdexec/execution.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace lux::exec
{
    enum class EAsyncFileError : std::uint8_t
    {
        INVALID_PATH,
        OPEN_FAILED,
        SIZE_FAILED,
        FILE_TOO_LARGE,
        READ_FAILED,
        WRITE_FAILED,
        STOPPING
    };

    struct AsyncFileFailure final
    {
        EAsyncFileError error{EAsyncFileError::READ_FAILED};
        std::error_code system_error{};
    };

    struct AsyncFileReadOptions final
    {
        std::size_t max_bytes{1024u * 1024u * 1024u};
    };

    using AsyncFileBuffer = std::vector<std::byte>;
    template <typename T>
    using AsyncFileExp = lux::cxx::expected<T, AsyncFileFailure>;

    using AsyncFileReadResult = AsyncFileExp<AsyncFileBuffer>;
    using AsyncFileWriteResult = AsyncFileExp<std::size_t>;

    struct AsyncFileStatistics final
    {
        std::uint64_t request_state_allocations{0u};
        std::uint64_t pending_state_allocations{0u};
        std::uint64_t native_state_allocations{0u};
        std::size_t   active_pending_operations{0u};
        std::size_t   active_native_requests{0u};
        std::size_t   blocking_io_running{0u};
    };

    namespace detail
    {
        struct AsyncFileRequestState final
        {
            std::atomic<bool> cancelled{false};
            std::atomic<std::uint64_t> id{0u};
        };

        class AsyncFileControl
        {
        public:
            using ReadCompletion = lux::cxx::move_only_function<void(
                AsyncFileReadResult,
                bool)>;
            using WriteCompletion = lux::cxx::move_only_function<void(
                AsyncFileWriteResult,
                bool)>;

            virtual ~AsyncFileControl() = default;

            [[nodiscard]] virtual bool tryRead(
                std::filesystem::path path,
                AsyncFileReadOptions options,
                std::shared_ptr<AsyncFileRequestState> state,
                ReadCompletion completion) noexcept = 0;
            [[nodiscard]] virtual bool tryWrite(
                std::filesystem::path path,
                AsyncFileBuffer bytes,
                std::shared_ptr<AsyncFileRequestState> state,
                WriteCompletion completion) noexcept = 0;
            virtual void cancel(
                const std::shared_ptr<AsyncFileRequestState>& state) noexcept = 0;
            virtual void closeAdmissionAndCancel() noexcept = 0;
            [[nodiscard]] virtual bool nativeAsyncAvailable() const noexcept = 0;
            virtual void recordRequestStateAllocation() noexcept = 0;
            [[nodiscard]] virtual AsyncFileStatistics statistics()
                const noexcept = 0;
        };
    }

    class AsyncFileClient final
    {
    public:
        AsyncFileClient() noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return static_cast<bool>(control_);
        }

        [[nodiscard]] bool nativeAsyncAvailable() const noexcept
        {
            return control_ && control_->nativeAsyncAvailable();
        }

        [[nodiscard]] AsyncFileStatistics statistics() const noexcept
        {
            return control_ ? control_->statistics() : AsyncFileStatistics{};
        }

    private:
        friend class AsyncFileService;
        friend class AsyncFileReadSender;
        friend class AsyncFileWriteSender;

        explicit AsyncFileClient(
            std::shared_ptr<detail::AsyncFileControl> control) noexcept
            : control_(std::move(control))
        {}

        std::shared_ptr<detail::AsyncFileControl> control_;
    };

    class AsyncFileService final
    {
    public:
        AsyncFileService(const AsyncFileService&) = delete;
        AsyncFileService& operator=(const AsyncFileService&) = delete;
        AsyncFileService(AsyncFileService&&) = delete;
        AsyncFileService& operator=(AsyncFileService&&) = delete;

        [[nodiscard]] AsyncFileClient client() const noexcept
        {
            return AsyncFileClient{control_};
        }

    private:
        friend class AsyncRuntime;

        explicit AsyncFileService(
            std::shared_ptr<detail::AsyncFileControl> control) noexcept
            : control_(std::move(control))
        {}

        std::shared_ptr<detail::AsyncFileControl> control_;
    };

    class AsyncFileReadSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(AsyncFileReadResult),
            stdexec::set_stopped_t()>;

        AsyncFileReadSender(
            AsyncFileClient client,
            std::filesystem::path path,
            AsyncFileReadOptions options) noexcept
            : client_(std::move(client))
            , path_(std::move(path))
            , options_(options)
        {}

        template <class Receiver>
        struct State final
        {
            using operation_state_concept = stdexec::operation_state_t;
            using StopToken = stdexec::stop_token_of_t<
                stdexec::env_of_t<Receiver>>;

            struct Cancel final
            {
                std::shared_ptr<detail::AsyncFileControl> control;
                std::shared_ptr<detail::AsyncFileRequestState> request;

                void operator()() noexcept
                {
                    request->cancelled.store(true, std::memory_order_release);
                    if (control)
                        control->cancel(request);
                }
            };

            using StopCallback = stdexec::stop_callback_for_t<
                StopToken,
                Cancel>;

            State(
                AsyncFileClient client_value,
                std::filesystem::path path_value,
                AsyncFileReadOptions options_value,
                Receiver receiver_value)
                : client(std::move(client_value))
                , path(std::move(path_value))
                , options(options_value)
                , receiver(std::move(receiver_value))
                , request(std::make_shared<detail::AsyncFileRequestState>())
            {}

            void start() & noexcept
            {
                if (!client.control_)
                {
                    stdexec::set_value(
                        std::move(receiver),
                        lux::cxx::unexpected(AsyncFileFailure{
                            EAsyncFileError::STOPPING,
                            {}}));
                    return;
                }
                if (path.empty() || options.max_bytes == 0u)
                {
                    stdexec::set_value(
                        std::move(receiver),
                        lux::cxx::unexpected(AsyncFileFailure{
                            EAsyncFileError::INVALID_PATH,
                            {}}));
                    return;
                }

                stop_callback.emplace(
                    stdexec::get_stop_token(stdexec::get_env(receiver)),
                    Cancel{client.control_, request});
                const auto accepted = client.control_->tryRead(
                    std::move(path),
                    options,
                    request,
                    [this](AsyncFileReadResult result, bool stopped)
                        mutable noexcept
                    {
                        stop_callback.reset();
                        if (stopped)
                            stdexec::set_stopped(std::move(receiver));
                        else
                            stdexec::set_value(
                                std::move(receiver),
                                std::move(result));
                    });
                if (!accepted)
                {
                    stop_callback.reset();
                    stdexec::set_value(
                        std::move(receiver),
                        lux::cxx::unexpected(AsyncFileFailure{
                            EAsyncFileError::STOPPING,
                            {}}));
                }
            }

            AsyncFileClient client;
            std::filesystem::path path;
            AsyncFileReadOptions options;
            Receiver receiver;
            std::shared_ptr<detail::AsyncFileRequestState> request;
            std::optional<StopCallback> stop_callback;
        };

        template <class Receiver>
        [[nodiscard]] State<std::decay_t<Receiver>> connect(
            Receiver&& receiver) &&
        {
            if (client_.control_)
                client_.control_->recordRequestStateAllocation();
            return State<std::decay_t<Receiver>>{
                std::move(client_),
                std::move(path_),
                options_,
                std::forward<Receiver>(receiver)};
        }

    private:
        AsyncFileClient client_;
        std::filesystem::path path_;
        AsyncFileReadOptions options_{};
    };

    class AsyncFileWriteSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(AsyncFileWriteResult),
            stdexec::set_stopped_t()>;

        AsyncFileWriteSender(
            AsyncFileClient client,
            std::filesystem::path path,
            AsyncFileBuffer bytes) noexcept
            : client_(std::move(client))
            , path_(std::move(path))
            , bytes_(std::move(bytes))
        {}

        template <class Receiver>
        struct State final
        {
            using operation_state_concept = stdexec::operation_state_t;
            using StopToken = stdexec::stop_token_of_t<
                stdexec::env_of_t<Receiver>>;

            struct Cancel final
            {
                std::shared_ptr<detail::AsyncFileControl> control;
                std::shared_ptr<detail::AsyncFileRequestState> request;

                void operator()() noexcept
                {
                    request->cancelled.store(true, std::memory_order_release);
                    if (control)
                        control->cancel(request);
                }
            };

            using StopCallback = stdexec::stop_callback_for_t<
                StopToken,
                Cancel>;

            State(
                AsyncFileClient client_value,
                std::filesystem::path path_value,
                AsyncFileBuffer bytes_value,
                Receiver receiver_value)
                : client(std::move(client_value))
                , path(std::move(path_value))
                , bytes(std::move(bytes_value))
                , receiver(std::move(receiver_value))
                , request(std::make_shared<detail::AsyncFileRequestState>())
            {}

            void start() & noexcept
            {
                if (!client.control_)
                {
                    stdexec::set_value(
                        std::move(receiver),
                        lux::cxx::unexpected(AsyncFileFailure{
                            EAsyncFileError::STOPPING,
                            {}}));
                    return;
                }
                if (path.empty())
                {
                    stdexec::set_value(
                        std::move(receiver),
                        lux::cxx::unexpected(AsyncFileFailure{
                            EAsyncFileError::INVALID_PATH,
                            {}}));
                    return;
                }

                stop_callback.emplace(
                    stdexec::get_stop_token(stdexec::get_env(receiver)),
                    Cancel{client.control_, request});
                const auto accepted = client.control_->tryWrite(
                    std::move(path),
                    std::move(bytes),
                    request,
                    [this](AsyncFileWriteResult result, bool stopped)
                        mutable noexcept
                    {
                        stop_callback.reset();
                        if (stopped)
                            stdexec::set_stopped(std::move(receiver));
                        else
                            stdexec::set_value(
                                std::move(receiver),
                                std::move(result));
                    });
                if (!accepted)
                {
                    stop_callback.reset();
                    stdexec::set_value(
                        std::move(receiver),
                        lux::cxx::unexpected(AsyncFileFailure{
                            EAsyncFileError::STOPPING,
                            {}}));
                }
            }

            AsyncFileClient client;
            std::filesystem::path path;
            AsyncFileBuffer bytes;
            Receiver receiver;
            std::shared_ptr<detail::AsyncFileRequestState> request;
            std::optional<StopCallback> stop_callback;
        };

        template <class Receiver>
        [[nodiscard]] State<std::decay_t<Receiver>> connect(
            Receiver&& receiver) &&
        {
            if (client_.control_)
                client_.control_->recordRequestStateAllocation();
            return State<std::decay_t<Receiver>>{
                std::move(client_),
                std::move(path_),
                std::move(bytes_),
                std::forward<Receiver>(receiver)};
        }

    private:
        AsyncFileClient client_;
        std::filesystem::path path_;
        AsyncFileBuffer bytes_;
    };

    [[nodiscard]] inline AsyncFileReadSender readFile(
        AsyncFileClient client,
        std::filesystem::path path,
        AsyncFileReadOptions options = {}) noexcept
    {
        return AsyncFileReadSender{
            std::move(client),
            std::move(path),
            options};
    }

    [[nodiscard]] inline AsyncFileWriteSender writeFile(
        AsyncFileClient client,
        std::filesystem::path path,
        AsyncFileBuffer bytes) noexcept
    {
        return AsyncFileWriteSender{
            std::move(client),
            std::move(path),
            std::move(bytes)};
    }
}
