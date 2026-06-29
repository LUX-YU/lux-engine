#pragma once
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGTransientDSTypes.hpp>   // EDescriptorType, EImageLayout
#include <lux/engine/render/core/VulkanContext.hpp>
#include <cassert>

namespace lux::render
{
	inline VkFormat convertTextureFormat(lux::common::ETextureFormat format)
	{
		switch (format)
		{
			case lux::common::ETextureFormat::R8_UNORM:          return VK_FORMAT_R8_UNORM;
			case lux::common::ETextureFormat::R8_SNORM:          return VK_FORMAT_R8_SNORM;
			case lux::common::ETextureFormat::R8_UINT:           return VK_FORMAT_R8_UINT;
			case lux::common::ETextureFormat::R8_SINT:           return VK_FORMAT_R8_SINT;
			case lux::common::ETextureFormat::R8_SRGB:           return VK_FORMAT_R8_SRGB;
			case lux::common::ETextureFormat::R16_UNORM:         return VK_FORMAT_R16_UNORM;
			case lux::common::ETextureFormat::R16_SNORM:         return VK_FORMAT_R16_SNORM;
			case lux::common::ETextureFormat::R16_UINT:          return VK_FORMAT_R16_UINT;
			case lux::common::ETextureFormat::R16_SINT:          return VK_FORMAT_R16_SINT;
			case lux::common::ETextureFormat::R16_SFLOAT:        return VK_FORMAT_R16_SFLOAT;
			case lux::common::ETextureFormat::R32_UINT:          return VK_FORMAT_R32_UINT;
			case lux::common::ETextureFormat::R32_SINT:          return VK_FORMAT_R32_SINT;
			case lux::common::ETextureFormat::R32_SFLOAT:        return VK_FORMAT_R32_SFLOAT;
			case lux::common::ETextureFormat::RG8_UNORM:         return VK_FORMAT_R8G8_UNORM;
			case lux::common::ETextureFormat::RG8_SNORM:         return VK_FORMAT_R8G8_SNORM;
			case lux::common::ETextureFormat::RG8_UINT:          return VK_FORMAT_R8G8_UINT;
			case lux::common::ETextureFormat::RG8_SINT:          return VK_FORMAT_R8G8_SINT;
			case lux::common::ETextureFormat::RG8_SRGB:          return VK_FORMAT_R8G8_SRGB;
			case lux::common::ETextureFormat::RG16_UNORM:        return VK_FORMAT_R16G16_UNORM;
			case lux::common::ETextureFormat::RG16_SNORM:        return VK_FORMAT_R16G16_SNORM;
			case lux::common::ETextureFormat::RG16_UINT:         return VK_FORMAT_R16G16_UINT;
			case lux::common::ETextureFormat::RG16_SINT:         return VK_FORMAT_R16G16_SINT;
			case lux::common::ETextureFormat::RG16_SFLOAT:       return VK_FORMAT_R16G16_SFLOAT;
			case lux::common::ETextureFormat::RG32_UINT:         return VK_FORMAT_R32G32_UINT;
			case lux::common::ETextureFormat::RG32_SINT:         return VK_FORMAT_R32G32_SINT;
			case lux::common::ETextureFormat::RG32_SFLOAT:       return VK_FORMAT_R32G32_SFLOAT;
			case lux::common::ETextureFormat::RGB8_UNORM:        return VK_FORMAT_R8G8B8_UNORM;
			case lux::common::ETextureFormat::RGB8_SNORM:        return VK_FORMAT_R8G8B8_SNORM;
			case lux::common::ETextureFormat::RGB8_UINT:         return VK_FORMAT_R8G8B8_UINT;
			case lux::common::ETextureFormat::RGB8_SINT:         return VK_FORMAT_R8G8B8_SINT;
			case lux::common::ETextureFormat::RGB8_SRGB:         return VK_FORMAT_R8G8B8_SRGB;
			case lux::common::ETextureFormat::RGB32_UINT:        return VK_FORMAT_R32G32B32_UINT;
			case lux::common::ETextureFormat::RGB32_SINT:        return VK_FORMAT_R32G32B32_SINT;
			case lux::common::ETextureFormat::RGB32_SFLOAT:      return VK_FORMAT_R32G32B32_SFLOAT;
			case lux::common::ETextureFormat::RGBA8_UNORM:       return VK_FORMAT_R8G8B8A8_UNORM;
			case lux::common::ETextureFormat::RGBA8_SNORM:       return VK_FORMAT_R8G8B8A8_SNORM;
			case lux::common::ETextureFormat::RGBA8_UINT:        return VK_FORMAT_R8G8B8A8_UINT;
			case lux::common::ETextureFormat::RGBA8_SINT:        return VK_FORMAT_R8G8B8A8_SINT;
			case lux::common::ETextureFormat::RGBA8_SRGB:        return VK_FORMAT_R8G8B8A8_SRGB;
			case lux::common::ETextureFormat::RGBA16_UNORM:      return VK_FORMAT_R16G16B16A16_UNORM;
			case lux::common::ETextureFormat::RGBA16_SNORM:      return VK_FORMAT_R16G16B16A16_SNORM;
			case lux::common::ETextureFormat::RGBA16_UINT:       return VK_FORMAT_R16G16B16A16_UINT;
			case lux::common::ETextureFormat::RGBA16_SINT:       return VK_FORMAT_R16G16B16A16_SINT;
			case lux::common::ETextureFormat::RGBA16_SFLOAT:     return VK_FORMAT_R16G16B16A16_SFLOAT;
			case lux::common::ETextureFormat::RGBA32_UINT:       return VK_FORMAT_R32G32B32A32_UINT;
			case lux::common::ETextureFormat::RGBA32_SINT:       return VK_FORMAT_R32G32B32A32_SINT;
			case lux::common::ETextureFormat::RGBA32_SFLOAT:     return VK_FORMAT_R32G32B32A32_SFLOAT;
			case lux::common::ETextureFormat::BGRA8_UNORM:       return VK_FORMAT_B8G8R8A8_UNORM;
			case lux::common::ETextureFormat::BGRA8_SRGB:        return VK_FORMAT_B8G8R8A8_SRGB;
			case lux::common::ETextureFormat::D16_UNORM:         return VK_FORMAT_D16_UNORM;
			case lux::common::ETextureFormat::D32_SFLOAT:        return VK_FORMAT_D32_SFLOAT;
			case lux::common::ETextureFormat::D16_UNORM_S8_UINT: return VK_FORMAT_D16_UNORM_S8_UINT;
			case lux::common::ETextureFormat::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
			case lux::common::ETextureFormat::D32_SFLOAT_S8_UINT:return VK_FORMAT_D32_SFLOAT_S8_UINT;
			case lux::common::ETextureFormat::BC1_RGB_UNORM:     return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
			case lux::common::ETextureFormat::BC1_RGB_SRGB:      return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
			case lux::common::ETextureFormat::BC1_RGBA_UNORM:    return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
			case lux::common::ETextureFormat::BC1_RGBA_SRGB:     return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
			case lux::common::ETextureFormat::BC2_UNORM:         return VK_FORMAT_BC2_UNORM_BLOCK;
			case lux::common::ETextureFormat::BC2_SRGB:          return VK_FORMAT_BC2_SRGB_BLOCK;
			case lux::common::ETextureFormat::BC3_UNORM:         return VK_FORMAT_BC3_UNORM_BLOCK;
			case lux::common::ETextureFormat::BC3_SRGB:          return VK_FORMAT_BC3_SRGB_BLOCK;
			case lux::common::ETextureFormat::BC4_UNORM:         return VK_FORMAT_BC4_UNORM_BLOCK;
			case lux::common::ETextureFormat::BC4_SNORM:         return VK_FORMAT_BC4_SNORM_BLOCK;
			case lux::common::ETextureFormat::BC5_UNORM:         return VK_FORMAT_BC5_UNORM_BLOCK;
			case lux::common::ETextureFormat::BC5_SNORM:         return VK_FORMAT_BC5_SNORM_BLOCK;
			case lux::common::ETextureFormat::BC6H_UFLOAT:       return VK_FORMAT_BC6H_UFLOAT_BLOCK;
			case lux::common::ETextureFormat::BC6H_SFLOAT:       return VK_FORMAT_BC6H_SFLOAT_BLOCK;
			case lux::common::ETextureFormat::BC7_UNORM:         return VK_FORMAT_BC7_UNORM_BLOCK;
			case lux::common::ETextureFormat::BC7_SRGB:          return VK_FORMAT_BC7_SRGB_BLOCK;
			case lux::common::ETextureFormat::UNDEFINED:
				return VK_FORMAT_UNDEFINED;
			default:
				assert(false && "Unknown lux::common::ETextureFormat");
				return VK_FORMAT_UNDEFINED;
		}
	}

