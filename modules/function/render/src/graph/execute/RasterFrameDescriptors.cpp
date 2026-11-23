#include <lux/engine/render/runtime/JobCpuPreparation.hpp>
#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/graph/execute/RasterFramePreparation.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/runtime/commands/Error.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <unordered_map>

namespace lux::render::detail
{
    RenderResult<void> RasterDescriptorLayout::reserveSet(nri::DescriptorPoolDesc& pool) const noexcept
    {
        for (std::size_t i = 0; i < per_set.size(); ++i)
            if (per_set[i] > UINT32_MAX - pool.*kDescriptorPoolCounts[i])
                return cxx::unexpected(RenderError{kDeviceCapacity});
        for (std::size_t i = 0; i < per_set.size(); ++i) pool.*kDescriptorPoolCounts[i] += per_set[i];
        return {};
    }

    const RasterDescriptorLayout& RasterFramePreparation::rasterBindings(RasterBindingKey key) const noexcept
    {
        const auto* stream = graph_.programs().rasterStream(key.pass);
        const auto selected = gpu_.selectRasterPrograms(key.pass, *stream);
        if (!selected || key.generation != selected->revision || key.program >= selected->programs.size())
            std::terminate();
        return selected->programs[key.program].bindings;
    }

    RenderResult<void> RasterFramePreparation::prepareDescriptorCapacity(bool resize_pool) noexcept
    {
        nri::DescriptorPoolDesc additional{};
        for (const auto p : graph_.recordingDependencies().producer_passes)
        {
            const auto* stream = graph_.programs().rasterStream(p);
            if (!stream) continue;
            auto selected = gpu_.selectRasterPrograms(p, *stream);
            if (!selected) return cxx::unexpected(selected.error());
            const auto programs = selected->programs;
            const auto revision = selected->revision;
            const auto& input = draws_[stream->list];
            auto& rows = frame_.stream_descriptor_rows[stream->list];
            rows.programs.resize(programs.size());
            rows.writes.clear();
            if (input.draws.size() > UINT32_MAX) return cxx::unexpected(RenderError{kDeviceCapacity});
            rows.input_count = 0;
            for (auto& program : rows.programs) program.used = 0;
            for (std::size_t d = 0; d < input.draws.size(); ++d)
            {
                const auto id = input.draws[d].program;
                const bool invalid = id.stream != p || id.index >= programs.size() || id.revision != revision;
                if (invalid) return cxx::unexpected(RenderError{kFrameContract, {p}});
                const auto& bindings = programs[id.index].bindings;
                const auto& draw = input.draws[d];
                const bool valid_ranges = draw.constants.fits(input.constants.size()) &&
                    draw.vertices.fits(input.vertices.size()) &&
                    draw.bindings.fits(input.bindings.size());
                const bool valid_constants = draw.constants.count == bindings.parameter_bytes &&
                    draw.constants.first % 4 == 0;
                const auto count = draw.bindings.count;
                const bool valid_abi = valid_constants && count == bindings.fields.size();
                if (!valid_ranges || !valid_abi) return cxx::unexpected(RenderError{kFrameContract, {p}});
                if (count > frame_.stream_binding_inputs.max_size() - rows.input_count)
                    return cxx::unexpected(RenderError{kDeviceCapacity});
                const auto first_input = rows.input_count;
                rows.input_count += count;
                if (bindings.fields.empty()) continue;
                auto& by_program = rows.programs[id.index];
                const auto ordinal = by_program.used++;
                if (ordinal == by_program.records.size())
                {
                    if (frame_.native.raster_bindings.size() >= kGraphInvalidIndex)
                        return cxx::unexpected(RenderError{kDeviceCapacity});
                    by_program.records.push_back(static_cast<std::uint32_t>(frame_.native.raster_bindings.size()));
                    auto& record = frame_.native.raster_bindings.emplace_back();
                    record.program = {p, id.index, revision};
                    record.list = stream->list;
                    record.ordinal = ordinal;
                    record.views.resize(bindings.fields.size());
                    record.inputs.resize(bindings.fields.size());
                }
                const auto index = by_program.records[ordinal];
                rows.writes.push_back({first_input, static_cast<std::uint32_t>(d), id.index, index});
                if (!frame_.native.raster_bindings[index].set)
                {
                    if (auto counted = bindings.reserveSet(additional); !counted) return counted;
                }
            }
        }
        if (resize_pool) return growDescriptorPool();
        if (!additional.descriptorSetMaxNum) return {};
        const bool fits = frame_.native.descriptor_pool &&
            std::ranges::all_of(kDescriptorPoolCounts, [&](auto field) noexcept
        {
            return additional.*field <= frame_.descriptor_capacity.*field - frame_.descriptor_reserved.*field;
        });
        return fits ? RenderResult<void>{} : growDescriptorPool();
    }

