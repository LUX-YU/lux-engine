#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/PhysicalFrameAllocation.hpp>

namespace lux::render::runtime_detail
{
    using detail::ResourceInput;

    std::uint32_t JobExecution::frameCount() const noexcept
    {
        return static_cast<std::uint32_t>(frames_.size());
    }

    bool JobExecution::frozenForSubmission(std::uint32_t slot) const noexcept
    {
        return slot < frames_.size() && frames_[slot].frozen_for_submission;
    }

    ResourceInput JobExecution::resourceInput(GraphResourceId resource, std::uint32_t slot) const noexcept
    {
        return frames_[slot].bindings[resource.index].input();
    }

    void JobExecution::bindImage(GraphResourceId resource, std::uint32_t slot, nri::Texture& image) noexcept
    {
        auto binding = resourceInput(resource, slot);
        if (binding.image == &image)
        {
            return;
        }
        binding.image = &image;
        bindResource(resource, slot, binding);
    }

    void JobExecution::bindResource(GraphResourceId resource, std::uint32_t slot, const ResourceInput& value) noexcept
    {
        const auto first = slot == UINT32_MAX ? 0 : slot;
        const auto end = slot == UINT32_MAX ? static_cast<std::uint32_t>(frames_.size()) : slot + 1;
        for (auto current = first; current < end; ++current)
            frames_[current].bindResource(graph_, gpu_.session, resource.index, value);
    }

    RenderResult<void>
    JobExecution::qualifyImportedImage(GraphResourceId resource, const nri::Texture& image) const noexcept
    {
        return detail::qualifyImportedImage(gpu_.session, graph_.resources()[resource.index], image, output_.description);
    }

    RenderResult<void> JobExecution::createFrameResources(std::uint32_t slot) noexcept
    {
        auto& session = gpu_.session;
        const auto& core = session.nriCore();
        const auto& plan = graph_;
        auto& frame = frames_[slot];
        auto allocation = frame.native.preparePhysical(session, {plan, output_.description, std::nullopt});
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
                    if (auto valid = qualifyImportedImage({i}, *binding.image); !valid) return valid;
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
                auto created = createGraphResource(
                    session, resource, output_.description, detail::ETransientAllocation::INDEPENDENT
                );
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
} // namespace lux::render::runtime_detail
