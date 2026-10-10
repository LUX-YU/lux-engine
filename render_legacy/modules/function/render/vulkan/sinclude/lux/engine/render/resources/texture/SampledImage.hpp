#pragma once

#include <lux/engine/render/gpu/lifecycle/DeviceObject.hpp>
#include <lux/engine/render/gpu/memory/VmaTypes.hpp>

#include <utility>

namespace lux::render
{
    // Native sampled-image ownership shared by creation, transfer and descriptor slots.
    // Dispose only before submission or at the original GPU completion/retirement boundary.
    struct SampledImage
    {
        VmaImage image;
        ImageViewOwner view;
        SamplerOwner sampler;
        VkFormat format{VK_FORMAT_UNDEFINED};
        uint32_t mip_levels{1};
        uint32_t array_layers{1};
        int width{}, height{};

        SampledImage() noexcept = default;
        SampledImage(const SampledImage&) = delete;
        SampledImage& operator=(const SampledImage&) = delete;
        SampledImage(SampledImage&&) noexcept = default;

        SampledImage& operator=(SampledImage&& other) noexcept
        {
            SampledImage previous(std::move(other));
            std::swap(image, previous.image);
            std::swap(view, previous.view);
            std::swap(sampler, previous.sampler);
            std::swap(format, previous.format);
            std::swap(mip_levels, previous.mip_levels);
            std::swap(array_layers, previous.array_layers);
            std::swap(width, previous.width);
            std::swap(height, previous.height);
            return *this;
        }
    };
} // namespace lux::render
