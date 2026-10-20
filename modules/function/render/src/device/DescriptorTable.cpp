#include <bit>
#include <lux/engine/render/device/DescriptorTable.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/PipelineLayoutPlan.hpp>
#include <lux/engine/render/graph/Shader.hpp>

namespace lux::render::detail
{
    namespace
    {
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

    RenderResult<void> DescriptorTable::initialize(
        std::uint32_t image_capacity,
        std::uint32_t sampler_capacity
    ) noexcept
    {
        image_capacity_ = image_capacity;
        sampler_capacity_ = sampler_capacity;
        const auto ranges = descriptorRanges(sampledImageTableBindings(), image_capacity, sampler_capacity);
        const nri::DescriptorSetDesc set{
            0,
            ranges.data(),
            static_cast<std::uint32_t>(ranges.size()),
            nri::DescriptorSetBits::ALLOW_UPDATE_AFTER_SET
        };
        nri::PipelineLayoutDesc layout{};
        layout.descriptorSets = &set;
        layout.descriptorSetNum = 1;
        layout.shaderStages =
            nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER | nri::StageBits::COMPUTE_SHADER;
        layout.flags = nri::PipelineLayoutBits::IGNORE_GLOBAL_SPIRV_OFFSETS;
        const auto& nri_core = session_.nriCore();
        auto status = nri_core.CreatePipelineLayout(session_.device(), layout, allocation_layout_);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        nri::DescriptorPoolDesc pool{};
        pool.descriptorSetMaxNum = 1;
        pool.textureMaxNum = image_capacity;
        pool.samplerMaxNum = sampler_capacity;
        pool.flags = nri::DescriptorPoolBits::ALLOW_UPDATE_AFTER_SET;
        status = nri_core.CreateDescriptorPool(session_.device(), pool, pool_);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        status = nri_core.AllocateDescriptorSets(*pool_, *allocation_layout_, 0, &descriptors_, 1, 0);
        return status == nri::Result::SUCCESS ? RenderResult<void>{} : cxx::unexpected(nativeError(status));
    }

    DescriptorTable::~DescriptorTable() noexcept
    {
        clear();
    }

    void DescriptorTable::clear() noexcept
    {
        if (!slots_.empty())
        {
            std::terminate();
        }
        const auto& nri_core = session_.nriCore();
        if (pool_)
        {
            nri_core.DestroyDescriptorPool(std::exchange(pool_, nullptr));
        }
        if (allocation_layout_)
        {
            nri_core.DestroyPipelineLayout(std::exchange(allocation_layout_, nullptr));
        }
        for (const auto& sampler : samplers_)
        {
            nri_core.DestroyDescriptor(sampler.descriptor);
        }
        samplers_.clear();
        descriptors_ = nullptr;
        image_capacity_ = sampler_capacity_ = 0;
    }

    RenderResult<DescriptorTable::Slot> DescriptorTable::allocate() noexcept
    {
        if (slots_.size() == image_capacity_)
        {
            return cxx::unexpected(RenderError{kDeviceCapacity});
        }
        const auto slot = slots_.emplace();
        if (slot.index >= image_capacity_)
        {
            slots_.erase(slot);
            return cxx::unexpected(RenderError{kDeviceCapacity});
        }
        return slot;
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
        if (samplers_.size() == sampler_capacity_)
        {
            return cxx::unexpected(RenderError{kDeviceCapacity});
        }
        nri::Descriptor* descriptor = nullptr;
        const auto result = session_.nriCore().CreateSampler(session_.device(), description, descriptor);
        if (result != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        const auto index = static_cast<std::uint32_t>(samplers_.size());
        samplers_.push_back({description, descriptor});
        const nri::UpdateDescriptorRangeDesc update{descriptors_, 1, index, &descriptor, 1};
        session_.nriCore().UpdateDescriptorRanges(&update, 1);
        return index;
    }

    void DescriptorTable::write(Slot slot, nri::Descriptor* image) noexcept
    {
        const nri::UpdateDescriptorRangeDesc update{descriptors_, 0, slot.index, &image, 1};
        session_.nriCore().UpdateDescriptorRanges(&update, 1);
    }

    void DescriptorTable::release(Slot slot) noexcept
    {
        slots_.erase(slot);
    }
} // namespace lux::render::detail