    RenderResult<nri::DescriptorSet*> RasterFramePreparation::descriptorSet(
        const RasterDescriptorLayout& bindings, std::uint32_t index,
        std::span<const RasterDescriptorInput> inputs
    ) noexcept
    {
        auto& record = frame_.native.raster_bindings[index];
        const auto& core = session_.nriCore();
        if (!record.set)
        {
            // Failed native allocation can consume pool quota. The next preflight counts that reservation.
            if (auto counted = bindings.reserveSet(frame_.descriptor_reserved); !counted)
                return cxx::unexpected(counted.error());
            nri::DescriptorSet* created = nullptr;
            const auto status = core.AllocateDescriptorSets(
                *frame_.native.descriptor_pool, *bindings.native.layout, bindings.native.private_set, &created, 1, 0
            );
            if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
            record.set = created;
        }
        bool changed = false;
        cxx::scope_exit invalidate([&]() noexcept
        {
            if (changed) frame_.invalidatePass(graph_, record.program.pass);
        });
        for (std::size_t i = 0; i < bindings.fields.size(); ++i)
        {
            const auto& use = bindings.fields[i];
            const auto& source = inputs[i];
            if (record.inputs[i] == source) continue;

            nri::Descriptor* descriptor = source.sampler;
            auto status = nri::Result::SUCCESS;
            if (source.image)
            {
                const auto& range = source.range;
                const auto type = use.view.image_arrayed
                    ? nri::TextureView::TEXTURE_ARRAY : nri::TextureView::TEXTURE;
                nri::TextureViewDesc view{
                    source.image, type, source.format,
                    static_cast<nri::Dim_t>(range.first_mip), static_cast<nri::Dim_t>(range.mip_count),
                    static_cast<nri::Dim_t>(range.first_layer), static_cast<nri::Dim_t>(range.layer_count)
                };
                view.planes = range.aspects == 1 ? nri::PlaneBits::COLOR :
                    range.aspects == 2 ? nri::PlaneBits::DEPTH : nri::PlaneBits::STENCIL;
                status = core.CreateTextureView(view, descriptor);
            }
            else if (source.buffer)
            {
                const auto view = use.view.bufferView(source.buffer, source.offset, source.size);
                status = createGraphBufferView(session_, view, descriptor);
            }
            if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
            if (!descriptor) return cxx::unexpected(RenderError{kDeviceArgument});
            const auto owned = source.sampler ? nullptr : descriptor;
            core.DestroyDescriptor(std::exchange(record.views[i], owned));
            record.inputs[i] = source;
            const nri::UpdateDescriptorRangeDesc write{record.set, use.binding, use.element, &descriptor, 1};
            core.UpdateDescriptorRanges(&write, 1);
            changed = true;
        }
        return record.set;
    }

