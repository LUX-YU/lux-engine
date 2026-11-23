#pragma once

#include "Support.hpp"

namespace sdk_test
{
    // The receipt and shared-image fixtures request NRI's dedicated families when available.
    // Distinct graphics queues are also valid on adapters with a single queue family.
    inline nri::Queue& gateQueue(lux::render::detail::DeviceSession& device, lux::render::detail::EQueueRole role)
    {
        using lux::render::detail::EQueueRole;
        const auto graphics = device.queueIndex(EQueueRole::GRAPHICS);
        const auto selected = device.queueIndex(role);
        const auto compute = device.queueIndex(EQueueRole::COMPUTE);
        auto type = nri::QueueType::GRAPHICS;
        unsigned index = 0;
        const auto& available = device.description().adapterDesc.queueNum;
        if (selected != graphics)
        {
            type = selected == compute ? nri::QueueType::COMPUTE : nri::QueueType::COPY;
            if (!available[static_cast<unsigned>(type)])
            {
                const bool separate_graphics_compute = compute != graphics && !available[static_cast<unsigned>(nri::QueueType::COMPUTE)];
                index = 1u + (selected != compute && separate_graphics_compute);
                type = nri::QueueType::GRAPHICS;
            }
        }
        nri::Queue* queue = nullptr;
        CHECK(device.nriCore().GetQueue(device.device(), type, index, queue) == nri::Result::SUCCESS && queue);
        return *queue;
    }
}
