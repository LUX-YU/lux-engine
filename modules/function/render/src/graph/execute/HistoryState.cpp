#include <lux/engine/render/graph/execute/PhysicalFrameAllocation.hpp>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <algorithm>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>

namespace lux::render::detail
{
    bool HistoryState::select(const DeviceSession& session) noexcept
    {
        if (invalidate)
        {
            retained_samples = 0;
            latest = kGraphInvalidIndex;
            invalidate = false;
        }
        previous = kGraphInvalidIndex;
        current = kGraphInvalidIndex;
        const auto distance = plan->description.distance;
        const auto oldest = retained_samples ? sequence - (retained_samples - 1) : 0;
        clear_previous = retained_samples < distance;
        for (std::uint32_t i = 0; i < slots.size(); ++i)
        {
            const auto& slot = slots[i];
            if (oldest != 0 && sample_sequences[i] >= oldest)
            {
                if (!clear_previous && sample_sequences[i] == oldest)
                {
                    previous = i;
                }
                continue;
            }
            if (!session.reached(slot.last_use))
            {
                continue;
            }
            if (clear_previous && previous == kGraphInvalidIndex)
            {
                previous = i;
            }
            else if (current == kGraphInvalidIndex)
            {
                current = i;
            }
        }
        if (previous == kGraphInvalidIndex || current == kGraphInvalidIndex)
        {
            return false;
        }
        clear_current = !slots[current].initialized;
        return true;
    }

    void HistoryState::publish() noexcept
    {
        if (sequence == UINT64_MAX)
        {
            std::terminate();
        }
        sample_sequences[current] = ++sequence;
        latest = current;
        if (retained_samples < plan->description.distance)
        {
            ++retained_samples;
        }
    }

    void HistoryState::bindBank(const HistoryPlan& recipe, JobHistoryResources& backing) noexcept
    {
        plan = &recipe;
        slots = backing.slots;
        sample_sequences.resize(slots.size());
    }

