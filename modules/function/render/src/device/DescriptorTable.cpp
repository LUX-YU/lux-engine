#include <algorithm>
#include <bit>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>

namespace lux::render::detail
{
    namespace
    {
        constexpr auto table_types = std::array{
            nri::DescriptorType::TEXTURE,
            nri::DescriptorType::SAMPLER,
            nri::DescriptorType::STRUCTURED_BUFFER
        };

        std::size_t tableIndex(nri::DescriptorType type) noexcept
        {
            const auto found = std::ranges::find(table_types, type);
            if (found == table_types.end())
            {
                std::terminate();
            }
            return static_cast<std::size_t>(found - table_types.begin());
        }

        bool sameSampler(const nri::SamplerDesc& left, const nri::SamplerDesc& right) noexcept
        {
            const bool same_filters = left.filters.min == right.filters.min && left.filters.mag == right.filters.mag &&
                                      left.filters.mip == right.filters.mip && left.filters.op == right.filters.op;
            const bool same_lod = left.anisotropy == right.anisotropy && left.mipBias == right.mipBias &&
                                  left.mipMin == right.mipMin && left.mipMax == right.mipMax;
            const bool same_address = left.addressModes.u == right.addressModes.u &&
                                      left.addressModes.v == right.addressModes.v &&
                                      left.addressModes.w == right.addressModes.w;
            // Color has four scalar lanes and no padding; compare their representation without reading an inactive
            // union member.
            const bool same_border = std::bit_cast<std::array<std::uint32_t, 4>>(left.borderColor) ==
                                     std::bit_cast<std::array<std::uint32_t, 4>>(right.borderColor);
            return same_filters && same_lod && same_address && same_border && left.compareOp == right.compareOp &&
                   left.isInteger == right.isInteger && left.unnormalizedCoordinates == right.unnormalizedCoordinates;
        }
    } // namespace

    std::array<nri::DescriptorRangeDesc, 3>
    DescriptorTable::layoutRanges(const std::array<std::uint32_t, 3>& capacities, bool variable_count) noexcept
    {
        const auto stages =
            nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER | nri::StageBits::COMPUTE_SHADER;
        auto flags = nri::DescriptorRangeBits::ARRAY | nri::DescriptorRangeBits::PARTIALLY_BOUND |
                     nri::DescriptorRangeBits::ALLOW_UPDATE_AFTER_SET;
        if (variable_count)
        {
            flags |= nri::DescriptorRangeBits::VARIABLE_SIZED_ARRAY;
        }
        std::array<nri::DescriptorRangeDesc, 3> ranges;
        for (std::size_t i = 0; i < ranges.size(); ++i)
        {
            ranges[i] = {0, capacities[i], table_types[i], stages, flags};
        }
        return ranges;
    }

