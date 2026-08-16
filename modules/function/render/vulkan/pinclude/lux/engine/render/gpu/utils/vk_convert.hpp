#pragma once
// Vulkan conversion utilities — render-thread only.
// Game-thread code must NEVER include this header.

#include <lux/engine/function/render/client/core/RenderTypes.hpp>
#include <vulkan/vulkan.h>

namespace lux::render::vk_convert
{
    // =========================================================================
    //  Viewport / Scissor / Extent
    // =========================================================================

    /// Convert Viewport → VkViewport.
    inline VkViewport toVk(const Viewport& v) noexcept
    {
        return { v.x, v.y, v.width, v.height, v.min_depth, v.max_depth };
    }

    /// Convert ScissorRect → VkRect2D.
    inline VkRect2D toVk(const ScissorRect& s) noexcept
    {
        return { { s.x, s.y }, { s.width, s.height } };
    }

    /// Convert common::Size2D → VkExtent2D.
    inline VkExtent2D toVk(const common::Size2D& e) noexcept
    {
        return { e.width, e.height };
    }

    /// Convert VkExtent2D → common::Size2D (for ingesting Vulkan query results).
    inline common::Size2D fromVk(VkExtent2D e) noexcept
    {
        return { e.width, e.height };
    }

    // =========================================================================
    //  Texture format
    // =========================================================================

    /// Convert ETextureFormatHint → VkFormat.
    inline VkFormat toVk(ETextureFormatHint f) noexcept
    {
        switch (f)
        {
        case ETextureFormatHint::RGBA8:   return VK_FORMAT_R8G8B8A8_UNORM;
        case ETextureFormatHint::SRGBA8:  return VK_FORMAT_R8G8B8A8_SRGB;
        case ETextureFormatHint::BGRA8:   return VK_FORMAT_B8G8R8A8_UNORM;
        case ETextureFormatHint::SBGRA8:  return VK_FORMAT_B8G8R8A8_SRGB;
        case ETextureFormatHint::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case ETextureFormatHint::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case ETextureFormatHint::Auto:    return VK_FORMAT_UNDEFINED;
        default:                      return VK_FORMAT_UNDEFINED;
        }
    }

    /// Convert VkFormat → ETextureFormatHint (for render-internal → cmd conversion).
    /// Returns ETextureFormatHint::Auto for unrecognised formats.
    inline ETextureFormatHint fromVk(VkFormat f) noexcept
    {
        switch (f)
        {
        case VK_FORMAT_R8G8B8A8_UNORM:        return ETextureFormatHint::RGBA8;
        case VK_FORMAT_R8G8B8A8_SRGB:         return ETextureFormatHint::SRGBA8;
        case VK_FORMAT_B8G8R8A8_UNORM:        return ETextureFormatHint::BGRA8;
        case VK_FORMAT_B8G8R8A8_SRGB:         return ETextureFormatHint::SBGRA8;
        case VK_FORMAT_R16G16B16A16_SFLOAT:   return ETextureFormatHint::RGBA16F;
        case VK_FORMAT_R32G32B32A32_SFLOAT:   return ETextureFormatHint::RGBA32F;
        default:                              return ETextureFormatHint::Auto;
        }
    }

    // =========================================================================
    //  Sampler
    // =========================================================================

