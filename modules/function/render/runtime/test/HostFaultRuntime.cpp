#include "RuntimeBackend.hpp"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdlib>
#include <utility>

namespace runtime_host_test
{
    unsigned fail_operation{};
    std::array<unsigned, 3> created{};
    unsigned closed{};
    std::atomic<unsigned> workers{};
    std::array<HANDLE, 3> handles{};

    bool fail(unsigned operation) noexcept
    {
        if (fail_operation != operation)
        {
            return false;
        }
        fail_operation = 0;
        SetLastError(ERROR_NOT_ENOUGH_QUOTA);
        return true;
    }

    HANDLE event(LPSECURITY_ATTRIBUTES attributes, BOOL manual, BOOL signaled, LPCWSTR name) noexcept
    {
        if (fail(1))
        {
            return nullptr;
        }
        const auto handle = CreateEventW(attributes, manual, signaled, name);
        if (handle)
        {
            handles[0] = handle;
            ++created[0];
        }
        return handle;
    }

    HANDLE timer(LPSECURITY_ATTRIBUTES attributes, LPCWSTR name, DWORD flags, DWORD access) noexcept
    {
        if (fail(2))
        {
            return nullptr;
        }
        const auto handle = CreateWaitableTimerExW(attributes, name, flags, access);
        if (handle)
        {
            handles[1] = handle;
            ++created[1];
        }
        return handle;
    }

    struct Start
    {
        LPTHREAD_START_ROUTINE run;
        void* argument;
    };

    // Tests start one Runtime at a time. No heap state or production factory.
    Start start{};

    HANDLE thread(
        LPSECURITY_ATTRIBUTES attributes,
        SIZE_T stack,
        LPTHREAD_START_ROUTINE run,
        LPVOID argument,
        DWORD flags,
        LPDWORD id
    ) noexcept
    {
        if (fail(3))
        {
            return nullptr;
        }
        start = {run, argument};
        const auto handle = CreateThread(
            attributes,
            stack,
            [](void*) -> DWORD
            {
                ++workers;
                const auto result = start.run(start.argument);
                --workers;
                return result;
            },
            nullptr,
            flags,
            id
        );
        if (handle)
        {
            handles[2] = handle;
            ++created[2];
        }
        return handle;
    }

    BOOL close(HANDLE handle) noexcept
    {
        bool found = false;
        for (auto& owned : handles)
        {
            if (owned == handle)
            {
                owned = nullptr;
                found = true;
                break;
            }
        }
        if (!found)
        {
            std::abort();
        }
        ++closed;
        return CloseHandle(handle);
    }
} // namespace runtime_host_test

// Compile the actual implementation with only these four Win32 calls replaced.
// The production library and its public headers contain no injection seam.
#define CreateEventW runtime_host_test::event
#define CreateWaitableTimerExW runtime_host_test::timer
#define CreateThread runtime_host_test::thread
#define CloseHandle runtime_host_test::close
#include "../src/Runtime.cpp"
