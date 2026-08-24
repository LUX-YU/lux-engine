// AsyncFileService acceptance test: owned buffers, structured failures and
// coordinator-independent completion (CPU only, no renderer).

#include <lux/engine/runtime/execution/AsyncFileService.hpp>
#include <lux/engine/runtime/execution/testing/AsyncCloseTestDriver.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>

#include <stdexec/execution.hpp>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <tuple>

namespace ex = stdexec;

int main()
{
    int failures = 0;
    auto check = [&](bool condition, const char* message)
    {
        if (condition)
            std::printf("[ ok ] %s\n", message);
        else
        {
            std::printf("[FAIL] %s\n", message);
            ++failures;
        }
    };

    std::error_code path_error;
    auto path = std::filesystem::temp_directory_path(path_error);
    check(!path_error, "temporary directory is available");
    path /= "lux_async_file_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin";

    lux::exec::AsyncRuntimeBuilder builder;
    auto plan = std::move(builder).compile();
    if (!plan || path_error)
        return 1;
    lux::exec::AsyncRuntime runtime(
        std::move(*plan),
        lux::exec::AsyncRuntimeConfig{
            .blocking_io_threads = 1u,
            .background_cpu_concurrency = 1u});
    const auto files = runtime.fileService().client();
    check(static_cast<bool>(files), "runtime exposes an AsyncFileClient");
#if defined(_WIN32)
    check(files.nativeAsyncAvailable(), "Windows file data uses Asio/IOCP");
#endif

    lux::exec::AsyncFileBuffer source{
        std::byte{0x00},
        std::byte{0x11},
        std::byte{0x7f},
        std::byte{0x80},
        std::byte{0xff}};
    auto write_terminal = ex::sync_wait(
        lux::exec::writeFile(files, path, source));
    check(write_terminal.has_value(), "write sender completes with a value");
    if (write_terminal)
    {
        const auto& result = std::get<0>(*write_terminal);
        check(result.has_value(), "write succeeds");
        if (result)
            check(*result == source.size(), "write reports the owned byte count");
    }

    auto read_terminal = ex::sync_wait(lux::exec::readFile(files, path));
    check(read_terminal.has_value(), "read sender completes with a value");
    if (read_terminal)
    {
        const auto& result = std::get<0>(*read_terminal);
        check(result.has_value(), "read succeeds");
        if (result)
            check(*result == source, "read returns an identical owned buffer");
    }

    auto limited_terminal = ex::sync_wait(lux::exec::readFile(
        files,
        path,
        lux::exec::AsyncFileReadOptions{.max_bytes = source.size() - 1u}));
    check(limited_terminal.has_value(), "oversize read terminates normally");
    if (limited_terminal)
    {
        const auto& result = std::get<0>(*limited_terminal);
        check(!result, "oversize read returns a structured failure");
        if (!result)
        {
            check(
                result.error().error ==
                    lux::exec::EAsyncFileError::FILE_TOO_LARGE,
                "oversize read reports FILE_TOO_LARGE");
        }
    }

    const auto file_stats = files.statistics();
    check(
        file_stats.request_state_allocations == 3u &&
            file_stats.pending_state_allocations == 3u &&
            file_stats.active_pending_operations == 0u &&
            file_stats.active_native_requests == 0u,
        "file service counts request/pending allocations and retires native state");
    if (files.nativeAsyncAvailable())
    {
        check(
            file_stats.native_state_allocations == 2u,
            "native file state allocations are observable without pooling guesses");
    }

    const auto close_report = lux::exec::testing::closeRuntime(runtime);
    check(
        close_report.status == lux::exec::EAsyncCloseStatus::CLOSED,
        "runtime closes cleanly after file IO");

    auto rejected_terminal = ex::sync_wait(lux::exec::readFile(files, path));
    check(rejected_terminal.has_value(), "post-close read has a terminal value");
    if (rejected_terminal)
    {
        const auto& result = std::get<0>(*rejected_terminal);
        check(!result, "post-close read is rejected");
        if (!result)
        {
            check(
                result.error().error == lux::exec::EAsyncFileError::STOPPING,
                "post-close read reports STOPPING");
        }
    }

    std::error_code remove_error;
    (void)std::filesystem::remove(path, remove_error);
    check(!remove_error, "temporary file is removed");

    std::printf(
        failures == 0 ? "\nALL CHECKS PASSED\n" : "\n%d CHECK(S) FAILED\n",
        failures);
    return failures == 0 ? 0 : 1;
}
