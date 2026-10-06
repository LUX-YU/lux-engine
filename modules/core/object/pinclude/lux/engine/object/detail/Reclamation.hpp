#pragma once

#include <lux/engine/object/ObjectRuntime.hpp>

namespace lux::object::detail
{
    // One node per actual lifetime, allocated before release can run on a worker.
    struct Reclamation
    {
        Reclamation* next{};
        bool queued{}; // Dispatcher mutex only.
        bool (*reclaim)(Reclamation&) noexcept {};
    };
    void scheduleReclamation(Reclamation&) noexcept;
    void retainReclamation() noexcept;
    void releaseReclamation() noexcept;
} // namespace lux::object::detail
