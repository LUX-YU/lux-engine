#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
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

    RenderResult<void> FrameResources::prepareDescriptorCapacity(
        JobGpuResources& gpu, std::span<const RasterDrawInput> draws, bool resize_pool
    ) noexcept
    {
        nri::DescriptorPoolDesc additional{};
        for (const auto& stream : recordingGraph().programs().rasterStreams())
        {
            const auto p = stream.pass;
            if (stream.resource_list && !passEnabled(recordingGraph().passes()[p])) continue;
            const auto& input = draws[stream.list];
            auto& resident = raster_draws[stream.list];
            // A successfully published revision already owns every descriptor row for this draw layout.
            if (input.command_revision && resident.command_revision == input.command_revision) continue;
            resident.command_revision = 0;
            auto selected = gpu.programSlots(stream);
            if (!selected) return cxx::unexpected(selected.error());
            const auto& programs = *selected;
            // Resource-authored lists use pass-local ordinals in the publication selected for this Run.
            const auto revision = stream.resource_list ? 0 : selected->revision;
            auto& rows = stream_descriptor_rows[stream.list];
            rows.resize(programs.size());
            if (!input.data.fitsSlotRanges()) return cxx::unexpected(RenderError{kDeviceCapacity, {p}});
            for (auto& program : rows) program.used = 0;
            for (const auto& draw : input.data.draws)
            {
                const auto id = draw.program;
                const bool invalid = !stream.acceptsStream(id.stream) ||
                    id.index >= programs.size() || id.revision != revision;
                if (invalid) return cxx::unexpected(RenderError{kFrameContract, {p}});
                const auto& bindings = programs[id.index].bindings;
                const bool valid_ranges = draw.constants.fits(input.data.constants.size()) &&
                    draw.vertices.fits(input.data.vertices.size()) &&
                    draw.bindings.fits(input.data.bindings.size());
                const bool valid_constants = draw.constants.count == bindings.parameter_bytes &&
                    draw.constants.first % 4 == 0;
                const auto count = draw.bindings.count;
                const bool valid_abi = valid_constants && count == bindings.fields.size();
                if (!valid_ranges || !valid_abi) return cxx::unexpected(RenderError{kFrameContract, {p}});
                if (count > stream_binding_inputs.max_size())
                    return cxx::unexpected(RenderError{kDeviceCapacity});
                auto& by_program = rows[id.index];
                if (!by_program.select(draw.bindings.first) || bindings.fields.empty()) continue;
                const auto ordinal = by_program.used - 1;
                if (ordinal == by_program.records.size())
                {
                    if (native.raster_bindings.size() >= kGraphInvalidIndex)
                        return cxx::unexpected(RenderError{kDeviceCapacity});
                    by_program.records.push_back(static_cast<std::uint32_t>(native.raster_bindings.size()));
                    auto& record = native.raster_bindings.emplace_back();
                    record.layout = &bindings;
                    record.pass = p;
                    record.program = id.index;
                    record.list = stream.list;
                    record.ordinal = ordinal;
                    record.views.resize(bindings.fields.size());
                    record.inputs.resize(bindings.fields.size());
                }
                const auto index = by_program.records[ordinal];
                if (!native.raster_bindings[index].set)
                {
                    if (auto counted = bindings.reserveSet(additional); !counted) return counted;
                }
            }
        }
        if (resize_pool) return growDescriptorPool(gpu);
        if (!additional.descriptorSetMaxNum) return {};
        const bool fits = native.descriptor_pool &&
            std::ranges::all_of(kDescriptorPoolCounts, [&](auto field) noexcept
        {
            return additional.*field <= descriptor_capacity.*field - descriptor_reserved.*field;
        });
        return fits ? RenderResult<void>{} : growDescriptorPool(gpu);
    }

    RenderResult<void> FrameResources::updateDrawSet(
        DeviceSession& session, std::uint32_t index, std::span<const RasterDescriptorInput> inputs
    ) noexcept
    {
        auto& record = this->native.raster_bindings[index];
        const auto& bindings = *record.layout;
        const auto& core = session.nriCore();
        if (!record.set)
        {
            // Failed native allocation can consume pool quota. The next preflight counts that reservation.
            if (auto counted = bindings.reserveSet(descriptor_reserved); !counted)
                return cxx::unexpected(counted.error());
            nri::DescriptorSet* created = nullptr;
            const auto status = core.AllocateDescriptorSets(
                *this->native.descriptor_pool, *bindings.native.layout, bindings.native.private_set, &created, 1, 0
            );
            if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
            record.set = created;
        }
        bool changed = false;
        cxx::scope_exit invalidate([&]() noexcept
        {
            if (changed) invalidatePass(recordingGraph(), record.pass);
        });
        for (std::size_t i = 0; i < bindings.fields.size(); ++i)
        {
            const auto& use = bindings.fields[i];
            const auto& source = inputs[i];
            if (record.inputs[i] == source) continue;

            const bool uses_target = !source.target_view.isNull();
            auto* descriptor = uses_target ? session.descriptors().textureView(source.target_view) : source.sampler;
            auto status = nri::Result::SUCCESS;
            if (source.image)
            {
                const auto& range = source.range;
                const auto view = graphImageView(source.image, use.view.image_type, source.format, range);
                status = core.CreateTextureView(view, descriptor);
            }
            else if (source.buffer)
            {
                const auto view = use.view.bufferView(source.buffer, source.offset, source.size);
                status = createGraphBufferView(session, view, descriptor);
            }
            if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
            if (!descriptor) return cxx::unexpected(RenderError{kDeviceArgument});
            const auto owned = source.sampler || uses_target ? nullptr : descriptor;
            core.DestroyDescriptor(std::exchange(record.views[i], owned));
            record.inputs[i] = source;
            const nri::UpdateDescriptorRangeDesc write{record.set, use.binding, use.element, &descriptor, 1};
            core.UpdateDescriptorRanges(&write, 1);
            changed = true;
        }
        return {};
    }

    RenderResult<void> FrameResources::growDescriptorPool(JobGpuResources& gpu) noexcept
    {
        auto& session = gpu.session;
        const auto& core = session.nriCore();
        auto required = recordingGraph().bindingPlan().private_pool;
        if (auto counted = session.descriptors().addSnapshotDescriptors(required); !counted) return counted;
        for (const auto& item : this->native.raster_bindings)
        {
            if (auto counted = item.layout->reserveSet(required); !counted)
                return cxx::unexpected(counted.error());
        }
        auto capacity = descriptor_capacity;
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
        auto status = core.CreateDescriptorPool(session.device(), capacity, pool);
        if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
        cxx::scope_exit rollback([&]() noexcept { core.DestroyDescriptorPool(pool); });
        auto shared = session.descriptors().createSnapshot(*pool);
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
        for (auto& pass : passes)
        {
            const auto& program = recordingGraph().bindingPlan().pass(pass.compiled_pass->binding_slot);
            if (!program.has_private_set) continue;
            const auto& native = recordingGraph().programs()[pass.compiled_pass->binding_slot];
            auto ready = allocate(program.uses, native.descriptors, pass.private_descriptors,
                [&](std::size_t i, const auto& use) noexcept -> nri::Descriptor*
            {
                if (!program.bindings[use.field].private_binding) return nullptr;
                if (use.sampler != kGraphInvalidIndex) return recordingGraph().programs().sampler(use.sampler);
                // Feature preparation precedes Instance::rebind. Dirty resources already
                // name the new allocation; the old cached view is not a valid replay source.
                if (bindings[use.resource].dirty) return static_cast<nri::Descriptor*>(nullptr);
                return pass.history_views[i] ? pass.history_views[i] : pass.private_views[i];
            });
            if (!ready) return cxx::unexpected(ready.error());
        }
        for (auto& item : this->native.raster_bindings)
        {
            const auto& program = *item.layout;
            if (program.fields.empty()) continue;
            auto ready = allocate(program.fields, program.native, item.set,
                [&](std::size_t i, const auto&) noexcept -> nri::Descriptor*
            {
                const auto& input = item.inputs[i];
                if (!input.target_view.isNull()) return session.descriptors().textureView(input.target_view);
                return input.sampler ? input.sampler : item.views[i];
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
        for (auto& list : raster_draws)
        {
            for (auto& packet : list.packets)
            {
                if (!packet.descriptor_set) continue;
                const auto found = relocated.find(packet.descriptor_set);
                if (found != relocated.end()) packet.descriptor_set = found->second;
            }
        }
        invalidateRecordings();
        auto* old_pool = std::exchange(this->native.descriptor_pool, pool);
        descriptor_capacity = capacity;
        descriptor_reserved = required;
        if (session.description().graphicsAPI == nri::GraphicsAPI::D3D12)
            shared_descriptors = *shared;
        rollback.release();
        if (old_pool)
        {
            session.deferRelease(
                gpu.accepted, [core = &core, old_pool]() noexcept { core->DestroyDescriptorPool(old_pool); }
            );
        }
        return {};
    }

} // namespace lux::render::detail
