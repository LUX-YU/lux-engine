#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>

#ifdef _MSC_VER
#include <malloc.h>
#endif

struct ProbeState final
{
    std::size_t size_only{};
    std::size_t aligned{};
    std::size_t requested_bytes{};
    std::uintptr_t before{};
    std::uintptr_t after{};
};

struct Probe final
{
    struct promise_type final
    {
        template <class... Args>
        static void* operator new(std::size_t size, ProbeState& state, Args&&...) noexcept
        {
            ++state.size_only;
            state.requested_bytes = size;
#ifdef _MSC_VER
            return _aligned_malloc(size, alignof(std::max_align_t));
#else
            constexpr auto alignment = alignof(std::max_align_t);
            return std::aligned_alloc(alignment, (size + alignment - 1U) / alignment * alignment);
#endif
        }
        template <class... Args>
        static void* operator new(std::size_t size, std::align_val_t, ProbeState& state, Args&&... args) noexcept
        {
            ++state.aligned;
            return operator new(size, state, args...);
        }
        static void operator delete(void* value, std::size_t) noexcept
        {
#ifdef _MSC_VER
            _aligned_free(value);
#else
            std::free(value);
#endif
        }
        static Probe get_return_object_on_allocation_failure() noexcept { return {}; }
        Probe get_return_object() noexcept { return {std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() const noexcept { return {}; }
        std::suspend_always final_suspend() const noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
    std::coroutine_handle<promise_type> handle;
};

Probe exercise(ProbeState& state) noexcept
{
    alignas(64) volatile std::uint64_t values[8]{};
    values[0] = 42U;
    state.before = reinterpret_cast<std::uintptr_t>(&values[0]);
    co_await std::suspend_always{};
    state.after = reinterpret_cast<std::uintptr_t>(&values[0]);
    if (values[0] != 42U)
        std::terminate();
}

int main()
{
    ProbeState state;
    auto probe = exercise(state);
    if (!probe.handle)
        return 1;
    probe.handle.resume();
    probe.handle.resume();
    std::printf("size_only=%zu aligned_overload=%zu bytes=%zu local_mod64=%zu stable_address=%d\n",
        state.size_only, state.aligned, state.requested_bytes, static_cast<std::size_t>(state.after % 64U),
        state.before == state.after ? 1 : 0);
    probe.handle.destroy();
    return state.before == state.after && state.after % 64U == 0U ? 0 : 1;
}