    RenderResult<void> DescriptorTable::initialize(const DeviceOptions& options, std::uint32_t pool_capacity) noexcept
    {
        pool_capacity_ = pool_capacity;
        const auto& limits = session_.description();
        const auto& enabled = session_.enabledFeatures();
        const auto maxima = std::array{
            std::min(
                limits.descriptorSet.updateAfterSet.textureMaxNum,
                limits.shaderStage.updateAfterSet.descriptorTextureMaxNum
            ),
            std::min(
                limits.descriptorSet.updateAfterSet.samplerMaxNum,
                limits.shaderStage.updateAfterSet.descriptorSamplerMaxNum
            ),
            enabled.buffer_descriptors ? std::min(
                                             limits.descriptorSet.updateAfterSet.storageBufferMaxNum,
                                             limits.shaderStage.updateAfterSet.descriptorStorageBufferMaxNum
                                         )
                                       : 0u
        };
        constexpr std::array kinds{
            EDescriptorLimit::SAMPLED_RESOURCE, EDescriptorLimit::SAMPLER, EDescriptorLimit::STORAGE_BUFFER
        };
        std::array<std::uint32_t, 3> capacities{};
        for (std::size_t i = 0; i < capacities.size(); ++i)
        {
            if (maxima[i] == 0 && i == 2)
            {
                continue;
            }
            const auto reserve = options.private_descriptor_reserve[i];
            const bool invalid_reserve = reserve >= maxima[i];
            const auto available = invalid_reserve ? 0 : maxima[i] - reserve;
            const auto requested = options.descriptor_layout_limits[i];
            const bool invalid_layout = requested > available;
            const bool empty_allocation = options.descriptor_initial_counts[i] == 0;
            if (invalid_reserve)
                return cxx::unexpected(descriptorLimit(kinds[i], std::uint64_t(reserve) + 1, maxima[i]));
            if (invalid_layout) return cxx::unexpected(descriptorLimit(kinds[i], requested, available));
            if (empty_allocation) return cxx::unexpected(RenderError{kDeviceArgument});
            capacities[i] = requested ? requested : available;
        }
        if (limits.graphicsAPI == nri::GraphicsAPI::D3D12)
        {
            const auto reserved =
                std::uint64_t(options.private_descriptor_reserve[0]) + options.private_descriptor_reserve[2];
            const auto budget = limits.shaderStage.resourceMaxNum;
            std::uint64_t committed = reserved;
            unsigned automatic = 0;
            for (auto kind : {0U, 2U})
            {
                if (options.descriptor_layout_limits[kind])
                {
                    committed += capacities[kind];
                }
                else if (capacities[kind])
                {
                    ++automatic;
                }
            }
            if (committed >= budget)
            {
                return cxx::unexpected(deviceBudget(EDeviceBudget::SHADER_HEAP, committed, budget));
            }
            auto remaining = static_cast<std::uint32_t>(budget - committed);
            for (auto kind : {0U, 2U})
            {
                if (options.descriptor_layout_limits[kind] || !capacities[kind])
                {
                    continue;
                }
                capacities[kind] = std::min(capacities[kind], remaining / automatic);
                --automatic;
                remaining -= capacities[kind];
                if (!capacities[kind])
                {
                    return cxx::unexpected(RenderError{kDeviceCapacity});
                }
            }
        }
        const auto ranges = layoutRanges(capacities, enabled.variable_descriptor_count);
        const auto& core = session_.nriCore();
        for (std::size_t i = 0; i < tables_.size(); ++i)
        {
            auto& table = tables_[i];
            table.layout_capacity = capacities[i];
            if (capacities[i] == 0)
            {
                continue;
            }
            const nri::DescriptorSetDesc set{0, &ranges[i], 1};
            nri::PipelineLayoutDesc layout{};
            layout.descriptorSets = &set;
            layout.descriptorSetNum = 1;
            layout.shaderStages = ranges[i].shaderStages;
            layout.flags = nri::PipelineLayoutBits::IGNORE_GLOBAL_SPIRV_OFFSETS;
            const auto status = core.CreatePipelineLayout(session_.device(), layout, table.allocation_layout);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
            auto ready = reserve(table_types[i], std::min(options.descriptor_initial_counts[i], capacities[i]));
            if (!ready)
            {
                return ready;
            }
        }
        return {};
    }

    DescriptorTable::~DescriptorTable() noexcept
    {
        clear();
    }

    void DescriptorTable::clear() noexcept
    {
        if (binding_pins_ != 0 || !pinned_pools_.empty())
        {
            std::terminate();
        }
        const auto& core = session_.nriCore();
        for (std::size_t i = 0; i < tables_.size(); ++i)
        {
            auto& table = tables_[i];
            if (i != 1 && !table.slots.empty())
            {
                std::terminate();
            }
            table.slots.clear();
            if (table.pool)
            {
                core.DestroyDescriptorPool(std::exchange(table.pool, nullptr));
                allocated_descriptors_ -= table.allocated_capacity;
            }
            if (table.allocation_layout)
            {
                core.DestroyPipelineLayout(std::exchange(table.allocation_layout, nullptr));
            }
            table.descriptors = nullptr;
            table.layout_capacity = table.allocated_capacity = 0;
        }
        if (allocated_descriptors_ != 0)
        {
            std::terminate(); // Retiring pools must have completed before close.
        }
        for (const auto& sampler : samplers_)
        {
            core.DestroyDescriptor(sampler.descriptor);
        }
        samplers_.clear();
        pool_capacity_ = 0;
    }

