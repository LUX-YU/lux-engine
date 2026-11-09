#include <lux/engine/object/detail/Reclamation.hpp>

#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/object/LuxObject.hpp>

#include <memory>
#include <new>

namespace lux::object::detail
{
    Reclamation::Reclamation() noexcept
        : delivery(stdexec::connect(
              stdexec::schedule(execution::ExecutionRuntime::instance().mainScheduler()),
              Receiver{this}
          ))
    {
    }

    void Reclamation::Receiver::set_value() && noexcept
    {
        auto* current = node;
        // Complete the old operation before reusing its storage. reclaim may delete the entire node.
        std::destroy_at(&current->delivery);
        new (&current->delivery) Delivery(stdexec::connect(
            stdexec::schedule(execution::ExecutionRuntime::instance().mainScheduler()),
            Receiver{current}
        ));
        current->queued.store(false, std::memory_order_release);
        if (LuxObject::isDispatching() || !current->reclaim(*current))
        {
            scheduleReclamation(*current);
        }
    }

    void scheduleReclamation(Reclamation& node) noexcept
    {
        if (!node.queued.exchange(true, std::memory_order_acq_rel))
        {
            // This already-owned cleanup operation bypasses new business-work admission during shutdown.
            stdexec::start(node.delivery);
        }
    }

} // namespace lux::object::detail
