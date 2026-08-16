#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <Windows.h>
#elif defined(__linux__) || defined(__ANDROID__)
#  include <linux/futex.h>
#  include <sys/syscall.h>
#  include <unistd.h>
#endif

namespace lux::common
{
    /// Wait until a monotonically changing 64-bit atomic differs from
    /// `observed`, or until `deadline` is reached. The Linux futex path waits
    /// on the low word only as a wake hint; the full 64-bit value is always
    /// rechecked before returning.
    [[nodiscard]] inline bool waitAtomicU64Until(
        std::atomic<std::uint64_t>& value,
        std::uint64_t observed,
        std::chrono::steady_clock::time_point deadline) noexcept
    {
        if (value.load(std::memory_order_acquire) != observed)
            return true;

        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero())
            return false;

#if defined(_WIN32)
        const auto millis = std::chrono::ceil<std::chrono::milliseconds>(
            remaining);
        const DWORD timeout = millis.count() >= INFINITE - 1u
            ? INFINITE - 1u
            : static_cast<DWORD>(millis.count());
        using WaitOnAddressFn = BOOL (WINAPI*)(
            volatile VOID*, PVOID, SIZE_T, DWORD);
        static const auto wait_on_address = []() noexcept
        {
            const auto kernel_base = ::GetModuleHandleW(L"KernelBase.dll");
            if (kernel_base == nullptr)
                std::terminate();
            const auto function = reinterpret_cast<WaitOnAddressFn>(
                ::GetProcAddress(kernel_base, "WaitOnAddress"));
            if (function == nullptr)
                std::terminate();
            return function;
        }();
        (void)wait_on_address(
            static_cast<volatile void*>(&value),
            &observed,
            sizeof(observed),
            timeout);
#elif defined(__linux__) || defined(__ANDROID__)
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(remaining);
        const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
            remaining - seconds);
        timespec timeout{
            static_cast<time_t>(seconds.count()),
            static_cast<long>(nanos.count())};
        const auto expected_low = static_cast<std::uint32_t>(observed);
        (void)::syscall(
            SYS_futex,
            reinterpret_cast<std::uint32_t*>(&value),
            FUTEX_WAIT_PRIVATE,
            expected_low,
            &timeout,
            nullptr,
            0);
#else
        value.wait(observed, std::memory_order_acquire);
#endif
        return value.load(std::memory_order_acquire) != observed;
    }
}
