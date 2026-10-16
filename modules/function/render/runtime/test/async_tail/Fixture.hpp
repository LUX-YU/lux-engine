#include "../ErasedRegistrationFixture.hpp"
#pragma once

#include <lux/engine/render/features/Registration.hpp>
#include <lux/engine/render/resource/AsyncWork.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <semaphore>

namespace async_tail_test
{
    enum class ETerminal : std::uint8_t { VALUE, FAILURE, STOPPED };

    struct Command final
    {
        static constexpr std::string_view name = "test.async.dll_callback_tail";
        using Request = std::uint32_t;
        using Response = std::uint32_t;
    };

    // EXE-owned gates stay valid after the operation state and Server are gone.
    struct Gate final
    {
        ETerminal terminal;
        std::binary_semaphore published{0};
        std::binary_semaphore release{0};
        std::atomic<unsigned> states_destroyed{0};
        std::atomic<unsigned> tails_returned{0};
    };

    struct Host final
    {
        lux::render::CpuSchedule cpu;
        std::weak_ptr<const void> module;
        Gate* gate;
        std::array<lux::render::CommandRegistration, 1> handlers;
    };

    // The export has a stable symbol name; its parameters still require the
    // same C++ toolchain/ABI on both sides, as do the real Feature contracts.
    using MakeFeature = void (*)(
        Host&, const std::shared_ptr<const void>&, lux::render::FeatureRegistration&
    ) noexcept;
}
