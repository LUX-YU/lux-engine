#include <algorithm>
#include <numeric>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/execute/PassExtent.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/graph/execute/PhysicalFrameAllocation.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/compile/PhysicalResourcePlan.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>

namespace lux::render::detail
{
    ResourceInput RenderGraphInstance::resourceInput(GraphResourceId resource, std::uint32_t slot) const noexcept
    {
        return frames_[slot].bindings[resource.index].input();
    }

    void RenderGraphInstance::bindImage(GraphResourceId resource, std::uint32_t slot, nri::Texture& image) noexcept
    {
        auto binding = resourceInput(resource, slot);
        if (binding.image == &image)
        {
            return;
        }
        binding.image = &image;
        setResource(resource.index, slot, binding);
    }

    RenderResult<void> RenderGraphInstance::createFrameResources(std::uint32_t slot) noexcept
    {
        auto& session = *session_;
        const auto& core = session.nriCore();
        const auto& plan = graph_;
        auto& frame = frames_[slot];
        auto allocation = frame.native.preparePhysical(session, {plan, target(), std::nullopt});
        if (!allocation) return cxx::unexpected(allocation.error());
        frame.memory_bindings = std::move(allocation->placements);
        for (auto& replacement : allocation->resources)
            frame.bindings[replacement.index] = std::move(replacement.binding);
        for (std::uint32_t i = 0; i < frame.bindings.size(); ++i)
        {
            auto& binding = frame.bindings[i];
            const auto& resource = plan.resources()[i];
            if (!resource.memory_used || resource.shared_table)
            {
                continue;
            }
            if (resource.history != kGraphInvalidIndex)
            {
                const auto& history = histories_[resource.history];
                const auto selected =
                    resource.origin == EGraphOrigin::HISTORY_PREVIOUS ? history.previous : history.current;
                const auto& physical = history.slots[selected];
                binding.select(physical.buffer, physical.image, core);
                binding.buffer_size = physical.buffer_size;
                continue;
            }
            if (resource.origin == EGraphOrigin::FRAME_BUFFER) continue;
            if (resource.origin == EGraphOrigin::IMPORTED)
            {
                if (std::holds_alternative<GraphImageDesc>(resource.description))
                {
                    if (!binding.image)
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                    binding.select(binding.buffer, binding.image, core);
                    if (auto valid = qualifyImportedImage({i}, *binding.image, target()); !valid) return valid;
                }
                continue;
            }
            if (i == plan.output())
            {
                binding.select(nullptr, output_.frames[slot].image, core);
                continue;
            }
            if (const auto* image = std::get_if<GraphImageDesc>(&resource.description); image && image->runtime_layers)
            {
                auto created = createGraphResource(session, resource, target(), ETransientAllocation::INDEPENDENT);
                if (!created) return cxx::unexpected(created.error());
                binding = *created;
                frame.native.images.push_back(binding.image);
                frame.layered_images.push_back({GraphResourceId{i}});
                continue;
            }
        }
        for (auto& binding : frame.bindings) binding.select(binding.buffer, binding.image, core);
        return {};
    }

    void RenderGraphInstance::prepareAliasBarriers(FrameResources& frame) noexcept
    {
        if (frame.memory_bindings.empty()) return;
        const auto barriers = instantiateAliasBarriers(
            graph_.physical_resources_, frame.memory_bindings, frame.passes.size()
        );
        for (std::size_t p = 0; p < barriers.size(); ++p) frame.passes[p].alias_barrier = barriers[p];
    }

    RenderResult<void> RenderGraphInstance::qualifyImportedImage(
        GraphResourceId id, const nri::Texture& image, const TargetDesc& target
    ) const noexcept
    {
        const auto& resource = graph_.resources()[id.index];
        const auto* expected = std::get_if<GraphImageDesc>(&resource.description);
        if (!expected) return cxx::unexpected(RenderError{kStorageMismatch});
        const auto& actual = session_->nriCore().GetTextureDesc(image);
        auto width = expected->width, height = expected->height;
        if (resource.target_sized)
        {
            const auto resolved = graphTextureDescription(*session_, resource, target);
            if (!resolved) return cxx::unexpected(resolved.error());
            width = resolved->width;
            height = resolved->height;
        }
        const bool wrong_extent = expected->extent.source != EImageExtentSource::IMPORTED &&
            (actual.width != width || actual.height != height);
        const bool wrong_shape = actual.type != nri::TextureType::TEXTURE_2D || actual.depth != 1 ||
            actual.mipNum != expected->mips || actual.layerNum != expected->layers ||
            actual.sampleNum != expected->samples;
        const bool wrong_format = actual.format != imageFormat(expected->format).native;
        const bool wrong_sharing = resource.concurrent_image_read && actual.sharingMode != nri::SharingMode::CONCURRENT;
        const bool missing_usage = (actual.usage & resource.image_usage) != static_cast<unsigned>(resource.image_usage);
        if (wrong_extent || wrong_shape || wrong_format || missing_usage || wrong_sharing)
            return cxx::unexpected(RenderError{kDeviceArgument});
        return {};
    }

    bool RenderGraphInstance::dumpResources(DumpSink sink) const noexcept
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
                session_->nriCore().GetBufferMemoryDesc(*prepared.buffer, nri::MemoryLocation::DEVICE, memory);
                bytes += memory.size;
                ++prepared_buffers;
            }
            for (auto* buffer : frame.native.buffers)
            {
                nri::MemoryDesc memory{};
                session_->nriCore().GetBufferMemoryDesc(*buffer, nri::MemoryLocation::DEVICE, memory);
                bytes += memory.size;
            }
            for (auto* image : frame.native.images)
            {
                nri::MemoryDesc memory{};
                session_->nriCore().GetTextureMemoryDesc(*image, nri::MemoryLocation::DEVICE, memory);
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
                      ",\"imported_revision\":" + std::to_string(frame.imported_revision) +
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
                    session_->nriCore().GetTextureMemoryDesc(*slot.image, nri::MemoryLocation::DEVICE, memory);
                }
                else
                {
                    session_->nriCore().GetBufferMemoryDesc(*slot.buffer, nri::MemoryLocation::DEVICE, memory);
                }
                output += "{\"sample\":" + std::to_string(history.sample_sequences[s]) +
                          ",\"initialized\":" + (slot.initialized ? "true" : "false") +
                          ",\"allocation_requirement_bytes\":" + std::to_string(memory.size) + ",\"last_use\":[";
                for (std::size_t q = 0; q < session_->queueCount(); ++q)
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

    nri::Buffer* RenderGraphInstance::nativeBuffer(std::uint32_t resource, std::uint32_t slot) const noexcept
    {
        return frames_[slot].bindings[resource].buffer;
    }

    void
    RenderGraphInstance::setResource(std::uint32_t resource, std::uint32_t slot, const ResourceInput& value) noexcept
    {
        const auto first = slot == UINT32_MAX ? 0 : slot;
        const auto end = slot == UINT32_MAX ? static_cast<std::uint32_t>(frames_.size()) : slot + 1;
        for (auto current = first; current < end; ++current)
            frames_[current].bindResource(graph_, *session_, resource, value);
    }
} // namespace lux::render::detail