    RenderResult<void> JobHistoryResources::initialize(
        DeviceSession& session, const HistoryPlan& recipe, const CompiledResource& resource, const TargetDesc& target
    ) noexcept
    {
        const auto& core = session.nriCore();
        for (auto& slot : slots)
        {
            slot.last_use = session.makeFrontiers();
            auto created = createGraphResource(session, resource, target, ETransientAllocation::INDEPENDENT);
            if (!created) return cxx::unexpected(created.error());
            slot.image = created->image;
            slot.buffer = created->buffer;
            slot.buffer_size = created->buffer_size;
            if (slot.buffer) slot.buffer_description = &core.GetBufferDesc(*slot.buffer);
        }
        for (auto& slot : slots)
        {
            slot.views.reserve(recipe.views.size());
            for (const auto& use : recipe.views)
            {
                nri::Descriptor* view = nullptr;
                nri::Result result;
                if (const auto* image = std::get_if<nri::TextureViewDesc>(&use.description))
                {
                    auto native = *image;
                    native.texture = slot.image;
                    result = core.CreateTextureView(native, view);
                }
                else
                {
                    auto native = std::get<nri::BufferViewDesc>(use.description);
                    native.buffer = slot.buffer;
                    result = createGraphBufferView(session, native, view);
                }
                if (result != nri::Result::SUCCESS) return cxx::unexpected(nativeError(result));
                slot.views.push_back(view);
            }
        }
        // Initialization views are immutable and shared by the safe frame descriptor banks.
        for (auto& slot : slots)
        {
            if (!slot.image)
            {
                continue;
            }
            const auto& image = core.GetTextureDesc(*slot.image);
            slot.image_description = &image;
            const auto& graph_image =
                std::get<GraphImageDesc>(resource.description);
            const bool depth = rdesc::textureAspectMask(graph_image.format) != 1;
            for (nri::Dim_t mip = 0; mip < image.mipNum; ++mip)
            {
                nri::Descriptor* view = nullptr;
                const nri::TextureViewDesc input{
                    slot.image,
                    depth ? nri::TextureView::DEPTH_STENCIL_ATTACHMENT : nri::TextureView::COLOR_ATTACHMENT,
                    image.format,
                    mip,
                    1,
                    0,
                    image.layerNum
                };
                const auto result = core.CreateTextureView(input, view);
                if (result != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
                slot.initial_views.push_back(view);
            }
        }
        return {};
    }

    void FrameResources::bindHistory(std::span<const HistoryState> histories, const RecordingDependencies& dependencies) noexcept
    {
        for (std::uint32_t h = 0; h < histories.size(); ++h)
        {
            const auto& history = histories[h];
            const auto clear = [&](bool required, std::uint32_t slot) noexcept
            {
                return required ? FrameResources::HistoryClear{slot, history.slots[slot].initialized}
                                : FrameResources::HistoryClear{};
            };
            const std::array clears{clear(history.clear_previous, history.previous),
                                    clear(history.clear_current, history.current)};
            if (history_clears[h] != clears)
            {
                history_clears[h] = clears;
                pending_history_clears.mark(h);
            }
            const bool valid = !history.clear_previous;
            const bool validity_changed = static_cast<bool>(history_valid[h]) != valid;
            history_valid[h] = valid;
            const auto& description = history.plan->description;
            for (const auto [resource, selected] :
                 {std::pair{description.previous, history.previous}, std::pair{description.current, history.current}})
            {
                const auto& physical = history.slots[selected];
                auto& binding = bindings[resource.index];
                const bool unchanged = binding.image == physical.image && binding.buffer == physical.buffer &&
                    binding.buffer_size == physical.buffer_size;
                if (unchanged && !validity_changed) continue;
                pending_history_views.mark(h);
                binding.image = physical.image;
                binding.image_description = physical.image_description;
                binding.buffer = physical.buffer;
                binding.buffer_description = physical.buffer_description;
                binding.buffer_size = physical.buffer_size;
                invalidateResource(dependencies, resource.index);
                if (!binding.dirty)
                {
                    binding.dirty = true;
                    changed_resources.push_back(resource.index);
                }
            }
        }
    }

    void FrameResources::updateHistoryViews(std::span<const HistoryState> histories, const nri::CoreInterface& core) noexcept
    {
        using Target = EImageViewTarget;
        for (const auto h : pending_history_views.slots())
        {
            const auto& history = histories[h];
            for (std::size_t i = 0; i < history.plan->views.size(); ++i)
            {
                const auto& use = history.plan->views[i];
                const auto selected = use.previous ? history.previous : history.current;
                auto* view = history.slots[selected].views[i];
                auto& pass = passes[use.pass];
                switch (use.target)
                {
                case Target::DESCRIPTOR:
                {
                    if (!history_valid[h] && use.has_fallback)
                    {
                        view = pass.private_views[use.index];
                    }
                    if (pass.history_views[use.index] == view) break;
                    const nri::UpdateDescriptorRangeDesc
                        update{pass.private_descriptors, use.binding, use.element, &view, 1};
                    core.UpdateDescriptorRanges(&update, 1);
                    pass.history_views[use.index] = view;
                    break;
                }
                case Target::COLOR:
                    pass.colors[use.index].descriptor = view;
                    break;
                case Target::DEPTH:
                    pass.depth.descriptor = view;
                    break;
                case Target::COLOR_RESOLVE:
                    pass.colors[use.index].resolveDst = view;
                    break;
                case Target::DEPTH_RESOLVE:
                    pass.depth.resolveDst = view;
                    break;
                case Target::CLEAR:
                    pass.clear_views[use.index] = view;
                    break;
                }
            }
        }
        pending_history_views.clear();
    }

    void FrameResources::freezeHistoryInitializations(
        std::span<const HistoryState> histories, std::span<const CompiledResource> resources, nri::GraphicsAPI api
    ) noexcept
    {
        for (const auto h : pending_history_clears.slots())
        {
            const auto& history = histories[h];
            const auto& description = history.plan->description;
            const auto canonical_layout = description.canonical_use == EGraphUse::SAMPLED ? nri::Layout::SHADER_RESOURCE
                : description.canonical_use == EGraphUse::TRANSFER_SOURCE ? nri::Layout::COPY_SOURCE
                                                                         : nri::Layout::SHADER_RESOURCE_STORAGE;
            const auto canonical_access = description.canonical_use == EGraphUse::SAMPLED
                ? nri::AccessBits::SHADER_RESOURCE
                : description.canonical_use == EGraphUse::TRANSFER_SOURCE ? nri::AccessBits::COPY_SOURCE
                                                                         : nri::AccessBits::SHADER_RESOURCE_STORAGE;
            const auto& selections = history_clears[h];
            for (std::size_t side = 0; side < selections.size(); ++side)
            {
                // History banks own disjoint resources. Each active clear is independently recordable.
                const auto key = static_cast<std::size_t>(h) * selections.size() + side;
                const auto selection = selections[side];
                if (selection.slot == kGraphInvalidIndex)
                {
                    history_initializations.erase(key);
                    continue;
                }
                const auto& selected = history.slots[selection.slot];
                auto& record = history_initializations.emplace(key);
                if (selected.image)
                {
                    const auto& graph_image =
                        std::get<GraphImageDesc>(resources[description.current.index].description);
                    const auto aspects = rdesc::textureAspectMask(graph_image.format);
                    record.depth = aspects != 1;
                    record.views = selected.initial_views;
                    auto& transition = record.image_before;
                    transition.texture = selected.image;
                    if (selected.initialized)
                        transition.before = {canonical_access, canonical_layout, nri::StageBits::ALL};
                    transition.after = record.depth
                        ? nri::AccessLayoutStage{
                            nri::AccessBits::DEPTH_STENCIL_ATTACHMENT, nri::Layout::DEPTH_STENCIL_ATTACHMENT,
                            nri::StageBits::DEPTH_STENCIL_ATTACHMENT
                        }
                        : nri::AccessLayoutStage{
                            nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT,
                            nri::StageBits::COLOR_ATTACHMENT
                        };
                    transition.mipNum = selected.image_description->mipNum;
                    transition.layerNum = selected.image_description->layerNum;
                    transition.planes = record.depth ? nri::PlaneBits::DEPTH : nri::PlaneBits::COLOR;
                    if (aspects & 4) transition.planes |= nri::PlaneBits::STENCIL;
                    if (record.depth) record.clear_value.depthStencil.depth = description.initial.depth;
                    else
                    {
                        const auto& color = description.initial.color;
                        record.clear_value.color.f = {color[0], color[1], color[2], color[3]};
                    }
                    record.image_after = transition;
                    record.image_after.before = transition.after;
                    record.image_after.after = {canonical_access, canonical_layout, nri::StageBits::ALL};
                }
                else
                {
                    auto& transition = record.buffer_before;
                    transition.buffer = selected.buffer;
                    if (selected.initialized) transition.before = {canonical_access, nri::StageBits::ALL};
                    transition.after = {
                        nri::AccessBits::COPY_DESTINATION, zeroBufferStages(api)
                    };
                    record.buffer_size = selected.buffer_size;
                    record.buffer_after = transition;
                    record.buffer_after.before = transition.after;
                    record.buffer_after.after = {canonical_access, nri::StageBits::ALL};
                }
            }
        }
        pending_history_clears.clear();
    }
} // namespace lux::render::detail
