#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/execution/HistoryState.hpp>

namespace lux::render::detail
{
    void RenderGraphInstance::HistoryState::release(const nri::CoreInterface& core) noexcept
    {
        for (auto& slot : slots)
        {
            for (auto* view : slot.views)
            {
                core.DestroyDescriptor(view);
            }
            for (auto* view : slot.initial_views)
            {
                core.DestroyDescriptor(view);
            }
            core.DestroyTexture(slot.image);
            core.DestroyBuffer(slot.buffer);
        }
    }

    bool RenderGraphInstance::HistoryState::select(const DeviceSession& session) noexcept
    {
        const auto reusable = [&](const HistoryState::HistorySlot& slot)
        {
            for (std::size_t q = 0; q < session.queueCount(); ++q)
            {
                if (slot.last_use.serials[q] > session.physicalQueue(q).completed())
                {
                    return false;
                }
            }
            return true;
        };

        if (invalidate)
        {
            latest = kGraphInvalidIndex;
            invalidate = false;
        }
        previous = latest;
        current = kGraphInvalidIndex;
        for (std::uint32_t i = 0; i < slots.size(); ++i)
        {
            if (i == previous || !reusable(slots[i]))
            {
                continue;
            }
            if (previous == kGraphInvalidIndex)
            {
                previous = i;
            }
            else
            {
                current = i;
                break;
            }
        }
        if (current == kGraphInvalidIndex)
        {
            return false;
        }
        clear_previous = latest == kGraphInvalidIndex;
        clear_current = !slots[current].initialized;
        return true;
    }

    void RenderGraphInstance::HistoryState::submitted(std::size_t queue, std::uint64_t serial, bool produced) noexcept
    {
        auto& read = slots[previous];
        auto& written = slots[current];
        read.initialized = written.initialized = true;
        read.last_use.serials[queue] = written.last_use.serials[queue] = serial;
        if (produced)
        {
            written.sample_sequence = ++sequence;
            latest = current;
        }
    }
} // namespace lux::render::detail
