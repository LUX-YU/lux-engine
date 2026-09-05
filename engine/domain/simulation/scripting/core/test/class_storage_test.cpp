#include <lux/engine/simulation/scripting/detail/BoundedClassStorage.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

int main()
{
    using namespace lux::simulation::script::detail;
    constexpr std::array plans{
        StorageClassPlan{64U, 64U, 4096U, 2U},
        StorageClassPlan{256U, 64U, 4096U, 2U},
        StorageClassPlan{32U, 16U, 4096U, 0U}
    };
    assert(!BoundedClassStorage::create(plans, 16384U, 256U)); // Metadata also consumes the budget.
    auto created = BoundedClassStorage::create(plans, 65536U, 256U);
    assert(created);
    auto storage = std::move(*created);
    const auto small = storage.select(48U, 32U);
    const auto big = storage.select(200U, 64U);
    const auto tiny = storage.select(24U, 8U);
    assert(small && big && tiny);
    assert(!storage.select(257U, 64U));
    assert(!storage.select(16U, 128U));
    assert(!storage.acquire(tiny, 24U));
    std::vector<BoundedClassStorage::Allocation> allocations;
    for (unsigned index{}; index < 128U; ++index)
    {
        const auto value = storage.acquire(small, 48U);
        assert(value);
        assert(reinterpret_cast<std::uintptr_t>(value->data) % 64U == 0U);
        std::memset(value->data, static_cast<int>(index), 48U);
        allocations.push_back(*value);
    }
    assert(!storage.acquire(small, 48U));
    assert(!storage.reclassifyEmptyPage(allocations.front().page, tiny));
    for (unsigned index{}; index < allocations.size(); index += 2U)
        assert(storage.release(allocations[index]));
    std::vector<BoundedClassStorage::Allocation> reused;
    for (unsigned index{}; index < 64U; ++index)
    {
        const auto value = storage.acquire(small, 48U);
        assert(value);
        reused.push_back(*value);
    }
    for (unsigned index{}; index < allocations.size(); index += 2U)
        assert(!storage.release(allocations[index]));
    for (auto iterator = reused.rbegin(); iterator != reused.rend(); ++iterator)
        assert(storage.release(*iterator));
    for (std::size_t index = allocations.size(); index != 0U; index -= 2U)
    {
        const auto& value = allocations[index - 1U];
        assert(*static_cast<const unsigned char*>(value.data) == index - 1U);
        assert(storage.release(value));
    }
    assert(storage.stats().active_allocations == 0U && storage.stats().live_bytes == 0U);
    assert(storage.stats().acquire_steps >= 3U * 192U && storage.stats().acquire_steps <= 6U * 192U);
    assert(storage.stats().release_steps >= 3U * 192U && storage.stats().release_steps <= 6U * 192U);
    const auto stale = allocations.front();
    assert(storage.reclassifyEmptyPage(stale.page, tiny));
    const auto replacement = storage.acquire(tiny, 24U);
    assert(replacement && !storage.release(stale));
    assert(storage.release(*replacement));
    const auto large = storage.acquire(big, 200U);
    assert(large && storage.release(*large));

    auto moved = std::move(storage);
    const auto after_move = moved.acquire(big, 200U);
    assert(after_move && moved.release(*after_move));
    assert(!storage.acquire(big, 200U));
    assert(!BoundedClassStorage::create(plans, 65536U, (std::numeric_limits<std::uint32_t>::max)()));
    auto exhausted = BoundedClassStorage::create(plans, 65536U, 256U, 1U);
    assert(exhausted);
    const auto layout = exhausted->select(48U, 32U);
    const auto last = exhausted->acquire(layout, 48U);
    assert(last && exhausted->release(*last));
    assert(!exhausted->acquire(layout, 48U));
    assert(exhausted->reclassifyEmptyPage(last->page, layout));
    assert(!exhausted->acquire(layout, 48U));
    return 0;
}