    void DescriptorTable::pinNativeBindings() noexcept
    {
        ++binding_pins_;
    }

    void DescriptorTable::unpinNativeBindings() noexcept
    {
        if (binding_pins_ == 0) std::terminate();
        if (--binding_pins_ != 0) return;
        for (const auto& pool : pinned_pools_) retirePool(pool);
        pinned_pools_.clear();
    }

    void DescriptorTable::retirePool(RetiredPool pool) noexcept
    {
        session_.deferRelease([this, pool]() noexcept
        {
            session_.nriCore().DestroyDescriptorPool(pool.pool);
            allocated_descriptors_ -= pool.capacity;
        });
    }

    RenderResult<void> DescriptorTable::reserve(nri::DescriptorType type, std::uint32_t required) noexcept
    {
        auto& table = tables_[tableIndex(type)];
        if (required <= table.allocated_capacity)
        {
            return {};
        }
        if (required > table.layout_capacity)
        {
            return cxx::unexpected(RenderError{kDeviceCapacity});
        }
        const auto grown = std::max<std::uint64_t>(required, std::uint64_t(table.allocated_capacity) * 2);
        const auto capacity = session_.enabledFeatures().variable_descriptor_count
                                  ? static_cast<std::uint32_t>(std::min<std::uint64_t>(grown, table.layout_capacity))
                                  : table.layout_capacity;
        if (allocated_descriptors_ + capacity > pool_capacity_)
        {
            return cxx::unexpected(
                deviceBudget(EDeviceBudget::DESCRIPTOR_POOL, allocated_descriptors_ + capacity, pool_capacity_)
            );
        }
        nri::DescriptorPoolDesc description{};
        description.descriptorSetMaxNum = 1;
        description.flags = nri::DescriptorPoolBits::ALLOW_UPDATE_AFTER_SET;
        if (session_.description().graphicsAPI == nri::GraphicsAPI::D3D12)
        {
            description.flags |= nri::DescriptorPoolBits::COPY_SOURCE;
        }
        switch (type)
        {
        case nri::DescriptorType::TEXTURE:
            description.textureMaxNum = capacity;
            break;
        case nri::DescriptorType::SAMPLER:
            description.samplerMaxNum = capacity;
            break;
        default:
            description.structuredBufferMaxNum = capacity;
            break;
        }
        const auto& core = session_.nriCore();
        nri::DescriptorPool* pool = nullptr;
        auto status = core.CreateDescriptorPool(session_.device(), description, pool);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        nri::DescriptorSet* descriptors = nullptr;
        const auto variable_count = session_.enabledFeatures().variable_descriptor_count ? capacity : 0u;
        status = core.AllocateDescriptorSets(*pool, *table.allocation_layout, 0, &descriptors, 1, variable_count);
        if (status != nri::Result::SUCCESS)
        {
            core.DestroyDescriptorPool(pool);
            return cxx::unexpected(nativeError(status));
        }
        std::vector<nri::UpdateDescriptorRangeDesc> writes;
        writes.reserve(table.slots.size());
        const auto& keys = table.slots.keys();
        const auto& values = table.slots.values();
        for (std::size_t i = 0; i < keys.size(); ++i)
        {
            if (values[i].descriptor)
            {
                writes.push_back({descriptors, 0, keys[i].index, &values[i].descriptor, 1});
            }
        }
        if (!writes.empty())
        {
            core.UpdateDescriptorRanges(writes.data(), static_cast<std::uint32_t>(writes.size()));
        }
        auto* old_pool = std::exchange(table.pool, pool);
        const auto old_capacity = std::exchange(table.allocated_capacity, capacity);
        table.descriptors = descriptors;
        ++binding_revision_;
        allocated_descriptors_ += capacity;
        if (old_pool)
        {
            const RetiredPool old{old_pool, old_capacity};
            if (binding_pins_) pinned_pools_.push_back(old);
            else retirePool(old);
        }
        return {};
    }

