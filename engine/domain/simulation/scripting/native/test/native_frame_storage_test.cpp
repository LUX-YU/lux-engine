#include <lux/engine/simulation/scripting/native/NativeFrameStorage.hpp>
#include <array>
#include <cassert>
#include <cstdio>
#include <random>
using namespace lux::simulation::script::detail;

int main()
{
    constexpr std::array plans{StorageClassPlan{1024U, 64U, 8192U, 1U}};
    for (const bool observe : {false, true})
    {
        auto made = NativeFrameStorage::create(plans, 12288U, 8U, 8U, observe);
        assert(made);
        auto& storage = *made;
        const auto domain = storage.domain(1024U, 64U);
        assert(domain && !storage.domain(1024U, 32U));
        const auto small = storage.prepare(domain, 128U, 64U);
        const auto large = storage.prepare(domain, 1024U, 64U);
        const auto middle = storage.prepare(domain, 192U, 64U);
        assert(small && large && middle && !storage.prepare(domain, 32U, 128U));
        std::array<NativeFrameStorage::Lease, 8U> leases;
        for (auto& lease : leases) assert(storage.acquire(small, lease));
        assert(storage.stats().arena_bytes == 8192U && storage.stats().reserved_slots == 8U);
        assert(storage.stats().metadata_bytes + storage.stats().arena_bytes <= 12288U);
        if (observe)
        {
            assert(storage.stats().live_bytes == 1024U && storage.stats().occupied_bytes == 1024U);
            assert(storage.stats().active_region_bytes == 1024U);
        }
        NativeFrameStorage::Lease excess;
        assert(!storage.acquire(large, excess)); // Includes a simulated destroy callback before returning its slot.
        assert(!storage.releaseLayout(small));
        const auto stale = leases[0].ticket();
        for (std::size_t i{}; i < leases.size(); ++i)
        {
            assert(storage.release(leases[i]));
            assert(storage.acquire(large, leases[i]));
        }
        assert(!storage.release(leases[0], stale));
        assert(!storage.acquire(large, excess));
        if (observe) assert(storage.stats().active_region_bytes == 8192U);
        for (auto& lease : leases) assert(storage.release(lease));

        std::mt19937 random{1592598566U};
        for (unsigned iteration{}; iteration < 4096U; ++iteration)
        {
            auto& lease = leases[random() % leases.size()];
            if (lease) assert(storage.release(lease));
            const std::array choices{small, large, middle};
            assert(storage.acquire(choices[random() % choices.size()], lease));
            assert(reinterpret_cast<std::uintptr_t>(lease.data) % 64U == 0U);
        }
        for (auto& lease : leases) if (lease) assert(storage.release(lease));
        assert(storage.stats().active_allocations == 0U && storage.stats().active_region_bytes == 0U);
        assert(storage.releaseLayout(small) && storage.releaseLayout(large) && storage.releaseLayout(middle));
        for (unsigned i{}; i < 32U; ++i)
        {
            const auto temporary = storage.prepare(domain, 32U + i, 8U);
            assert(temporary && storage.acquire(temporary, excess));
            assert(storage.release(excess) && storage.releaseLayout(temporary));
        }
        std::printf("NATIVE_REGIONS,observe=%d,reserved=%zu,metadata=%zu,all_large=8,excess_rejected=1,"
            "mixed=4096,small_regions=1,layout_reuse=32,lease=%zu,ticket=%zu\n", observe,
            storage.stats().arena_bytes, storage.stats().metadata_bytes,
            sizeof(NativeFrameStorage::Lease), sizeof(NativeFrameStorage::Ticket));
    }
    constexpr std::array isolated{
        StorageClassPlan{1024U, 64U, 2048U, 1U}, StorageClassPlan{1024U, 32U, 1024U, 1U}
    };
    auto storage = NativeFrameStorage::create(isolated, 8192U, 3U, 4U, true, 4U);
    auto foreign = NativeFrameStorage::create(isolated, 8192U, 3U, 4U);
    assert(storage && foreign);
    const auto first = storage->prepare(storage->domain(1024U, 64U), 1U, 1U);
    const auto second = storage->prepare(storage->domain(1024U, 32U), 1024U, 32U);
    NativeFrameStorage::Lease a, b, c, over;
    assert(storage->acquire(first, a) && storage->acquire(first, b));
    assert(!storage->acquire(first, over) && storage->acquire(second, c));
    assert(!foreign->release(a, a.ticket()));
    const auto old = a.ticket();
    assert(storage->release(a) && storage->acquire(first, a));
    assert(!storage->release(a, old));
    assert(storage->release(a) && storage->release(b) && storage->release(c));
    assert(!storage->acquire(first, a)); // Full-width generation exhaustion never wraps.
    assert(storage->releaseLayout(first) && storage->releaseLayout(second));
    assert(!NativeFrameStorage::create(plans, 8192U, 8U, 8U)); // Metadata must fit the original budget too.
    std::puts("NATIVE_REGION_IDENTITY,domain_isolation=1,tiny=1,owner=1,generation=1,metadata_budget=1 PASS");
}
