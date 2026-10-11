#include "GraphNative.hpp"
#include <sstream>

namespace lux::render::vulkan
{
    namespace
    {
        void rangeJson(std::ostream& output, const VGraphRange& range)
        {
            if (const auto* image = std::get_if<ImageRange>(&range))
            {
                output << "{\"aspect\":" << unsigned(image->aspect) << ",\"mip\":" << image->base_mip
                       << ",\"mips\":" << image->mip_count << ",\"layer\":" << image->base_layer
                       << ",\"layers\":" << image->layer_count << '}';
            }
            else if (const auto* buffer = std::get_if<BufferRange>(&range))
            {
                output << "{\"offset\":" << buffer->byte_offset << ",\"bytes\":" << buffer->byte_count << '}';
            }
            else
            {
                output << "null";
            }
        }

        void stateJson(std::ostream& output, NativeResourceState state)
        {
            output << "{\"stage\":" << state.stages << ",\"access\":" << state.access << ",\"layout\":" << state.layout
                   << ",\"family\":" << state.family << '}';
        }
    } // namespace

    std::size_t ExecutableGraphPlan::traceCapacity() const noexcept
    {
        std::size_t result = 0;
        for (const auto& recipe : backing_->recipes)
        {
            result += 1 + recipe.alias_barriers.size();
            for (const auto& use : recipe.uses)
            {
                result += 4 * use.cells.size(); // release/acquire/host plus a conservative diagnostic margin
            }
        }
        return result;
    }

    std::string nativeTraceJson(std::span<const NativeTraceEvent> trace) noexcept
    {
        std::ostringstream output;
        output << '[';
        for (std::size_t i = 0; i < trace.size(); ++i)
        {
            const auto& event = trace[i];
            output << (i ? ",{\"operation\":" : "{\"operation\":") << unsigned(event.operation)
                   << ",\"pass\":" << event.pass.value() << ",\"pass_key\":" << event.pass_key.value()
                   << ",\"resource\":" << event.resource.value() << ",\"use\":" << event.use_index
                   << ",\"queue\":" << unsigned(event.queue) << ",\"range\":";
            rangeJson(output, event.range);
            output << ",\"before\":";
            stateJson(output, event.before);
            output << ",\"after\":";
            stateJson(output, event.after);
            output << '}';
        }
        output << ']';
        return output.str();
    }

    std::string ExecutableGraphPlan::diagnostics() const noexcept
    {
        std::ostringstream output;
        output << "{\"queues\":[";
        for (unsigned q = 0; q < 3; ++q)
        {
            const auto queue = backing_->queues[q]->nativeQueue();
            output << (q ? ",{" : "{") << "\"family\":" << queue.family << ",\"index\":" << queue.index << '}';
        }
        output << "],\"images\":[";
        bool first = true;
        for (std::size_t slot = 0; slot < backing_->slots.size(); ++slot)
        {
            const auto& resources = backing_->slots[slot].resources;
            for (std::size_t r = 0; r < resources.size(); ++r)
            {
                const auto* image = detail::imageOf(resources[r]);
                if (!image)
                {
                    continue;
                }
                const auto memory = image->memoryBinding();
                output << (first ? "{" : ",{") << "\"slot\":" << slot << ",\"resource\":" << r + 1 << ",\"image\":\""
                       << image->native() << "\",\"memory\":\"" << memory.memory << "\",\"offset\":" << memory.offset
                       << ",\"bytes\":" << memory.size << ",\"memory_type\":" << memory.memory_type
                       << ",\"owns_allocation\":" << (memory.owns_allocation ? "true" : "false") << '}';
                first = false;
            }
        }
        output << "],\"versions\":[";
        first = true;
        std::uint32_t index = 0;
        for (const auto& version : backing_->logical.versions())
        {
            output << (first ? "{" : ",{") << "\"index\":" << index++ << ",\"resource\":" << version.resource.value()
                   << ",\"writer\":" << version.writer.value() << ",\"range\":";
            rangeJson(output, version.range);
            output << ",\"readers\":[";
            for (std::size_t i = 0; i < version.readers.size(); ++i)
            {
                output << (i ? "," : "") << version.readers[i].value();
            }
            output << "]}";
            first = false;
        }
        output << "]}";
        return output.str();
    }
} // namespace lux::render::vulkan
