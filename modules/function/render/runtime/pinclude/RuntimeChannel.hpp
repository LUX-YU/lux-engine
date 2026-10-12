#pragma once

#include <lux/cxx/concurrent/BoundedSpscFrameRing.hpp>
#include <lux/engine/render/runtime/Runtime.hpp>

// Windows defines ABSOLUTE, which must not precede the neutral graph declarations.
#include <Windows.h>

namespace lux::render::detail
{
    struct RuntimeChannel
    {
        std::unique_ptr<RenderTransport> transport;
        BoundRenderRoute<RuntimeBindingPublication> publication;
        BoundRenderRoute<RuntimeControl> control;
        std::uint64_t owner;
        HANDLE wake;
        std::atomic<ERuntimeState> state{ERuntimeState::RUNNING};
        std::atomic<bool> stopping{};
        std::array<std::atomic<std::uint64_t>, 6> counters{};
        cxx::BoundedSpscFrameRing<RenderError, 66> diagnostics{64};
        std::optional<RenderError> failure;

        ~RuntimeChannel() noexcept
        {
            CloseHandle(wake);
        }

        void signal() noexcept
        {
            if (!SetEvent(wake))
            {
                std::terminate();
            }
        }

        void stop() noexcept
        {
            stopping.store(true, std::memory_order_release);
            transport->requestStop();
            signal();
        }

        void diagnose(RenderError error) noexcept
        {
            if (auto* value = diagnostics.tryBeginWrite())
            {
                *value = error;
                (void)diagnostics.publishWrite();
            }
        }
    };
} // namespace lux::render::detail