	inline VkImageType convertTextureDimension(lux::common::ETextureDimension dimension, VkImageCreateFlags& flags)
	{
		flags = 0;
		switch (dimension)
		{
			case lux::common::ETextureDimension::TEX_2D:
			case lux::common::ETextureDimension::TEX_2D_ARRAY:
				return VK_IMAGE_TYPE_2D;
			case lux::common::ETextureDimension::TEX_3D:
				return VK_IMAGE_TYPE_3D;
			case lux::common::ETextureDimension::CUBE:
				flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
				return VK_IMAGE_TYPE_2D;
			default:
				return VK_IMAGE_TYPE_2D;
		}
	}

	inline VkImageUsageFlags convertTextureUsage(uint32_t usage)
	{
		VkImageUsageFlags flags = 0;
		if (usage & static_cast<uint32_t>(ERGTextureUsageBits::COLOR_ATTACHMENT))
			flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		if (usage & static_cast<uint32_t>(ERGTextureUsageBits::DEPTH_STENCIL))
			flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		if (usage & static_cast<uint32_t>(ERGTextureUsageBits::SAMPLED))
			flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
		if (usage & static_cast<uint32_t>(ERGTextureUsageBits::STORAGE))
			flags |= VK_IMAGE_USAGE_STORAGE_BIT;
		if (usage & static_cast<uint32_t>(ERGTextureUsageBits::TRANSFER_SRC))
			flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		if (usage & static_cast<uint32_t>(ERGTextureUsageBits::TRANSFER_DST))
			flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		return flags;
	}

