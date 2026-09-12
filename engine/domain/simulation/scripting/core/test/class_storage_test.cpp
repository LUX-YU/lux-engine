#include <lux/engine/simulation/scripting/ScriptRuntime.hpp>
#include <lux/engine/simulation/scripting/detail/BoundedClassStorage.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <limits>
#include <vector>

static void testOwnedBytes()
{
    using lux::simulation::script::ScriptOwnedBytes;
    ScriptOwnedBytes bytes;
    assert(bytes.resize(8U));
    for (std::size_t i{}; i < 8U; ++i) bytes.data()[i] = static_cast<std::byte>(i + 1U);
    auto* inline_address = bytes.data();
    assert(bytes.resize(16U) && bytes.data() == inline_address);
    for (std::size_t i{}; i < 8U; ++i) assert(bytes.data()[i] == static_cast<std::byte>(i + 1U));
    assert(bytes.resize(64U, 32U) && bytes.data() != inline_address);
    for (std::size_t i{}; i < 8U; ++i) assert(bytes.data()[i] == static_cast<std::byte>(i + 1U));
    assert(bytes.resize(4U) && bytes.data() == inline_address);
    for (std::size_t i{}; i < 4U; ++i) assert(bytes.data()[i] == static_cast<std::byte>(i + 1U));
    assert(!bytes.resize(4U, 3U) && bytes.size() == 4U);
    ScriptOwnedBytes moved{std::move(bytes)};
    assert(bytes.empty() && moved.size() == 4U);
    for (std::size_t i{}; i < 4U; ++i) assert(moved.data()[i] == static_cast<std::byte>(i + 1U));
    auto& self = moved;
    moved = std::move(self);
    assert(moved.size() == 4U && moved.resize(0U) && moved.empty());
    assert(moved.resize(64U, 32U));
    moved.data()[0] = std::byte{17};
    auto* spill = moved.data();
    bytes = std::move(moved);
    assert(moved.empty() && bytes.data() == spill && bytes.data()[0] == std::byte{17});
    assert(bytes.resize(0U) && bytes.empty());
    std::puts("OWNED_BYTES inline-grow spill inline-shrink invalid-align move self-move empty PASS");
}

int main()
{
    testOwnedBytes();
    using namespace lux::simulation::script::detail;
    constexpr std::array plans{
        StorageClassPlan{64U, 64U, 4096U, 2U},
        StorageClassPlan{256U, 64U, 4096U, 2U},
        StorageClassPlan{32U, 16U, 4096U, 0U}
    };
    assert(!BoundedClassStorage::create(plans, 16384U, 256U)); // Metadata also consumes the budget.
    auto created = BoundedClassStorage::create(plans, 65536U, 256U, UINT64_MAX, true);
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
    const auto replacement = storage.acquire(small, 48U);
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
    assert(!exhausted->acquire(layout, 48U));

    constexpr std::array large_pages{StorageClassPlan{1024U, 64U, 65536U, 4U}};
    auto large_only = BoundedClassStorage::create(large_pages, 1024U * 1024U, 512U);
    constexpr std::array mixed_pages{
        StorageClassPlan{1024U, 64U, 65536U, 4U}, StorageClassPlan{1U, 1U, 64U, 1U}};
    auto mixed = BoundedClassStorage::create(mixed_pages, 1024U * 1024U, 512U);
    assert(large_only && mixed);
    assert(large_only->stats().reserved_slots == 256U);
    assert(mixed->stats().reserved_slots == 320U);
    assert(mixed->stats().metadata_bytes - large_only->stats().metadata_bytes < 4096U);
    assert(mixed->stats().arena_bytes - large_only->stats().arena_bytes == 64U);
    for (const bool observe : {false, true})
    {
        auto checked = BoundedClassStorage::create(plans, 65536U, 2U, 3U, observe);
        auto foreign = BoundedClassStorage::create(plans, 65536U, 2U, 3U, observe);
        assert(checked && foreign);
        const auto handle = checked->select(48U, 32U);
        const auto a = checked->acquire(handle, 48U);
        assert(a);
        const auto ticket = checked->ticket(*a);
        assert(!foreign->release(ticket));
        auto wrong = *a;
        wrong.data = static_cast<std::byte*>(wrong.data) + 1;
        assert(!checked->release(wrong));
        wrong = *a;
        ++wrong.size;
        assert(!checked->release(wrong));
        wrong = *a;
        wrong.page = UINT32_MAX;
        assert(!checked->release(wrong));
        wrong = *a;
        wrong.slot = UINT32_MAX;
        assert(!checked->release(wrong));
        auto wrong_ticket = ticket;
        wrong_ticket.page = UINT32_MAX;
        assert(!checked->release(wrong_ticket));
        wrong_ticket = ticket;
        wrong_ticket.slot = UINT32_MAX;
        assert(!checked->release(wrong_ticket));
        assert(checked->stats().active_allocations == 1U);
        assert(checked->release(ticket));
        assert(!checked->release(ticket) && !checked->release(*a));
        const auto b = checked->acquire(handle, 48U);
        assert(b && !checked->release(ticket));
        assert(checked->release(checked->ticket(*b)));
        const auto last = checked->acquire(handle, 48U);
        assert(last && checked->release(*last));
        assert(!checked->release(*last) && !checked->release(checked->ticket(*last)));
        assert(!checked->acquire(handle, 48U));
        const auto stats = checked->stats();
        assert(stats.active_allocations == 0U && stats.capacity_failures == 1U);
        assert(stats.observation_collected == observe);
        assert(observe ? stats.allocation_high_water == 1U : stats.acquire_steps == 0U);
        std::printf("STORAGE_TICKET,observe=%d,owner=1,generation=1,allocation_checks=1,exhaustion=1,ticket=%zu\n",
            observe, sizeof(BoundedClassStorage::Ticket));
    }
    return 0;
}
