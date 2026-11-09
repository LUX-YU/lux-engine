#pragma once

#include <lux/engine/core/execution/RunLoop.hpp>

#include <atomic>

namespace lux::object::detail
{
    // Embedded in the actual lifetime record. Main scheduling remains available while Execution drains.
    struct Reclamation
    {
        struct Receiver final
        {
            using receiver_concept = stdexec::receiver_t;
            Reclamation* node;

            [[nodiscard]] stdexec::env<> get_env() const noexcept
            {
                return {};
            }

            void set_value() && noexcept;
        };

        using Delivery = stdexec::connect_result_t<execution::RunLoop::ScheduleSender, Receiver>;

        Reclamation() noexcept;
        std::atomic_bool queued{};
        bool (*reclaim)(Reclamation&) noexcept {};
        Delivery delivery;
    };

    void scheduleReclamation(Reclamation&) noexcept;
    void retainReclamation() noexcept;
    void releaseReclamation() noexcept;
} // namespace lux::object::detail
