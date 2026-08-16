#include <lux/engine/runtime/execution/MainThreadStateCache.hpp>

#include <cstdint>
#include <cstdio>

namespace
{
    int failures = 0;

    void check(bool condition, const char* description)
    {
        if (!condition)
        {
            std::printf("FAIL: %s\n", description);
            ++failures;
        }
    }
}

int main()
{
    lux::exec::MainThreadStateCache<
        std::uint32_t,
        std::uint32_t
    > cache;

    check(cache.state(1u) == lux::exec::CacheState::Pending,
        "missing keys read as pending");
    check(cache.tryGet(1u) == nullptr,
        "missing keys have no value");
    check(cache.markPending(1u),
        "the first request inserts a slot");
    check(!cache.markPending(1u),
        "duplicate requests do not insert another slot");

    cache.setReady(1u, 42u);
    check(cache.state(1u) == lux::exec::CacheState::Ready,
        "setReady commits the state");
    check(cache.tryGet(1u) != nullptr && *cache.tryGet(1u) == 42u,
        "setReady stores the value");

    cache.markPending(2u);
    cache.setFailed(2u);
    check(cache.state(2u) == lux::exec::CacheState::Failed,
        "setFailed commits the state");
    check(cache.tryGet(2u) == nullptr,
        "failed entries have no readable value");

    std::uint32_t ready_count = 0u;
    cache.forEachReady(
        [&](std::uint32_t key, std::uint32_t value)
        {
            check(key == 1u && value == 42u,
                "forEachReady exposes committed entries");
            ++ready_count;
        }
    );
    check(ready_count == 1u,
        "forEachReady skips pending and failed entries");

    cache.invalidate(1u);
    check(cache.tryGet(1u) == nullptr,
        "invalidate removes one entry");
    cache.clear();
    check(cache.state(2u) == lux::exec::CacheState::Pending,
        "clear removes all entries");

    return failures == 0 ? 0 : 1;
}