    RenderResult<DescriptorTable::Slot> DescriptorTable::allocate(nri::DescriptorType type) noexcept
    {
        auto& table = tables_[tableIndex(type)];
        if (table.slots.size() >= table.layout_capacity)
        {
            return cxx::unexpected(RenderError{kDeviceCapacity});
        }
        const auto slot = table.slots.emplace();
        if (slot.isNull())
        {
            return cxx::unexpected(RenderError{kDeviceCapacity});
        }
        if (slot.index >= table.layout_capacity)
        {
            table.slots.erase(slot);
            return cxx::unexpected(RenderError{kDeviceCapacity});
        }
        if (auto ready = reserve(type, slot.index + 1); !ready)
        {
            table.slots.erase(slot);
            return cxx::unexpected(ready.error());
        }
        return slot;
    }

    void DescriptorTable::write(nri::DescriptorType type, Slot slot, nri::Descriptor* descriptor) noexcept
    {
        auto& table = tables_[tableIndex(type)];
        auto* entry = table.slots.tryGet(slot);
        const bool invalid_slot = entry == nullptr;
        const bool replacing = !invalid_slot && entry->descriptor && entry->descriptor != descriptor;
        const bool invalid_write = invalid_slot || descriptor == nullptr || replacing;
        if (invalid_write)
        {
            std::terminate();
        }
        if (entry->descriptor == descriptor)
        {
            return;
        }
        const nri::UpdateDescriptorRangeDesc update{table.descriptors, 0, slot.index, &descriptor, 1};
        session_.nriCore().UpdateDescriptorRanges(&update, 1);
        entry->descriptor = descriptor;
        if (session_.description().graphicsAPI == nri::GraphicsAPI::D3D12)
        {
            ++binding_revision_;
        }
    }

    void DescriptorTable::release(nri::DescriptorType type, Slot slot) noexcept
    {
        if (!tables_[tableIndex(type)].slots.erase(slot))
        {
            std::terminate();
        }
    }

    nri::DescriptorSet* DescriptorTable::descriptors(nri::DescriptorType type) const noexcept
    {
        return tables_[tableIndex(type)].descriptors;
    }

    std::uint32_t DescriptorTable::capacity(nri::DescriptorType type) const noexcept
    {
        return tables_[tableIndex(type)].layout_capacity;
    }

    std::array<std::uint32_t, 3> DescriptorTable::snapshotCounts() const noexcept
    {
        if (session_.description().graphicsAPI != nri::GraphicsAPI::D3D12)
        {
            return {};
        }
        return {tables_[0].allocated_capacity, tables_[1].allocated_capacity, tables_[2].allocated_capacity};
    }

    RenderResult<void> DescriptorTable::addSnapshotDescriptors(nri::DescriptorPoolDesc& pool) const noexcept
    {
        const auto counts = snapshotCounts();
        constexpr auto fields = std::array{
            &nri::DescriptorPoolDesc::textureMaxNum,
            &nri::DescriptorPoolDesc::samplerMaxNum,
            &nri::DescriptorPoolDesc::structuredBufferMaxNum
        };
        for (std::size_t kind = 0; kind < counts.size(); ++kind)
        {
            if (!counts[kind])
            {
                continue;
            }
            const bool is_set_capacity_exhausted = pool.descriptorSetMaxNum == UINT32_MAX;
            const bool is_descriptor_capacity_exhausted = counts[kind] > UINT32_MAX - pool.*fields[kind];
            const bool is_capacity_exhausted = is_set_capacity_exhausted || is_descriptor_capacity_exhausted;
            if (is_capacity_exhausted)
            {
                return cxx::unexpected(RenderError{kDeviceCapacity});
            }
            ++pool.descriptorSetMaxNum;
            pool.*fields[kind] += counts[kind];
            pool.flags |= nri::DescriptorPoolBits::ALLOW_UPDATE_AFTER_SET;
        }
        return {};
    }

    RenderResult<DescriptorTable::Snapshot> DescriptorTable::createSnapshot(nri::DescriptorPool& pool) const noexcept
    {
        Snapshot result{};
        const auto counts = snapshotCounts();
        for (std::size_t kind = 0; kind < counts.size(); ++kind)
        {
            if (!counts[kind])
            {
                continue;
            }
            const auto status =
                session_.nriCore()
                    .AllocateDescriptorSets(pool, *tables_[kind].allocation_layout, 0, &result[kind], 1, counts[kind]);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
        }
        updateSnapshot(result);
        return result;
    }

