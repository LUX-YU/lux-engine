#pragma once
#include <lux/engine/render/gpu/utils/FormatMap.hpp>   // convertTextureFormat(已下沉 utils;graph->utils 为向下依赖)
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGTransientDSTypes.hpp>   // EDescriptorType, EImageLayout
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <cassert>

namespace lux::render
{

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
		if (usage & static_cast<uint32_t>(ERGTextureUsageBits::INPUT_ATTACHMENT))
			flags |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
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
			case EImageLayout::RENDERING_LOCAL_READ:            return VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ;
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
			case VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ:            return EImageLayout::RENDERING_LOCAL_READ;
			default:                                              return EImageLayout::UNDEFINED;
		}
	}

	// =====================================================================
	// Reverse conversion: convert from Vulkan types back to RenderGraph types
	// =====================================================================


	// (convertVkFormat 已删除 —— 它与 utils/FormatMap.hpp 的窄表同向映射同一对类型,
	//  且自首次提交起零调用点。其 70 条映射已作为唯一权威表搬入 FormatMap.hpp 的
	//  tryMapVkFormat(返 optional,不再静默 fallback)。)

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
		if (flags & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)
			usage |= static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::INPUT_ATTACHMENT);
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