	inline VkBufferUsageFlags convertBufferUsage(uint32_t usage)
	{
		VkBufferUsageFlags flags = 0;
		if (usage & ERGBufferUsageBits::VERTEX)
			flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		if (usage & ERGBufferUsageBits::INDEX)
			flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		if (usage & ERGBufferUsageBits::UNIFORM)
			flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		if (usage & ERGBufferUsageBits::STORAGE)
			flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		if (usage & ERGBufferUsageBits::INDIRECT)
			flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
		if (usage & ERGBufferUsageBits::TRANSFER_SRC)
			flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		if (usage & ERGBufferUsageBits::TRANSFER_DST)
			flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		if (usage & ERGBufferUsageBits::ACCEL_STRUCT)
			flags |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
		if (usage & ERGBufferUsageBits::SHADER_BINDING)
			flags |= VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;
		return flags;
	}

	// Neutral -> Vulkan: transient descriptor-set write enums (SDK three-tier).
	inline VkDescriptorType convertDescriptorType(EDescriptorType t)
	{
		switch (t)
		{
			case EDescriptorType::SAMPLER:                 return VK_DESCRIPTOR_TYPE_SAMPLER;
			case EDescriptorType::COMBINED_IMAGE_SAMPLER:  return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			case EDescriptorType::SAMPLED_IMAGE:           return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			case EDescriptorType::STORAGE_IMAGE:           return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			case EDescriptorType::UNIFORM_BUFFER:          return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			case EDescriptorType::STORAGE_BUFFER:          return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			case EDescriptorType::UNIFORM_BUFFER_DYNAMIC:  return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
			case EDescriptorType::STORAGE_BUFFER_DYNAMIC:  return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
			case EDescriptorType::INPUT_ATTACHMENT:        return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
		}
		return VK_DESCRIPTOR_TYPE_SAMPLER;
	}

