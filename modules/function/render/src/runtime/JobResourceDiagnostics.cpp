#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <algorithm>
#include <numeric>

namespace lux::render::runtime_detail
{
    bool JobExecution::dumpResources(DumpSink sink) const noexcept
    {
        if (!sink.write)
        {
            return false;
        }
        std::string output = "{\"allocation\":\"compiled physical slots\",\"frames\":[";
        for (std::size_t i = 0; i < frames_.size(); ++i)
        {
            if (i)
            {
                output += ',';
            }
            const auto& frame = frames_[i];
            std::uint64_t bytes = 0;
            std::size_t prepared_buffers = 0;
            for (const auto& prepared : frame.native.prepared_buffers)
            {
                if (!prepared.buffer) continue;
                nri::MemoryDesc memory{};
                gpu_.session.nriCore().GetBufferMemoryDesc(*prepared.buffer, nri::MemoryLocation::DEVICE, memory);
                bytes += memory.size;
                ++prepared_buffers;
            }
            for (auto* buffer : frame.native.buffers)
            {
                nri::MemoryDesc memory{};
                gpu_.session.nriCore().GetBufferMemoryDesc(*buffer, nri::MemoryLocation::DEVICE, memory);
                bytes += memory.size;
            }
            for (auto* image : frame.native.images)
            {
                nri::MemoryDesc memory{};
                gpu_.session.nriCore().GetTextureMemoryDesc(*image, nri::MemoryLocation::DEVICE, memory);
                bytes += memory.size;
            }
            const auto allocated = std::accumulate(
                frame.native.memories.begin(),
                frame.native.memories.end(),
                std::uint64_t{0},
                [](auto sum, const auto& allocation) { return sum + allocation.bytes; }
            );
            std::size_t command_count = 0;
            for (const auto& batch : frame.native.batches)
                command_count += std::ranges::count_if(
                    batch.commands, [](auto* command) noexcept { return command != nullptr; }
                );
            output += "{\"slot\":" + std::to_string(i) +
                      ",\"owned_command_buffers\":" + std::to_string(command_count) +
                      ",\"owned_buffers\":" +
                          std::to_string(frame.native.buffers.size() + prepared_buffers) +
                      ",\"owned_textures\":" + std::to_string(frame.native.images.size()) +
                      ",\"allocation_requirement_bytes\":" + std::to_string(bytes) +
                      ",\"explicit_memory_count\":" + std::to_string(frame.native.memories.size()) +
                      ",\"explicit_memory_bytes\":" + std::to_string(allocated) + ",\"memory_bindings\":[";
            constexpr std::array
                reasons{
                    "excluded", "dedicated", "rendering_scope", "distinct_requirements", "incompatible_layout",
                    "unordered", "aliased"
                };
            for (std::size_t s = 0; s < frame.memory_bindings.size(); ++s)
            {
                if (s)
                {
                    output += ',';
                }
                const auto& binding = frame.memory_bindings[s];
                output += "{\"allocation\":" + std::to_string(binding.group.index) + ",\"reason\":\"" +
                          reasons[static_cast<std::size_t>(binding.reason)] + "\"}";
            }
            output += "],\"alias_barriers\":[";
            bool first_barrier = true;
            for (std::size_t p = 0; p < frame.passes.size(); ++p)
            {
                const auto& barrier = frame.passes[p].alias_barrier;
                if (!barrier) continue;
                if (!first_barrier) output += ',';
                first_barrier = false;
                output += "{\"pass\":" + std::to_string(p) +
                    ",\"before_access\":" + std::to_string(static_cast<std::uint32_t>(barrier->before.access)) +
                    ",\"before_stages\":" + std::to_string(static_cast<std::uint32_t>(barrier->before.stages)) +
                    ",\"after_access\":" + std::to_string(static_cast<std::uint32_t>(barrier->after.access)) +
                    ",\"after_stages\":" + std::to_string(static_cast<std::uint32_t>(barrier->after.stages)) + "}";
            }
            output += "]}";
        }
        output += "],\"histories\":[";
        for (std::size_t i = 0; i < histories_.size(); ++i)
        {
            if (i)
            {
                output += ',';
            }
            const auto& history = histories_[i];
            output += "{\"previous_resource\":" + std::to_string(history.plan->description.previous.index) +
                      ",\"current_resource\":" + std::to_string(history.plan->description.current.index) +
                      ",\"sequence\":" + std::to_string(history.sequence) +
                      ",\"latest_slot\":" + std::to_string(history.latest) + ",\"slots\":[";
            for (std::size_t s = 0; s < history.slots.size(); ++s)
            {
                if (s)
                {
                    output += ',';
                }
                const auto& slot = history.slots[s];
                nri::MemoryDesc memory{};
                if (slot.image)
                {
                    gpu_.session.nriCore().GetTextureMemoryDesc(*slot.image, nri::MemoryLocation::DEVICE, memory);
                }
                else
                {
                    gpu_.session.nriCore().GetBufferMemoryDesc(*slot.buffer, nri::MemoryLocation::DEVICE, memory);
                }
                output += "{\"sample\":" + std::to_string(history.sample_sequences[s]) +
                          ",\"initialized\":" + (slot.initialized ? "true" : "false") +
                          ",\"allocation_requirement_bytes\":" + std::to_string(memory.size) + ",\"last_use\":[";
                for (std::size_t q = 0; q < gpu_.session.queueCount(); ++q)
                {
                    if (q)
                    {
                        output += ',';
                    }
                    output += std::to_string(slot.last_use.serials()[q]);
                }
                output += "]}";
            }
            output += "]}";
        }
        output += "],\"excludes\":\"borrowed Target/inputs, allocator pool reserve and descriptors\"}";
        return sink.write(sink.destination, output);
    }
} // namespace lux::render::runtime_detail
