#pragma once

#include <lux/engine/platform/ipc/IpcMessage.hpp>
#include <cstdlib>
#include <vector>

namespace ipc_typed_test
{
    // Identical POD layout in both modules. The ABI-negative DLL changes the
    // actual standard-library configuration, never this type or its TypeToken.
    struct AbiProbe final
    {
        std::uint64_t value = 0;
    };

    struct Batch final
    {
        std::uint64_t scene = 0;
        std::vector<std::uint64_t> values;
    };

    struct DifferentBatch final
    {
        std::uint64_t scene = 0;
        std::vector<std::uint64_t> values;
    };

    struct PinProbe final
    {
        int* destroyed = nullptr;
        const bool* pin_alive = nullptr;

        PinProbe(int& count, const bool& alive) noexcept : destroyed(&count), pin_alive(&alive) {}
        PinProbe(PinProbe&& other) noexcept
            : destroyed(std::exchange(other.destroyed, nullptr)), pin_alive(other.pin_alive) {}

        ~PinProbe() noexcept
        {
            if (destroyed)
            {
                if (!*pin_alive) std::abort();
                ++*destroyed;
            }
        }
    };
}
