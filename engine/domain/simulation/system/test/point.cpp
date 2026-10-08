#include <lux/engine/simulation/HookPoint.hpp>

#include <cassert>
#include <cstdio>
#include <type_traits>

using namespace lux::simulation;

namespace
{
    void callback(void*) noexcept {}

    template <class T>
    concept HasPrepare = requires(T& value) { value.prepare(1); };
} // namespace

static_assert(!std::is_default_constructible_v<THookPoint<void()>>);
static_assert(!std::is_default_constructible_v<THookPoint<void() noexcept>>);
static_assert(std::is_nothrow_constructible_v<THookPoint<void()>, std::size_t>);
static_assert(std::is_nothrow_constructible_v<THookPoint<void() noexcept>, std::size_t>);
static_assert(!std::is_copy_constructible_v<THookPoint<void()>>);
static_assert(!std::is_move_constructible_v<THookPoint<void()>>);
static_assert(!HasPrepare<THookPoint<void()>>);

int main()
{
    THookPoint<void()> zero{0};
    assert(zero.connect(nullptr, &callback).error == EEndpointMutationError::CAPACITY_EXCEEDED);
    THookPoint<void() noexcept> point{2};
    assert(point.connect(nullptr, nullptr).error == EEndpointMutationError::INVALID_CALLBACK);
    const auto first = point.connect(nullptr, &callback);
    const auto second = point.connect(nullptr, &callback);
    assert(first && second && point.handlerCount() == 2);
    assert(point.connect(nullptr, &callback).error == EEndpointMutationError::CAPACITY_EXCEEDED);
    assert(point.disconnect(first.token) == EEndpointMutationError::NONE);
    assert(point.disconnect(first.token) == EEndpointMutationError::INVALID_TOKEN);
    auto current = point.connect(nullptr, &callback);
    assert(current && current.token != first.token);
    for (int i = 0; i < 10000; ++i)
    {
        const auto stale = current.token;
        assert(point.disconnect(stale) == EEndpointMutationError::NONE);
        current = point.connect(nullptr, &callback);
        assert(current && current.token != stale);
        assert(point.disconnect(stale) == EEndpointMutationError::INVALID_TOKEN);
    }
    assert(point.disconnect(second.token) == EEndpointMutationError::NONE);
    assert(point.disconnect(current.token) == EEndpointMutationError::NONE);
    assert(point.handlerCount() == 0);
    std::puts("Actual public HookPoint: capacity, null callback, stale generation,10000 reuse PASS");
}
