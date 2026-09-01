#include <lux/engine/scene/LatestSpscExchange.hpp>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>

namespace
{
    struct State final
    {
        std::uint64_t value{};
        std::vector<std::uint64_t> reused;
    };
}

int main()
{
    constexpr std::uint64_t kPublishes = 1'000'000U;
    lux::scene::LatestSpscExchange<State> exchange;
    for (auto& value : exchange.write().reused)
        static_cast<void>(value);
    exchange.write().reused.reserve(64U);

    std::atomic_bool producer_done{};
    std::thread producer([&]() {
        for (std::uint64_t value{1U}; value <= kPublishes; ++value)
        {
            auto& state = exchange.write();
            state.value = value;
            state.reused.clear();
            state.reused.push_back(value);
            exchange.publish();
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::uint64_t observed{};
    while (!producer_done.load(std::memory_order_acquire) || observed != kPublishes)
    {
        if (!exchange.acquireLatest())
        {
            std::this_thread::yield();
            continue;
        }
        const auto& state = exchange.read();
        assert(state.value >= observed);
        assert(state.reused.size() == 1U);
        assert(state.reused[0] == state.value);
        observed = state.value;
    }
    producer.join();
    assert(observed == kPublishes);
    return 0;
}
