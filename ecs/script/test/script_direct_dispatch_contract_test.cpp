#include <lux/engine/ecs/script/detail/DirectDispatch.hpp>
#include <lux/engine/ecs/script/systems/ScriptEventRegistry.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <type_traits>
#include <vector>

namespace
{
    std::atomic<std::size_t> g_tracked_allocations{0};
    thread_local bool g_track_allocations = false;

    #if defined(_MSC_VER)
        #define LUX_NOINLINE __declspec(noinline)
    #else
        #define LUX_NOINLINE __attribute__((noinline))
    #endif

    LUX_NOINLINE int directNoop(lux_script_call_frame* frame) noexcept
    {
        auto* count = static_cast<std::uint64_t*>(frame->user_context);
        ++*count;
        return 0;
    }

    LUX_NOINLINE int directFailure(lux_script_call_frame*) noexcept
    {
        return 17;
    }

    LUX_NOINLINE lux::ecs::detail::DirectDispatchResult runRawBaseline(
        std::span<const lux_script_invoke_fn> functions,
        std::span<void* const> contexts,
        lux_script_call_frame& frame
    ) noexcept
    {
        for (std::size_t i = 0; i < functions.size(); ++i)
        {
            frame.user_context = contexts[i];
            const int result = functions[i](&frame);
            if (result != 0) [[unlikely]]
                return {i, result};
        }
        return {functions.size(), 0};
    }

    LUX_NOINLINE lux::ecs::detail::DirectDispatchResult runStructBaseline(
        std::span<const lux::ecs::BoundScriptCall> calls,
        lux_script_call_frame& frame
    ) noexcept
    {
        for (std::size_t i = 0; i < calls.size(); ++i)
        {
            frame.user_context = calls[i].context;
            const int result = calls[i].invoke(&frame);
            if (result != 0) [[unlikely]]
                return {i, result};
        }
        return {calls.size(), 0};
    }

    [[nodiscard]] std::uint64_t elapsedNanoseconds(auto&& operation)
    {
        const auto begin = std::chrono::steady_clock::now();
        operation();
        const auto end = std::chrono::steady_clock::now();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
                .count()
        );
    }

    [[nodiscard]] std::uint64_t median(
        std::array<std::uint64_t, 7> values
    )
    {
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    }

    struct ConcreteBehavior final : lux::ecs::ScriptBehavior
    {
        void onUpdate(float) noexcept {}
    };
}

void* operator new(std::size_t size)
{
    if (g_track_allocations)
        g_tracked_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

int main()
{
    static_assert(
        sizeof(lux::ecs::BoundScriptCall) == 2 * sizeof(void*)
    );
    static_assert(
        std::is_trivially_copyable_v<lux::ecs::BoundScriptCall>
    );
    static_assert(!std::is_polymorphic_v<lux::ecs::ScriptBehavior>);
    static_assert(!std::is_polymorphic_v<ConcreteBehavior>);

    const auto& update = lux::ecs::scriptEventRegistry().desc(
        lux::ecs::ScriptEventRegistry::kOnUpdate
    );
    if (update.abi_params.size() != 1
        || update.abi_params[0].kind != LUX_SCRIPT_VK_FLOAT
        || update.abi_params[0].size != sizeof(float))
        return 1;

    constexpr std::size_t kCallCount = 100'000;
    constexpr std::size_t kRepeats = 100;
    std::uint64_t call_counter = 0;
    std::vector<lux::ecs::BoundScriptCall> calls(
        kCallCount,
        lux::ecs::BoundScriptCall{directNoop, &call_counter}
    );
    std::vector<lux_script_invoke_fn> baseline_functions(
        kCallCount,
        directNoop
    );
    std::vector<void*> baseline_contexts(kCallCount, &call_counter);

    lux_script_call_frame frame{};
    g_tracked_allocations.store(0, std::memory_order_relaxed);
    g_track_allocations = true;
    const auto allocation_probe = lux::ecs::detail::dispatchBoundCalls(
        calls,
        frame
    );
    g_track_allocations = false;
    if (allocation_probe.failed_index != calls.size()
        || allocation_probe.result != 0
        || g_tracked_allocations.load(std::memory_order_relaxed) != 0)
        return 2;

    calls[kCallCount / 2].invoke = directFailure;
    const auto failure = lux::ecs::detail::dispatchBoundCalls(calls, frame);
    if (failure.failed_index != kCallCount / 2 || failure.result != 17)
        return 3;
    calls[kCallCount / 2].invoke = directNoop;

    std::array<std::uint64_t, 7> direct_samples{};
    std::array<std::uint64_t, 7> baseline_samples{};
    std::array<std::uint64_t, 7> struct_baseline_samples{};
    for (std::size_t sample = 0; sample < direct_samples.size(); ++sample)
    {
        baseline_samples[sample] = elapsedNanoseconds([&]
        {
            for (std::size_t repeat = 0; repeat < kRepeats; ++repeat)
            {
                const auto result = runRawBaseline(
                    baseline_functions,
                    baseline_contexts,
                    frame
                );
                if (result.result != 0)
                    std::abort();
            }
        });
        direct_samples[sample] = elapsedNanoseconds([&]
        {
            for (std::size_t repeat = 0; repeat < kRepeats; ++repeat)
            {
                const auto result = lux::ecs::detail::dispatchBoundCalls(
                    calls,
                    frame
                );
                if (result.result != 0)
                    std::abort();
            }
        });
        struct_baseline_samples[sample] = elapsedNanoseconds([&]
        {
            for (std::size_t repeat = 0; repeat < kRepeats; ++repeat)
            {
                const auto result = runStructBaseline(calls, frame);
                if (result.result != 0)
                    std::abort();
            }
        });
    }

    const auto direct_ns = median(direct_samples);
    const auto baseline_ns = median(baseline_samples);
    const auto struct_baseline_ns = median(struct_baseline_samples);
    const double ratio = static_cast<double>(direct_ns)
        / static_cast<double>(struct_baseline_ns);
    std::printf(
        "direct-dispatch: calls=%zu repeats=%zu median=%llu ns "
        "raw-baseline=%llu ns equivalent-baseline=%llu ns ratio=%.4f "
        "allocations=0 counter=%llu\n",
        kCallCount,
        kRepeats,
        static_cast<unsigned long long>(direct_ns),
        static_cast<unsigned long long>(baseline_ns),
        static_cast<unsigned long long>(struct_baseline_ns),
        ratio,
        static_cast<unsigned long long>(call_counter)
    );

    #if defined(NDEBUG)
    if (ratio > 1.10)
        return 4;
    #endif
    return 0;
}