    void DescriptorTable::updateSnapshot(const Snapshot& sets) const noexcept
    {
        for (std::size_t kind = 0; kind < sets.size(); ++kind)
        {
            if (!sets[kind])
            {
                continue;
            }
            const auto& keys = tables_[kind].slots.keys();
            const auto& entries = tables_[kind].slots.values();
            for (std::size_t index = 0; index < keys.size(); ++index)
            {
                if (!entries[index].descriptor)
                {
                    continue;
                }
                const nri::UpdateDescriptorRangeDesc
                    write{sets[kind], 0, keys[index].index, &entries[index].descriptor, 1};
                session_.nriCore().UpdateDescriptorRanges(&write, 1);
            }
        }
    }

    RenderResult<std::uint32_t> DescriptorTable::sampler(const nri::SamplerDesc& description) noexcept
    {
        for (std::uint32_t index = 0; index < samplers_.size(); ++index)
        {
            if (sameSampler(samplers_[index].description, description))
            {
                return index;
            }
        }
        const auto& table = tables_[1];
        const auto limit =
            table.layout_capacity ? table.layout_capacity : session_.description().descriptorSet.samplerMaxNum;
        if (samplers_.size() == limit)
        {
            return cxx::unexpected(RenderError{kDeviceCapacity});
        }
        nri::Descriptor* descriptor = nullptr;
        const auto status = session_.nriCore().CreateSampler(session_.device(), description, descriptor);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        const auto index = static_cast<std::uint32_t>(samplers_.size());
        if (table.layout_capacity)
        {
            auto slot = allocate(nri::DescriptorType::SAMPLER);
            if (!slot)
            {
                session_.nriCore().DestroyDescriptor(descriptor);
                return cxx::unexpected(slot.error());
            }
            if (slot->index != index)
            {
                std::terminate();
            }
            write(nri::DescriptorType::SAMPLER, *slot, descriptor);
        }
        samplers_.push_back({description, descriptor});
        return index;
    }

    RenderResult<nri::Descriptor*> DescriptorTable::sampler(const SamplerDescription& input) noexcept
    {
        const bool invalid = input.filter > ESamplerFilter::LINEAR ||
                             input.address > ESamplerAddress::CLAMP_TO_BORDER ||
                             (input.comparison && *input.comparison > EDepthCompare::ALWAYS);
        if (invalid)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        nri::SamplerDesc description{};
        description.filters.min = description.filters.mag =
            input.filter == ESamplerFilter::LINEAR ? nri::Filter::LINEAR : nri::Filter::NEAREST;
        description.filters.mip = nri::Filter::NEAREST;
        constexpr nri::AddressMode addresses[]{
            nri::AddressMode::REPEAT,
            nri::AddressMode::CLAMP_TO_EDGE,
            nri::AddressMode::CLAMP_TO_BORDER
        };
        description.addressModes = {
            addresses[static_cast<unsigned>(input.address)],
            addresses[static_cast<unsigned>(input.address)],
            addresses[static_cast<unsigned>(input.address)]
        };
        constexpr nri::CompareOp comparisons[]{
            nri::CompareOp::NEVER,
            nri::CompareOp::LESS,
            nri::CompareOp::EQUAL,
            nri::CompareOp::LESS_EQUAL,
            nri::CompareOp::GREATER,
            nri::CompareOp::NOT_EQUAL,
            nri::CompareOp::GREATER_EQUAL,
            nri::CompareOp::ALWAYS
        };
        description.compareOp =
            input.comparison ? comparisons[static_cast<unsigned>(*input.comparison)] : nri::CompareOp::NONE;
        if (input.address == ESamplerAddress::CLAMP_TO_BORDER)
        {
            description.borderColor.f = {input.border, input.border, input.border, input.border};
        }
        auto index = sampler(description);
        if (!index)
        {
            return cxx::unexpected(index.error());
        }
        return samplers_[*index].descriptor;
    }

} // namespace lux::render::detail
