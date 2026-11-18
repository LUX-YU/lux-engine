#include "GraphMeasurements.hpp"
#include "Allocation.hpp"

namespace lux::render::detail
{
    void startGraphMeasurements() noexcept
    {
        ipc_allocation::start();
    }

    std::size_t stopGraphMeasurements() noexcept
    {
        return ipc_allocation::stop();
    }
}