    RenderResult<void> RasterFramePreparation::growDescriptorPool() noexcept
    {
        const auto& core = session_.nriCore();
        auto required = graph_.bindingPlan().private_pool;
        if (auto counted = session_.descriptors().addSnapshotDescriptors(required); !counted) return counted;
        for (const auto& item : frame_.native.raster_bindings)
        {
            if (auto counted = rasterBindings(item.program).reserveSet(required); !counted)
                return cxx::unexpected(counted.error());
        }
        auto capacity = frame_.descriptor_capacity;
        capacity.flags |= required.flags;
        for (auto field : kDescriptorPoolCounts)
        {
            if (required.*field > capacity.*field)
            {
                const auto grown = std::max<std::uint64_t>(required.*field, std::uint64_t(capacity.*field) * 2);
                capacity.*field = static_cast<std::uint32_t>(std::min<std::uint64_t>(grown, UINT32_MAX));
            }
        }
        nri::DescriptorPool* pool = nullptr;
        auto status = core.CreateDescriptorPool(session_.device(), capacity, pool);
        if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
        cxx::scope_exit rollback([&]() noexcept { core.DestroyDescriptorPool(pool); });
        auto shared = session_.descriptors().createSnapshot(*pool);
        if (!shared) return cxx::unexpected(shared.error());
        std::vector<std::pair<nri::DescriptorSet**, nri::DescriptorSet*>> replacements;
        const auto allocate = [&](const auto& uses, PassDescriptorBindings native,
                                  nri::DescriptorSet*& destination, const auto& view) noexcept -> RenderResult<void>
        {
            nri::DescriptorSet* set = nullptr;
            const auto allocated = core.AllocateDescriptorSets(
                *pool, *native.layout, native.private_set, &set, 1, 0
            );
            if (allocated != nri::Result::SUCCESS) return cxx::unexpected(nativeError(allocated));
            for (std::size_t i = 0; i < uses.size(); ++i)
            {
                const auto& use = uses[i];
                auto* descriptor = view(i, use);
                if (!descriptor) continue;
                const nri::UpdateDescriptorRangeDesc write{set, use.binding, use.element, &descriptor, 1};
                core.UpdateDescriptorRanges(&write, 1);
            }
            replacements.emplace_back(&destination, set);
            return {};
        };
        for (auto& pass : frame_.passes)
        {
            const auto& program = graph_.bindingPlan().pass(pass.compiled_pass->binding_slot);
            if (!program.has_private_set) continue;
            const auto& native = graph_.programs()[pass.compiled_pass->binding_slot];
            auto ready = allocate(program.uses, native.descriptors, pass.private_descriptors,
                [&](std::size_t i, const auto& use) noexcept -> nri::Descriptor*
            {
                if (!program.bindings[use.field].private_binding) return nullptr;
                if (use.sampler != kGraphInvalidIndex) return graph_.programs().sampler(use.sampler);
                // Feature preparation precedes Instance::rebind. Dirty resources already
                // name the new allocation; the old cached view is not a valid replay source.
                if (frame_.bindings[use.resource].dirty) return static_cast<nri::Descriptor*>(nullptr);
                return pass.history_views[i] ? pass.history_views[i] : pass.private_views[i];
            });
            if (!ready) return cxx::unexpected(ready.error());
        }
        for (auto& item : frame_.native.raster_bindings)
        {
            const auto& program = rasterBindings(item.program);
            if (program.fields.empty()) continue;
            auto ready = allocate(program.fields, program.native, item.set,
                [&](std::size_t i, const auto&) noexcept -> nri::Descriptor*
            {
                return item.inputs[i].sampler ? item.inputs[i].sampler : item.views[i];
            });
            if (!ready) return cxx::unexpected(ready.error());
        }
        // Earlier Features may already have produced CPU packets for this same writable FIF.
        // Rewire those borrows before the old pool can retire; they have not been recorded yet.
        std::unordered_map<nri::DescriptorSet*, nri::DescriptorSet*> relocated;
        for (const auto& [destination, value] : replacements)
        {
            if (*destination) relocated.emplace(*destination, value);
            *destination = value;
        }
        const auto relocate = [&](RasterDrawList& list) noexcept
        {
            for (auto*& set : list.descriptors)
            {
                if (const auto found = relocated.find(set); found != relocated.end()) set = found->second;
            }
        };
        for (auto& list : frame_.raster_draws) relocate(list);
        frame_.invalidateRecordings();
        auto* old_pool = std::exchange(frame_.native.descriptor_pool, pool);
        frame_.descriptor_capacity = capacity;
        frame_.descriptor_reserved = required;
        if (session_.description().graphicsAPI == nri::GraphicsAPI::D3D12)
            frame_.shared_descriptors = *shared;
        rollback.release();
        if (old_pool)
        {
            session_.deferRelease(
                gpu_.accepted, [core = &core, old_pool]() noexcept { core->DestroyDescriptorPool(old_pool); }
            );
        }
        return {};
    }

} // namespace lux::render::detail
