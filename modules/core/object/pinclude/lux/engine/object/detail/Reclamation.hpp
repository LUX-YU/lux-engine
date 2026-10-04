#pragma once

#include <lux/engine/object/ObjectDispatcher.hpp>

namespace lux::object::detail
{
    // One node per actual lifetime, allocated before release can run on a worker.
    struct Reclamation
    {
        Reclamation* next{};
        bool queued{}; // Dispatcher mutex only.
        bool (*reclaim)(Reclamation&) noexcept {};
    };
    void scheduleReclamation(const ObjectDispatcherRef&, Reclamation&) noexcept;
    void retainReclamation(const ObjectDispatcherRef&) noexcept;
    void releaseReclamation(const ObjectDispatcherRef&) noexcept;
} // namespace lux::object::detail
