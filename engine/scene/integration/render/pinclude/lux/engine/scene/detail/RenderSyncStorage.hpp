#pragma once

#include <atomic>
#include <lux/engine/function/render/client/BoundedSpscFrameRing.hpp>
#include <lux/engine/function/render/client/RenderProgram.hpp>

namespace lux::scene::detail
{
    // Only packet slots and synchronization facts cross the Scene/Main boundary.
    struct RenderSyncStorage final
    {
        lux::cxx::BoundedSpscFrameRing<render::RenderProgram<>, 3> updates{1U};
        std::atomic_uint64_t progress{};
        std::atomic_uint64_t published{};
        std::atomic_uint64_t forwarded{};
        std::atomic_uint64_t backpressured{};
        std::uint64_t retired_unforwarded{}; // Main-only, after producer terminal.
        std::atomic_bool producer_closed{};
        std::atomic_bool consumer_closed{};

        void notify() noexcept
        {
            progress.fetch_add(1, std::memory_order_release);
            progress.notify_all();
        }
    };
} // namespace lux::scene::detail
