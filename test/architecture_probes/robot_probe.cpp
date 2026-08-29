#include <lux/engine/scene/LatestSpscExchange.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>

namespace
{
    struct RobotPresentationState final
    {
        std::uint64_t step{};
        double logical_seconds{};
    };
}

int main()
{
    using namespace std::chrono_literals;
    lux::scene::LatestSpscExchange<RobotPresentationState> exchange;
    std::atomic_bool done{};
    std::thread simulation([&]() {
        for (std::uint64_t step{1U}; step <= 4U; ++step)
        {
            std::this_thread::sleep_for(10ms);
            exchange.write() = {step, static_cast<double>(step) * 0.000'01};
            exchange.publish();
        }
        done.store(true, std::memory_order_release);
    });

    std::uint64_t observed{};
    std::uint64_t presentation_frames{};
    while (!done.load(std::memory_order_acquire) || observed != 4U)
    {
        if (exchange.acquireLatest())
            observed = exchange.read().step;
        ++presentation_frames;
        std::this_thread::yield();
    }
    simulation.join();
    assert(observed == 4U);
    assert(presentation_frames > observed);
    return 0;
}
