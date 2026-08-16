#include <lux/engine/log/Log.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/execution/testing/AsyncCloseTestDriver.hpp>
#include <lux/engine/runtime/logging/LogRouter.hpp>
#include <lux/engine/runtime/logging/testing/LogRouterCloseTestDriver.hpp>

#include <cstdio>

int main()
{
    lux::exec::AsyncRuntimeBuilder builder;
    auto plan = std::move(builder).compile();
    if (!plan)
        return 1;

    lux::exec::AsyncRuntime runtime(
        std::move(*plan),
        lux::exec::AsyncRuntimeConfig{
            .blocking_io_threads = 1,
            .background_cpu_concurrency = 1
        }
    );
    lux::logging::LogRouter router(
        runtime,
        lux::logging::LogRouterConfig{
            .queue_capacity = 128,
            .batch_size = 32
        }
    );
    router.install();
    for (int index = 0; index < 16; ++index)
        lux::log::info("logging-test", "record {}", index);

    const auto statistics =
        lux::logging::testing::closeLogRouter(router, runtime);
    const auto close_report = lux::exec::testing::closeRuntime(runtime);
    const bool valid = statistics.accepted == 16u &&
        statistics.written == 16u && statistics.dropped == 0u &&
        close_report.clean();
    std::printf("LogRouter: accepted=%llu written=%llu dropped=%llu\n",
        static_cast<unsigned long long>(statistics.accepted),
        static_cast<unsigned long long>(statistics.written),
        static_cast<unsigned long long>(statistics.dropped));
    return valid ? 0 : 1;
}
