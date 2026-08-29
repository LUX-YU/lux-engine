#include <lux/engine/scene/LatestSpscExchange.hpp>

#include <array>
#include <cassert>
#include <cstdint>

namespace
{
    struct PixelPresentationState final
    {
        std::uint64_t revision{};
        std::uint32_t changed_chunks{};
    };
}

int main()
{
    constexpr std::uint64_t kSteps = 10'000U;
    lux::scene::LatestSpscExchange<PixelPresentationState> exchange;
    std::array<std::uint8_t, 1024U> cells{};
    for (std::uint64_t step{}; step < kSteps; ++step)
    {
        cells[step % cells.size()] ^= 1U;
        if ((step % 8U) == 0U)
        {
            exchange.write() = {step + 1U, 1U};
            exchange.publish();
        }
    }
    assert(exchange.acquireLatest());
    assert(exchange.read().revision <= kSteps);
    assert(exchange.read().changed_chunks == 1U);
    return 0;
}
