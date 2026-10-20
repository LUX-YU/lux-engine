#include <algorithm>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/DescriptorArray.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/LayoutPlan.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <numeric>
#include <utility>

namespace lux::render::detail
{
    RenderResult<std::uint32_t> DeviceSession::sampledImageCapacity() const noexcept
    {
        if (!enabled_features_.texture_descriptors)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        const auto maximum = bindless_capacity_;
        if (maximum == 0)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        return maximum;
    }

    RenderResult<DescriptorArray*> DeviceSession::sampledImageArray() noexcept
    {
        if (sampled_images_)
        {
            return sampled_images_.get();
        }
        auto capacity = sampledImageCapacity();
        if (!capacity)
        {
            return cxx::unexpected(capacity.error());
        }
        auto result = std::unique_ptr<DescriptorArray>(new DescriptorArray(*this, *capacity));
        if (auto ready = result->initialize(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        if (auto ready = result->initializeDefault(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        sampled_images_ = std::move(result);
        return sampled_images_.get();
    }

    RenderResult<void> DescriptorArray::initialize() noexcept
    {
        const auto ranges = descriptorRanges(sampledImageTableBindings(), capacity_);
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
        const auto& api = session_.api();
        auto status = api.CreatePipelineLayout(session_.device(), layout, allocation_layout_);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        nri::DescriptorPoolDesc pool{};
        pool.descriptorSetMaxNum = 1;
        pool.textureMaxNum = capacity_;
        pool.samplerMaxNum = capacity_;
        pool.flags = nri::DescriptorPoolBits::ALLOW_UPDATE_AFTER_SET;
        status = api.CreateDescriptorPool(session_.device(), pool, pool_);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        status = api.AllocateDescriptorSets(*pool_, *allocation_layout_, 0, &descriptors_, 1, 0);
        return status == nri::Result::SUCCESS ? RenderResult<void>{} : cxx::unexpected(nativeError(status));
    }

    DescriptorArray::~DescriptorArray() noexcept
    {
        if (!white_slot_.isNull())
        {
            slots_.erase(white_slot_);
        }
        if (!slots_.empty())
        {
            std::terminate();
        }
        const auto& api = session_.api();
        api.DestroyDescriptorPool(pool_);
        api.DestroyPipelineLayout(allocation_layout_);
        api.DestroyDescriptor(white_view_);
        api.DestroyDescriptor(white_sampler_);
        api.DestroyTexture(white_image_);
    }

    RenderResult<void> DescriptorArray::initializeDefault() noexcept
    {
        const auto& api = session_.api();
        const nri::TextureDesc description{
            nri::TextureType::TEXTURE_2D,
            nri::TextureUsageBits::SHADER_RESOURCE,
            nri::Format::RGBA8_UNORM,
            1,
            1,
            1,
            1,
            1,
            1,
            nri::SharingMode::CONCURRENT
        };
        auto status = api.CreatePlacedTexture(session_.device(), NriDeviceHeap, description, white_image_);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        const nri::TextureViewDesc view{white_image_, nri::TextureView::TEXTURE, description.format, 0, 1, 0, 1};
        status = api.CreateTextureView(view, white_view_);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        nri::SamplerDesc sampler{};
        sampler.addressModes =
            {nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE};
        status = api.CreateSampler(session_.device(), sampler, white_sampler_);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        white_slot_ = slots_.emplace();
        write(white_slot_, white_view_, white_sampler_);
        const auto& alignment = session_.description().memoryAlignment;
        const auto row_pitch = std::max(4u, alignment.uploadBufferTextureRow);
        const auto bytes = std::lcm(row_pitch, alignment.uploadBufferTextureSlice);
        nri::Buffer* staging = nullptr;
        const nri::BufferDesc buffer{bytes, 0, nri::BufferUsageBits::NONE};
        status = api.CreatePlacedBuffer(session_.device(), NriHostUploadHeap, buffer, staging);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        cxx::scope_exit rollback([&]() noexcept { api.DestroyBuffer(staging); });
        auto* mapped = api.MapBuffer(*staging, 0, bytes);
        if (!mapped)
        {
            return cxx::unexpected(nativeError(nri::Result::FAILURE));
        }
        std::memset(mapped, 255, 4);
        status = session_.vk().FlushBufferRangeVK(*staging, 0, 4);
        api.UnmapBuffer(*staging);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        auto submitted = session_.queue().recordAndSubmit(
            [&](nri::CommandBuffer& command, std::uint32_t) noexcept -> RenderResult<void>
            {
                nri::TextureBarrierDesc barrier{};
                barrier.texture = white_image_;
                barrier.mipNum = barrier.layerNum = 1;
                barrier.before = {nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::NONE};
                barrier
                    .after = {nri::AccessBits::COPY_DESTINATION, nri::Layout::COPY_DESTINATION, nri::StageBits::COPY};
                const nri::BarrierDesc dependency{nullptr, 0, nullptr, 0, &barrier, 1};
                api.CmdBarrier(command, dependency);
                const nri::TextureRegionDesc region{0, 0, 0, 1, 1, 1, 0, 0, nri::PlaneBits::COLOR};
                const nri::TextureDataLayoutDesc layout{0, row_pitch, bytes};
                api.CmdUploadBufferToTexture(command, *white_image_, region, *staging, layout);
                barrier.before = barrier.after;
                barrier.after = {nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::ALL};
                api.CmdBarrier(command, dependency);
                return {};
            }
        );
        if (!submitted)
        {
            return submitted;
        }
        rollback.release();
        session_.deferRelease(
            session_.queue(),
            [staging, destroy = api.DestroyBuffer]() noexcept { destroy(staging); }
        );
        return {};
    }

    RenderResult<cxx::SlotKey<DescriptorSlotTag>> DescriptorArray::allocate() noexcept
    {
        if (slots_.size() == capacity_)
        {
            return cxx::unexpected(RenderError{kDeviceCapacity});
        }
        const auto slot = slots_.emplace();
        if (slot.index >= capacity_)
        {
            slots_.erase(slot);
            return cxx::unexpected(RenderError{kDeviceCapacity});
        }
        return slot;
    }

    void DescriptorArray::write(Slot slot, nri::Descriptor* image, nri::Descriptor* sampler) noexcept
    {
        const nri::UpdateDescriptorRangeDesc updates[]{
            {descriptors_, 0, slot.index, &image, 1},
            {descriptors_, 1, slot.index, &sampler, 1}
        };
        session_.api().UpdateDescriptorRanges(updates, 2);
    }

    void DescriptorArray::release(Slot slot) noexcept
    {
        slots_.erase(slot);
    }
} // namespace lux::render::detail
