#pragma once
#include "lux/engine/gapi/vk/LogicalDevice.hpp"
#include "lux/engine/gapi/vk/DeviceMemory.hpp"
#include <vulkan/vulkan.h>

namespace lux::gapi::vk
{
	class ImageBuilder;
	class Image
	{
	public:
		using Builder = ImageBuilder;

		Image() : image(VK_NULL_HANDLE) {}

		Image(VkImage image) : image(image) {}

		Image(VkDevice dev, const VkImageCreateInfo& ci, VkAllocationCallbacks* allocator = nullptr)
		{
			VK_FUNC_INVOKE(vkCreateImage, "Failed to create Image object", dev, &ci, allocator, &image);
		}

		Image(const Image&) = delete;
		Image& operator=(const Image&) = delete;


		Image(Image&& other) noexcept
		{
			image = other.image;
			other.image = VK_NULL_HANDLE;
		}

		Image& operator=(Image&& other) noexcept
		{
			image = other.image;
			other.image = VK_NULL_HANDLE;
			return *this;
		}

		void release(VkDevice dev, VkAllocationCallbacks* allocator = nullptr)
		{
			if (image != VK_NULL_HANDLE)
			{
				vkDestroyImage(dev, image, allocator);
				image = VK_NULL_HANDLE;
			}
		}

		VkMemoryRequirements memoryRequirements(VkDevice dev)
		{
			VkMemoryRequirements req;
			vkGetImageMemoryRequirements(dev, image, &req);
			return req;
		}

		DeviceMemory allocateMemory(VkDevice dev, VkDeviceSize allocation_size, uint32_t memory_type_index, VkAllocationCallbacks* allocator = nullptr)
		{
			return DeviceMemory{
				dev,
				allocation_size,
				memory_type_index,
				allocator
			};
		}

		void bindMemory(VkDevice logical_dev, VkDeviceMemory memory, VkDeviceSize offset = 0)
		{
			VK_FUNC_INVOKE(vkBindImageMemory, "Failed to bind memory to image", logical_dev, image, memory, offset);
		}

		inline operator bool() const noexcept { return image != VK_NULL_HANDLE; }
		inline bool operator!() const noexcept { return image == VK_NULL_HANDLE; }

		inline operator VkImage() const noexcept { return image; }
		inline const VkImage* operator&() const noexcept { return &image; }

		inline VkImage handle() const noexcept { return image; }
		inline const VkImage* handlePtr() const noexcept { return &image; }

	private:
		VkImage image;
	};

	class ImageBuilder
	{
	public:
		ImageBuilder()
		{
			ci.sType	   = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			ci.pNext	   = nullptr;
			ci.flags	   = 0;
			ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		}

		ImageBuilder& setFlags(VkImageCreateFlags flags)
		{
			ci.flags = flags;
			return *this;
		}

		ImageBuilder& setImageType(VkImageType type)
		{
			ci.imageType = type;
			return *this;
		}

		ImageBuilder& setFormat(VkFormat format)
		{
			ci.format = format;
			return *this;
		}

		ImageBuilder& setExtent(VkExtent3D extent)
		{
			ci.extent = extent;
			return *this;
		}

		ImageBuilder& setMipLevels(uint32_t mipLevels)
		{
			ci.mipLevels = mipLevels;
			return *this;
		}

		ImageBuilder& setArrayLayers(uint32_t arrayLayers)
		{
			ci.arrayLayers = arrayLayers;
			return *this;
		}

		ImageBuilder& setSamples(VkSampleCountFlagBits samples)
		{
			ci.samples = samples;
			return *this;
		}

		ImageBuilder& setTiling(VkImageTiling tiling)
		{
			ci.tiling = tiling;
			return *this;
		}

		ImageBuilder& setUsage(VkImageUsageFlags usage)
		{
			ci.usage = usage;
			return *this;
		}

		ImageBuilder& setSharingMode(VkSharingMode sharingMode)
		{
			ci.sharingMode = sharingMode;
			return *this;
		}

		ImageBuilder& setQueueFamilyIndices(const uint32_t* indices, uint32_t count)
		{
			ci.queueFamilyIndexCount = count;
			ci.pQueueFamilyIndices   = indices;
			return *this;
		}

		ImageBuilder& setInitialLayout(VkImageLayout layout)
		{
			ci.initialLayout = layout;
			return *this;
		}

		// Make the created image's memory exportable to an external API (CUDA interop).
		// Chains a VkExternalMemoryImageCreateInfo onto ci.pNext. The chained struct is a
		// builder member, so it stays alive until build() (called on this live builder).
		ImageBuilder& setExternalMemoryHandleTypes(VkExternalMemoryHandleTypeFlags types)
		{
			ext_mem_ci_.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
			ext_mem_ci_.handleTypes = types;
			ext_mem_ci_.pNext       = ci.pNext;   // preserve any existing chain
			ci.pNext                = &ext_mem_ci_;
			return *this;
		}

		Image build(VkDevice dev, VkAllocationCallbacks* allocator = nullptr)
		{
			return Image(dev, ci, allocator);
		}

		Image buildAndBind(
			VkDevice logical_dev, VkDeviceMemory memory, VkDeviceSize offset, VkAllocationCallbacks* allocator = nullptr)
		{
			Image img = Image(logical_dev, ci, allocator);
			img.bindMemory(logical_dev, memory, offset);
			return img;
		}

	private:

		static uint32_t calculateAlignedOffset(uint32_t currentOffset, uint32_t alignment) 
		{
			uint32_t alignedOffset = (currentOffset + alignment - 1) & ~(alignment - 1);
			return alignedOffset;
		}

		VkImageCreateInfo ci;
		VkExternalMemoryImageCreateInfo ext_mem_ci_{};   // chained by setExternalMemoryHandleTypes
	};
}