	inline VkImageLayout convertImageLayout(EImageLayout l)
	{
		switch (l)
		{
			case EImageLayout::UNDEFINED:                       return VK_IMAGE_LAYOUT_UNDEFINED;
			case EImageLayout::GENERAL:                         return VK_IMAGE_LAYOUT_GENERAL;
			case EImageLayout::SHADER_READ_ONLY_OPTIMAL:        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			case EImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		}
		return VK_IMAGE_LAYOUT_UNDEFINED;
	}

	/// Vulkan -> neutral image layout (the compiler derives a Vk barrier layout and
	/// stores it back into the neutral RGDescriptorWrite::image_layout).
	inline EImageLayout convertVkImageLayout(VkImageLayout l)
	{
		switch (l)
		{
			case VK_IMAGE_LAYOUT_GENERAL:                         return EImageLayout::GENERAL;
			case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:        return EImageLayout::SHADER_READ_ONLY_OPTIMAL;
			case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return EImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL;
			default:                                              return EImageLayout::UNDEFINED;
		}
	}

	// =====================================================================
	// Reverse conversion: convert from Vulkan types back to RenderGraph types
	// =====================================================================


	inline lux::common::ETextureFormat convertVkFormat(VkFormat format)
	{
		switch (format)
		{
			case VK_FORMAT_R8_UNORM:                  return lux::common::ETextureFormat::R8_UNORM;
			case VK_FORMAT_R8_SNORM:                  return lux::common::ETextureFormat::R8_SNORM;
			case VK_FORMAT_R8_UINT:                   return lux::common::ETextureFormat::R8_UINT;
			case VK_FORMAT_R8_SINT:                   return lux::common::ETextureFormat::R8_SINT;
			case VK_FORMAT_R8_SRGB:                   return lux::common::ETextureFormat::R8_SRGB;
			case VK_FORMAT_R16_UNORM:                 return lux::common::ETextureFormat::R16_UNORM;
			case VK_FORMAT_R16_SNORM:                 return lux::common::ETextureFormat::R16_SNORM;
			case VK_FORMAT_R16_UINT:                  return lux::common::ETextureFormat::R16_UINT;
			case VK_FORMAT_R16_SINT:                  return lux::common::ETextureFormat::R16_SINT;
			case VK_FORMAT_R16_SFLOAT:                return lux::common::ETextureFormat::R16_SFLOAT;
			case VK_FORMAT_R32_UINT:                  return lux::common::ETextureFormat::R32_UINT;
			case VK_FORMAT_R32_SINT:                  return lux::common::ETextureFormat::R32_SINT;
			case VK_FORMAT_R32_SFLOAT:                return lux::common::ETextureFormat::R32_SFLOAT;
			case VK_FORMAT_R8G8_UNORM:                return lux::common::ETextureFormat::RG8_UNORM;
			case VK_FORMAT_R8G8_SNORM:                return lux::common::ETextureFormat::RG8_SNORM;
			case VK_FORMAT_R8G8_UINT:                 return lux::common::ETextureFormat::RG8_UINT;
			case VK_FORMAT_R8G8_SINT:                 return lux::common::ETextureFormat::RG8_SINT;
			case VK_FORMAT_R8G8_SRGB:                 return lux::common::ETextureFormat::RG8_SRGB;
			case VK_FORMAT_R16G16_UNORM:              return lux::common::ETextureFormat::RG16_UNORM;
			case VK_FORMAT_R16G16_SNORM:              return lux::common::ETextureFormat::RG16_SNORM;
			case VK_FORMAT_R16G16_UINT:               return lux::common::ETextureFormat::RG16_UINT;
			case VK_FORMAT_R16G16_SINT:               return lux::common::ETextureFormat::RG16_SINT;
			case VK_FORMAT_R16G16_SFLOAT:             return lux::common::ETextureFormat::RG16_SFLOAT;
			case VK_FORMAT_R32G32_UINT:               return lux::common::ETextureFormat::RG32_UINT;
			case VK_FORMAT_R32G32_SINT:               return lux::common::ETextureFormat::RG32_SINT;
			case VK_FORMAT_R32G32_SFLOAT:             return lux::common::ETextureFormat::RG32_SFLOAT;
			case VK_FORMAT_R8G8B8_UNORM:              return lux::common::ETextureFormat::RGB8_UNORM;
			case VK_FORMAT_R8G8B8_SNORM:              return lux::common::ETextureFormat::RGB8_SNORM;
			case VK_FORMAT_R8G8B8_UINT:               return lux::common::ETextureFormat::RGB8_UINT;
			case VK_FORMAT_R8G8B8_SINT:               return lux::common::ETextureFormat::RGB8_SINT;
			case VK_FORMAT_R8G8B8_SRGB:               return lux::common::ETextureFormat::RGB8_SRGB;
			case VK_FORMAT_R32G32B32_UINT:            return lux::common::ETextureFormat::RGB32_UINT;
			case VK_FORMAT_R32G32B32_SINT:            return lux::common::ETextureFormat::RGB32_SINT;
			case VK_FORMAT_R32G32B32_SFLOAT:          return lux::common::ETextureFormat::RGB32_SFLOAT;
			case VK_FORMAT_R8G8B8A8_UNORM:            return lux::common::ETextureFormat::RGBA8_UNORM;
			case VK_FORMAT_R8G8B8A8_SNORM:            return lux::common::ETextureFormat::RGBA8_SNORM;
			case VK_FORMAT_R8G8B8A8_UINT:             return lux::common::ETextureFormat::RGBA8_UINT;
			case VK_FORMAT_R8G8B8A8_SINT:             return lux::common::ETextureFormat::RGBA8_SINT;
			case VK_FORMAT_R8G8B8A8_SRGB:             return lux::common::ETextureFormat::RGBA8_SRGB;
			case VK_FORMAT_R16G16B16A16_UNORM:        return lux::common::ETextureFormat::RGBA16_UNORM;
			case VK_FORMAT_R16G16B16A16_SNORM:        return lux::common::ETextureFormat::RGBA16_SNORM;
			case VK_FORMAT_R16G16B16A16_UINT:         return lux::common::ETextureFormat::RGBA16_UINT;
			case VK_FORMAT_R16G16B16A16_SINT:         return lux::common::ETextureFormat::RGBA16_SINT;
			case VK_FORMAT_R16G16B16A16_SFLOAT:       return lux::common::ETextureFormat::RGBA16_SFLOAT;
			case VK_FORMAT_R32G32B32A32_UINT:         return lux::common::ETextureFormat::RGBA32_UINT;
			case VK_FORMAT_R32G32B32A32_SINT:         return lux::common::ETextureFormat::RGBA32_SINT;
			case VK_FORMAT_R32G32B32A32_SFLOAT:       return lux::common::ETextureFormat::RGBA32_SFLOAT;
			case VK_FORMAT_B8G8R8A8_UNORM:            return lux::common::ETextureFormat::BGRA8_UNORM;
			case VK_FORMAT_B8G8R8A8_SRGB:             return lux::common::ETextureFormat::BGRA8_SRGB;
			case VK_FORMAT_D16_UNORM:                 return lux::common::ETextureFormat::D16_UNORM;
			case VK_FORMAT_D32_SFLOAT:                return lux::common::ETextureFormat::D32_SFLOAT;
			case VK_FORMAT_D16_UNORM_S8_UINT:         return lux::common::ETextureFormat::D16_UNORM_S8_UINT;
			case VK_FORMAT_D24_UNORM_S8_UINT:         return lux::common::ETextureFormat::D24_UNORM_S8_UINT;
			case VK_FORMAT_D32_SFLOAT_S8_UINT:        return lux::common::ETextureFormat::D32_SFLOAT_S8_UINT;
			case VK_FORMAT_BC1_RGB_UNORM_BLOCK:       return lux::common::ETextureFormat::BC1_RGB_UNORM;
			case VK_FORMAT_BC1_RGB_SRGB_BLOCK:        return lux::common::ETextureFormat::BC1_RGB_SRGB;
			case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:      return lux::common::ETextureFormat::BC1_RGBA_UNORM;
			case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:       return lux::common::ETextureFormat::BC1_RGBA_SRGB;
			case VK_FORMAT_BC2_UNORM_BLOCK:           return lux::common::ETextureFormat::BC2_UNORM;
			case VK_FORMAT_BC2_SRGB_BLOCK:            return lux::common::ETextureFormat::BC2_SRGB;
			case VK_FORMAT_BC3_UNORM_BLOCK:           return lux::common::ETextureFormat::BC3_UNORM;
			case VK_FORMAT_BC3_SRGB_BLOCK:            return lux::common::ETextureFormat::BC3_SRGB;
			case VK_FORMAT_BC4_UNORM_BLOCK:           return lux::common::ETextureFormat::BC4_UNORM;
			case VK_FORMAT_BC4_SNORM_BLOCK:           return lux::common::ETextureFormat::BC4_SNORM;
			case VK_FORMAT_BC5_UNORM_BLOCK:           return lux::common::ETextureFormat::BC5_UNORM;
			case VK_FORMAT_BC5_SNORM_BLOCK:           return lux::common::ETextureFormat::BC5_SNORM;
			case VK_FORMAT_BC6H_UFLOAT_BLOCK:         return lux::common::ETextureFormat::BC6H_UFLOAT;
			case VK_FORMAT_BC6H_SFLOAT_BLOCK:         return lux::common::ETextureFormat::BC6H_SFLOAT;
			case VK_FORMAT_BC7_UNORM_BLOCK:           return lux::common::ETextureFormat::BC7_UNORM;
			case VK_FORMAT_BC7_SRGB_BLOCK:            return lux::common::ETextureFormat::BC7_SRGB;
			case VK_FORMAT_UNDEFINED:
			default:
				return lux::common::ETextureFormat::UNDEFINED;
		}
	}