    namespace detail
    {
        inline VkFilter toVkFilter(SamplerDesc::Filter f) noexcept
        {
            return f == SamplerDesc::Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        }
        inline SamplerDesc::Filter fromVkFilter(VkFilter f) noexcept
        {
            return f == VK_FILTER_LINEAR ? SamplerDesc::Filter::Linear
                                         : SamplerDesc::Filter::Nearest;
        }
        inline VkSamplerMipmapMode toVkMipmap(SamplerDesc::MipmapMode m) noexcept
        {
            return m == SamplerDesc::MipmapMode::Linear
                       ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                       : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        }
        inline SamplerDesc::MipmapMode fromVkMipmap(VkSamplerMipmapMode m) noexcept
        {
            return m == VK_SAMPLER_MIPMAP_MODE_LINEAR
                       ? SamplerDesc::MipmapMode::Linear
                       : SamplerDesc::MipmapMode::Nearest;
        }
        inline VkSamplerAddressMode toVkAddr(SamplerDesc::AddressMode a) noexcept
        {
            switch (a) {
            case SamplerDesc::AddressMode::Repeat:         return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case SamplerDesc::AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case SamplerDesc::AddressMode::ClampToEdge:    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case SamplerDesc::AddressMode::ClampToBorder:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            default:                                       return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            }
        }
        inline SamplerDesc::AddressMode fromVkAddr(VkSamplerAddressMode a) noexcept
        {
            switch (a) {
            case VK_SAMPLER_ADDRESS_MODE_REPEAT:          return SamplerDesc::AddressMode::Repeat;
            case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return SamplerDesc::AddressMode::MirroredRepeat;
            case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:   return SamplerDesc::AddressMode::ClampToEdge;
            case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER: return SamplerDesc::AddressMode::ClampToBorder;
            default:                                      return SamplerDesc::AddressMode::Repeat;
            }
        }
        // Both enums are declared in the same order as the Vulkan values they
        // mirror, so the mapping is an index cast rather than a switch. The
        // static_asserts below pin the two endpoints of each range; a reordered
        // or extended enum trips them instead of silently shifting every value.
        static_assert(static_cast<int>(SamplerDesc::CompareOp::Never)  == VK_COMPARE_OP_NEVER);
        static_assert(static_cast<int>(SamplerDesc::CompareOp::Always) == VK_COMPARE_OP_ALWAYS);
        static_assert(static_cast<int>(SamplerDesc::BorderColor::TransparentBlackFloat)
                      == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
        static_assert(static_cast<int>(SamplerDesc::BorderColor::OpaqueWhiteInt)
                      == VK_BORDER_COLOR_INT_OPAQUE_WHITE);
        // The endpoints alone would not catch two interior values swapping, and
        // these two carry shadow correctness: the wrong compare op flips every
        // depth test, the wrong border makes out-of-atlas lookups read shadowed.
        static_assert(static_cast<int>(SamplerDesc::CompareOp::LessOrEqual)
                      == VK_COMPARE_OP_LESS_OR_EQUAL);
        static_assert(static_cast<int>(SamplerDesc::BorderColor::OpaqueWhiteFloat)
                      == VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE);

        inline VkCompareOp toVkCompareOp(SamplerDesc::CompareOp v) noexcept
        { return static_cast<VkCompareOp>(v); }
        inline SamplerDesc::CompareOp fromVkCompareOp(VkCompareOp v) noexcept
        { return static_cast<SamplerDesc::CompareOp>(v); }

        inline VkBorderColor toVkBorderColor(SamplerDesc::BorderColor v) noexcept
        { return static_cast<VkBorderColor>(v); }
        inline SamplerDesc::BorderColor fromVkBorderColor(VkBorderColor v) noexcept
        { return static_cast<SamplerDesc::BorderColor>(v); }
    } // namespace detail

    /// Convert SamplerDesc → VkSamplerCreateInfo.
    inline VkSamplerCreateInfo toVk(const SamplerDesc& d) noexcept
    {
        VkSamplerCreateInfo ci{};
        ci.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ci.magFilter               = detail::toVkFilter(d.mag_filter);
        ci.minFilter               = detail::toVkFilter(d.min_filter);
        ci.mipmapMode              = detail::toVkMipmap(d.mipmap_mode);
        ci.addressModeU            = detail::toVkAddr(d.address_u);
        ci.addressModeV            = detail::toVkAddr(d.address_v);
        ci.addressModeW            = detail::toVkAddr(d.address_w);
        ci.maxAnisotropy           = d.max_anisotropy;
        ci.anisotropyEnable        = d.anisotropy_enable ? VK_TRUE : VK_FALSE;
        ci.compareEnable           = d.compare_enable    ? VK_TRUE : VK_FALSE;
        ci.compareOp               = detail::toVkCompareOp(d.compare_op);
        ci.borderColor             = detail::toVkBorderColor(d.border_color);
        ci.unnormalizedCoordinates = d.unnormalized_coordinates ? VK_TRUE : VK_FALSE;
        ci.minLod                  = d.min_lod;
        ci.maxLod                  = d.max_lod;
        ci.mipLodBias              = d.mip_lod_bias;
        return ci;
    }

    /// Convert VkSamplerCreateInfo → SamplerDesc.
    inline SamplerDesc fromVk(const VkSamplerCreateInfo& sci) noexcept
    {
        SamplerDesc d;
        d.mag_filter                = detail::fromVkFilter(sci.magFilter);
        d.min_filter                = detail::fromVkFilter(sci.minFilter);
        d.mipmap_mode               = detail::fromVkMipmap(sci.mipmapMode);
        d.address_u                 = detail::fromVkAddr(sci.addressModeU);
        d.address_v                 = detail::fromVkAddr(sci.addressModeV);
        d.address_w                 = detail::fromVkAddr(sci.addressModeW);
        d.max_anisotropy            = sci.maxAnisotropy;
        d.anisotropy_enable         = sci.anisotropyEnable != VK_FALSE;
        d.compare_enable            = sci.compareEnable    != VK_FALSE;
        d.compare_op                = detail::fromVkCompareOp(sci.compareOp);
        d.border_color              = detail::fromVkBorderColor(sci.borderColor);
        d.unnormalized_coordinates  = sci.unnormalizedCoordinates != VK_FALSE;
        d.min_lod                   = sci.minLod;
        d.max_lod                   = sci.maxLod;
        d.mip_lod_bias              = sci.mipLodBias;
        return d;
    }

} // namespace lux::render::vk_convert
