#pragma once
#include "lux/engine/gapi/vk/PhysicalDevice.hpp"
#include "lux/engine/gapi/vk/DeviceMemory.hpp"
#include "lux/engine/gapi/vk/Queue.hpp"
#include <cstring>

namespace lux::gapi::vk
{
	class LogicalDeviceBuilder;
	class LogicalDevice
	{
	public:
		using Builder = LogicalDeviceBuilder;

		LogicalDevice() : device(VK_NULL_HANDLE) {}

		LogicalDevice(VkPhysicalDevice pdevice, const VkDeviceCreateInfo& ci, VkAllocationCallbacks* allocator)
		{
			VK_FUNC_INVOKE(vkCreateDevice, "Failed to create LogicalDevice object", pdevice, &ci, allocator, &device);
		}

		LogicalDevice(const LogicalDevice&) = delete;
		LogicalDevice& operator=(const LogicalDevice&) = delete;

		LogicalDevice(LogicalDevice&& other) noexcept
		{
			device = other.device;
			other.device = VK_NULL_HANDLE;
		}

		LogicalDevice& operator=(LogicalDevice&& other) noexcept
		{
			device = other.device;
			other.device = VK_NULL_HANDLE;
			return *this;
		}

		void release(VkAllocationCallbacks* allocator = nullptr)
		{
			if (device != VK_NULL_HANDLE)
			{
				vkDestroyDevice(device, allocator);
				device = VK_NULL_HANDLE;
			}
		}

		[[nodiscard]] VkResult waitIdle() noexcept
		{
			if (device == VK_NULL_HANDLE)
				return VK_ERROR_INITIALIZATION_FAILED;
			return vkDeviceWaitIdle(device);
		}

		Queue getQueue(uint32_t queueFamilyIndex, uint32_t queueIndex)
		{
			return Queue{ device, queueFamilyIndex, queueIndex };
		}

		DeviceMemory allocateMemory(const VkMemoryAllocateInfo& ai, VkAllocationCallbacks* allocator = nullptr)
		{
			return DeviceMemory{ device, ai, allocator };
		}

		DeviceMemory allocateMemory(VkDeviceSize size, uint32_t memoryTypeIndex, VkAllocationCallbacks* allocator = nullptr)
		{
			VkMemoryAllocateInfo ai{
				.sType			 = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.allocationSize  = size,
				.memoryTypeIndex = memoryTypeIndex
			};
			
			return DeviceMemory{ device, ai, allocator };
		}

		inline operator VkDevice() const noexcept { return device; }
		inline const VkDevice* operator&() const noexcept { return &device; }

		inline VkDevice handle() const noexcept { return device; }
		inline const VkDevice* handlePtr() const noexcept { return &device; }

	private:
		VkDevice device{ VK_NULL_HANDLE };
	};

	struct QueueInfo
	{
		uint32_t family_index;
		uint32_t index;
	};

	class LogicalDeviceBuilder
	{
	public:
		LogicalDeviceBuilder()
		{
			// set default values
			create_info.sType					= VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
			create_info.pNext					= nullptr;
			create_info.flags					= 0;
			create_info.enabledExtensionCount	= 0;
			create_info.ppEnabledExtensionNames = nullptr;
			create_info.enabledLayerCount		= 0;
			create_info.ppEnabledLayerNames		= nullptr;
			create_info.pEnabledFeatures		= nullptr;
		}

		// Don't forget to check is physical device has required queue family
		// auto graphic_queue_family_index = physical_device->findQueueFamilyByFlags(VK_QUEUE_GRAPHICS_BIT);
		// if (graphic_queue_family_index == UINT32_MAX)
		// {
		// 		return VK_ERROR_INITIALIZATION_FAILED;
		// }
		// const float queue_priority[] = { 1.0f };
		// addQueueCreateInfo(graphic_queue_family_index, 1, queue_priority);
		inline LogicalDeviceBuilder& addQueueCreateInfo(uint32_t queueFamilyIndex, uint32_t queueCount, const float* pQueuePriorities)
		{
			VkDeviceQueueCreateInfo create_info{
				.sType				= VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				.queueFamilyIndex	= queueFamilyIndex,
				.queueCount			= queueCount,
				.pQueuePriorities	= pQueuePriorities
			};

			create_queue_infos.push_back(create_info);
			return *this;
		}

		inline LogicalDeviceBuilder& setEnabledFeatures(const VkPhysicalDeviceFeatures* features)
		{
			create_info.pEnabledFeatures = features;
			return *this;
		}

		// CRITICAL FIX: Add support for modern Vulkan features through pNext chain
		inline LogicalDeviceBuilder& setNextChain(const void* pNext)
		{
			create_info.pNext = pNext;
			return *this;
		}

		inline LogicalDeviceBuilder& addExtension(const char* extension)
		{
			device_extensions.push_back(extension);
			return *this;
		}

		LogicalDevice build(VkPhysicalDevice pdevice, VkAllocationCallbacks* allocator = nullptr)
		{
			device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

			uint32_t properties_count;
			vkEnumerateDeviceExtensionProperties(pdevice, nullptr, &properties_count, nullptr);
			std::vector<VkExtensionProperties> properties(properties_count);
			vkEnumerateDeviceExtensionProperties(pdevice, nullptr, &properties_count, properties.data());
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
			if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
				device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif
			// HDR metadata extension
			for (const auto& ext : properties) {
				if (strcmp(ext.extensionName, VK_EXT_HDR_METADATA_EXTENSION_NAME) == 0) {
					device_extensions.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
					break;
				}
			}
			
			// Add modern Vulkan extensions (fixes issue B)
			if (IsExtensionAvailable(properties, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
				device_extensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
			}
			
			create_info.queueCreateInfoCount	= static_cast<uint32_t>(create_queue_infos.size());
			create_info.pQueueCreateInfos		= create_queue_infos.data();
			create_info.enabledExtensionCount	= static_cast<uint32_t>(device_extensions.size());
			create_info.ppEnabledExtensionNames = device_extensions.data();

			return LogicalDevice{ pdevice, create_info, allocator };
		}

	private:

		static bool IsExtensionAvailable(const std::vector<VkExtensionProperties>& properties, const char* extension)
		{
			for (const VkExtensionProperties& p : properties)
			{
				if (strcmp(p.extensionName, extension) != 0)
				{
					return false;
				}
			}
			return true;
		}

		std::vector<const char*>				device_extensions;
		std::vector<VkDeviceQueueCreateInfo>	create_queue_infos;
		VkDeviceCreateInfo						create_info;
	};
}