	inline lux::common::ETextureDimension convertVkImageType(VkImageType imageType, VkImageCreateFlags flags)
	{
		if (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)
		{
			return lux::common::ETextureDimension::CUBE;
		}

		switch (imageType)
		{
			case VK_IMAGE_TYPE_2D:
				return lux::common::ETextureDimension::TEX_2D;
			case VK_IMAGE_TYPE_3D:
				return lux::common::ETextureDimension::TEX_3D;
			default:
				return lux::common::ETextureDimension::TEX_2D;
		}
	}

	inline ERGTextureUsageFlags convertVkImageUsage(VkImageUsageFlags flags)
	{
		ERGTextureUsageFlags usage = 0;
		if (flags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
			usage |= static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::COLOR_ATTACHMENT);
		if (flags & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
			usage |= static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::DEPTH_STENCIL);
		if (flags & VK_IMAGE_USAGE_SAMPLED_BIT)
			usage |= static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::SAMPLED);
		if (flags & VK_IMAGE_USAGE_STORAGE_BIT)
			usage |= static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::STORAGE);
		if (flags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
			usage |= static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::TRANSFER_SRC);
		if (flags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)
			usage |= static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::TRANSFER_DST);
		return usage;
	}

	inline ERGBufferUsageFlags convertVkBufferUsage(VkBufferUsageFlags flags)
	{
		ERGBufferUsageFlags usage = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::NONE);
		if (flags & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
			usage |= ERGBufferUsageBits::VERTEX;
		if (flags & VK_BUFFER_USAGE_INDEX_BUFFER_BIT)
			usage |= ERGBufferUsageBits::INDEX;
		if (flags & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
			usage |= ERGBufferUsageBits::UNIFORM;
		if (flags & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
			usage |= ERGBufferUsageBits::STORAGE;
		if (flags & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)
			usage |= ERGBufferUsageBits::INDIRECT;
		if (flags & VK_BUFFER_USAGE_TRANSFER_SRC_BIT)
			usage |= ERGBufferUsageBits::TRANSFER_SRC;
		if (flags & VK_BUFFER_USAGE_TRANSFER_DST_BIT)
			usage |= ERGBufferUsageBits::TRANSFER_DST;
		if (flags & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR)
			usage |= ERGBufferUsageBits::ACCEL_STRUCT;
		if (flags & VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR)
			usage |= ERGBufferUsageBits::SHADER_BINDING;
		return usage;
	}

}
