#pragma once
#include "lux/engine/gapi/vk/Object.hpp"
#include "lux/engine/gapi/vk/ImageView.hpp"
#include <vector>
#include <array>
#include <optional>
#include <string>
#include <cassert>
#include <algorithm>

namespace lux::gapi::vk
{
	class SwapchainBuilder;
	class Swapchain
	{
	public:
		using Builder = SwapchainBuilder;

		Swapchain() : swapchain{ VK_NULL_HANDLE } {}

		Swapchain(VkDevice device, const VkSwapchainCreateInfoKHR& info, VkAllocationCallbacks* allocator)
		{
			VK_FUNC_INVOKE(vkCreateSwapchainKHR, "Failed to create Swapchain object", device, &info, allocator, &swapchain);
		}

		Swapchain(const Swapchain&) = delete;
		Swapchain& operator=(const Swapchain&) = delete;

		Swapchain(Swapchain&& other) noexcept
		{
			swapchain		= other.swapchain;
			other.swapchain = VK_NULL_HANDLE;
		}

		Swapchain& operator=(Swapchain&& other) noexcept
		{
			swapchain		= other.swapchain;
			other.swapchain = VK_NULL_HANDLE;
			return *this;
		}

		void release(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
		{
			if (swapchain != VK_NULL_HANDLE)
			{
				vkDestroySwapchainKHR(device, swapchain, allocator);
				swapchain = VK_NULL_HANDLE;
			}
		}

		std::vector<Image> images(VkDevice device) const
		{
			std::vector<Image> ret_images;
			uint32_t image_count = 0;
			vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
			std::vector<VkImage> images(image_count);
			vkGetSwapchainImagesKHR(device, swapchain, &image_count, images.data());
			ret_images.reserve(image_count);
			for (auto& image : images)
			{
				ret_images.emplace_back(image);
			}
			return ret_images;
		}

		inline operator VkSwapchainKHR() const noexcept { return swapchain; }
		const VkSwapchainKHR* operator&() const noexcept { return &swapchain; }

		inline VkSwapchainKHR handle() const noexcept { return swapchain; }
		inline const VkSwapchainKHR* handlePtr() const noexcept { return &swapchain; }

	private:
		VkSwapchainKHR swapchain{ VK_NULL_HANDLE };
	};

	class SwapchainBuilder
	{
	public:
		SwapchainBuilder(const PhysicalDevice& physical_device, const Surface& surface)
		{
			support_details = physical_device.swapChainSupport(surface);
			// set default values
			create_info.sType					= VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
			create_info.surface					= surface;
			create_info.pNext					= nullptr;
			create_info.flags					= 0;
			create_info.imageFormat				= VK_FORMAT_UNDEFINED;
			create_info.imageColorSpace			= VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
			create_info.imageExtent				= { 0, 0 };
			create_info.imageArrayLayers		= 1;
			create_info.imageUsage				= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			create_info.imageSharingMode		= VK_SHARING_MODE_EXCLUSIVE;
			create_info.queueFamilyIndexCount   = 0;
			create_info.pQueueFamilyIndices		= nullptr;
			create_info.preTransform			= VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
			create_info.compositeAlpha			= VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
			create_info.presentMode				= VK_PRESENT_MODE_FIFO_KHR;
			create_info.clipped					= VK_TRUE;
			create_info.oldSwapchain			= VK_NULL_HANDLE;
		}

		Swapchain build(VkDevice device, VkAllocationCallbacks* allocator = nullptr)
		{
			configureSwapchainSettings();
			return Swapchain(device, create_info, allocator);
		}

		SwapchainBuilder& enableVsync(bool enable)
		{
			create_info.presentMode = choosePresentMode(support_details.presentModes, enable);
			return *this;
		}

		SwapchainBuilder& enableHDR(bool enable)
		{
			VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(support_details.formats, enable);
			create_info.imageFormat		= surfaceFormat.format;
			create_info.imageColorSpace = surfaceFormat.colorSpace;
			return *this;
		}

		SwapchainBuilder& setQueueFamilyIndices(const uint32_t(&index)[2])
		{
			queue_family_index[0] = index[0];
			queue_family_index[1] = index[1];
			return *this;
		}

