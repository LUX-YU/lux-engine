#pragma once
#include "lux/engine/gapi/vk/Object.hpp"
#include <vulkan/vulkan.h>

namespace lux::gapi::vk
{
	class SamplerBuilder;
	class Sampler
	{
	public:
		Sampler() : sampler(VK_NULL_HANDLE) {}

		Sampler(VkDevice device, const VkSamplerCreateInfo& info, VkAllocationCallbacks* allocator = nullptr)
		{
			VK_FUNC_INVOKE(vkCreateSampler, "Failed to create Sampler object", device, &info, allocator, &sampler);
		}

		Sampler(const Sampler&) = delete;
		Sampler& operator=(const Sampler&) = delete;

		Sampler(Sampler&& other) noexcept
		{
			sampler = other.sampler;
			other.sampler = VK_NULL_HANDLE;
		}

		Sampler& operator=(Sampler&& other) noexcept
		{
			sampler = other.sampler;
			other.sampler = VK_NULL_HANDLE;
			return *this;
		}

		void release(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
		{
			if (sampler != VK_NULL_HANDLE)
			{
				vkDestroySampler(device, sampler, allocator);
				sampler = VK_NULL_HANDLE;
			}
		}

		inline operator VkSampler() const noexcept { return sampler; }
		inline const VkSampler* operator&() const noexcept { return &sampler; }
		
		inline VkSampler handle() const noexcept { return sampler; }
		inline const VkSampler* handlePtr() const noexcept { return &sampler; }

	private:
		VkSampler sampler{ VK_NULL_HANDLE };
	};

	class SamplerBuilder
	{
	public:
		SamplerBuilder()
		{
			info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			info.pNext = nullptr;
			info.flags = 0;
			info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			info.compareOp  = VK_COMPARE_OP_NEVER;
			info.unnormalizedCoordinates = VK_FALSE;
			info.anisotropyEnable = VK_FALSE;
			info.compareEnable = VK_FALSE;
			info.minLod = 0.0f;
			info.maxLod = 1.0f;
		}

		SamplerBuilder& setMagFilter(VkFilter filter)
		{
			info.magFilter = filter;
			return *this;
		}

		SamplerBuilder& setMinFilter(VkFilter filter)
		{
			info.minFilter = filter;
			return *this;
		}

		SamplerBuilder& setMipmapMode(VkSamplerMipmapMode mode)
		{
			info.mipmapMode = mode;
			return *this;
		}

		SamplerBuilder& setAddressModeU(VkSamplerAddressMode mode)
		{
			info.addressModeU = mode;
			return *this;
		}

		SamplerBuilder& setAddressModeV(VkSamplerAddressMode mode)
		{
			info.addressModeV = mode;
			return *this;
		}

		SamplerBuilder& setAddressModeW(VkSamplerAddressMode mode)
		{
			info.addressModeW = mode;
			return *this;
		}

		SamplerBuilder& setMipLodBias(float bias)
		{
			info.mipLodBias = bias;
			return *this;
		}

		SamplerBuilder& setAnisotropyEnable(VkBool32 enable)
		{
			info.anisotropyEnable = enable;
			return *this;
		}

		SamplerBuilder& setMaxAnisotropy(float anisotropy)
		{
			info.maxAnisotropy = anisotropy;
			return *this;
		}

		SamplerBuilder& setCompareEnable(VkBool32 enable)
		{
			info.compareEnable = enable;
			return *this;
		}

		SamplerBuilder& setCompareOp(VkCompareOp op)
		{
			info.compareOp = op;
			return *this;
		}

		SamplerBuilder& setMinLod(float lod)
		{
			info.minLod = lod;
			return *this;
		}

		SamplerBuilder& setMaxLod(float lod)
		{
			info.maxLod = lod;
			return *this;
		}

		SamplerBuilder& setBorderColor(VkBorderColor color)
		{
			info.borderColor = color;
			return *this;
		}

		SamplerBuilder& setUnnormalizedCoordinates(VkBool32 enable)
		{
			info.unnormalizedCoordinates = enable;
			return *this;
		}

		Sampler build(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
		{
			return Sampler(device, info, allocator);
		}

	private:
		VkSamplerCreateInfo info;
	};
}