		SwapchainBuilder& setQueueFamilyIndices(const std::array<uint32_t, 2>& indices)
		{
			queue_family_index = indices;
			return *this;
		}

		SwapchainBuilder& setExtent(VkExtent2D extent)
		{
			create_info.imageExtent = extent;
			return *this;
		}

		SwapchainBuilder& setFormat(VkFormat format)
		{
			create_info.imageFormat = format;
			return *this;
		}

		SwapchainBuilder& setOldSwapchain(VkSwapchainKHR oldSwapchain)
		{
			create_info.oldSwapchain = oldSwapchain;
			return *this;
		}

		SwapchainBuilder& setCompositeAlpha(VkCompositeAlphaFlagBitsKHR compositeAlpha)
		{
			create_info.compositeAlpha = compositeAlpha;
			return *this;
		}

		const std::array<uint32_t, 2>& getQueueFamilyIndices() const
		{
			return queue_family_index;
		}

		const VkSwapchainCreateInfoKHR& getCreateInfo() const
		{
			return create_info;
		}

	private:

		VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes, bool vsyncEnabled) {
			if (!vsyncEnabled) {
				for (const auto& mode : availablePresentModes) {
					if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
						return mode;
				}
				// IMMEDIATE not available, try MAILBOX (lowest-latency with tearing prevention)
				for (const auto& mode : availablePresentModes) {
					if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
						return mode;
				}
			}

			// vsync ON → FIFO_KHR (guaranteed available, caps frame rate to refresh rate)
			return VK_PRESENT_MODE_FIFO_KHR;
		}

		VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats, bool hdr) {
			VkSurfaceFormatKHR chosenFormat = availableFormats[0];
			for (const auto& availableFormat : availableFormats) {
				if (hdr) {
					if (availableFormat.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT && availableFormat.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32)
					{
						chosenFormat = availableFormat;
						break;
					}
				}
				else {
					if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
						availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
					{
						chosenFormat = availableFormat;
						break;
					}
				}
			}
			return chosenFormat;
		}

		void configureSwapchainSettings()
		{
			// Request minImageCount + 1 to guarantee forward progress when using
			// UINT64_MAX acquire timeout (VUID-vkAcquireNextImageKHR-surface-07783).
			// With only minImageCount images the presentation engine may hold all of
			// them, leaving none for the application to acquire.
			bool is_mailbox = create_info.presentMode == VK_PRESENT_MODE_MAILBOX_KHR;
			uint32_t imageCount = support_details.capabilities.minImageCount + 1;
			if (is_mailbox) imageCount = std::max(imageCount, 3u);
			if (support_details.capabilities.maxImageCount > 0) {
				imageCount = std::min(imageCount, support_details.capabilities.maxImageCount);
			}
			create_info.minImageCount	= imageCount;

			bool isQueueComplete{ true };
			const auto& queue_family_indices = getQueueFamilyIndices();
			if (queue_family_indices[0] == UINT32_MAX) { isQueueComplete = true; }
			else if (queue_family_indices[0] != queue_family_indices[1]) { isQueueComplete = false; }

			if (!isQueueComplete)
			{
				create_info.imageSharingMode		= VK_SHARING_MODE_CONCURRENT;
				create_info.queueFamilyIndexCount	= 2;
				create_info.pQueueFamilyIndices		= queue_family_indices.data();
			}
			else
			{
				create_info.imageSharingMode		= VK_SHARING_MODE_EXCLUSIVE;
				create_info.queueFamilyIndexCount	= 0;
				create_info.pQueueFamilyIndices		= nullptr;
			}

			create_info.preTransform				= support_details.capabilities.currentTransform;
			create_info.imageArrayLayers			= 1;
			create_info.clipped						= VK_TRUE;
		}

		SwapChainSupportDetails		support_details;
		VkSwapchainCreateInfoKHR	create_info;
		bool						threeD{ true };
		std::array<uint32_t, 2>		queue_family_index{UINT32_MAX, UINT32_MAX};
	};
}
