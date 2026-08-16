#pragma once
#include <vector>
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>
#include <vulkan/vulkan.h>
#include <lux/cxx/compile_time/expected.hpp>
#include "lux/engine/gapi/RenderDevice.hpp"
#include "lux/engine/gapi/vk/SwapchainError.hpp"

namespace lux::gapi::vk
{
	struct SwapChainSupportDetails 
	{
		VkSurfaceCapabilitiesKHR		capabilities;
		std::vector<VkSurfaceFormatKHR> formats;
		std::vector<VkPresentModeKHR>	presentModes;
	};

	struct SurfaceQueryOps final
	{
		PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR capabilities{nullptr};
		PFN_vkGetPhysicalDeviceSurfaceFormatsKHR      formats{nullptr};
		PFN_vkGetPhysicalDeviceSurfacePresentModesKHR present_modes{nullptr};

		[[nodiscard]] static SurfaceQueryOps defaults() noexcept
		{
			return SurfaceQueryOps{
				&vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
				&vkGetPhysicalDeviceSurfaceFormatsKHR,
				&vkGetPhysicalDeviceSurfacePresentModesKHR,
			};
		}
	};

	using SurfaceCapabilitiesResult = lux::cxx::expected<
		VkSurfaceCapabilitiesKHR,
		SwapchainBuildError>;
	using SurfaceFormatsResult = lux::cxx::expected<
		std::vector<VkSurfaceFormatKHR>,
		SwapchainBuildError>;
	using SurfacePresentModesResult = lux::cxx::expected<
		std::vector<VkPresentModeKHR>,
		SwapchainBuildError>;
	using SwapChainSupportResult = lux::cxx::expected<
		SwapChainSupportDetails,
		SwapchainBuildError>;

	namespace detail
	{
		template <class T, class QueryFn>
		[[nodiscard]] lux::cxx::expected<
			std::vector<T>,
			SwapchainBuildError>
		enumerateSurfaceValues(
			VkPhysicalDevice physical_device,
			VkSurfaceKHR surface,
			ESwapchainBuildStage stage,
			QueryFn query) noexcept
		{
			if (physical_device == VK_NULL_HANDLE
				|| surface == VK_NULL_HANDLE
				|| query == nullptr)
			{
				return lux::cxx::unexpected(SwapchainBuildError{
					stage,
					std::nullopt,
				});
			}

			// WSI lists may change between the count and value calls. Retry a
			// small bounded number of times on VK_INCOMPLETE; returning the
			// exact status is preferable to an unbounded loop during surface
			// destruction churn.
			for (std::uint32_t attempt = 0; attempt < 3; ++attempt)
			{
				std::uint32_t count = 0;
				const VkResult count_result = query(
					physical_device,
					surface,
					&count,
					nullptr
				);
				if (count_result != VK_SUCCESS)
				{
					return lux::cxx::unexpected(SwapchainBuildError{
						stage,
						count_result,
					});
				}

				const std::uint32_t capacity = count;
				std::vector<T> values(capacity);
				if (count == 0)
					return values;

				const VkResult values_result = query(
					physical_device,
					surface,
					&count,
					values.data()
				);
				if (values_result == VK_SUCCESS)
				{
					// A conforming implementation cannot report more values than
					// the supplied capacity together with VK_SUCCESS. Treat that as
					// a local contract violation instead of fabricating default data.
					if (count > capacity)
					{
						return lux::cxx::unexpected(SwapchainBuildError{
							stage,
							std::nullopt,
						});
					}
					values.resize(count);
					return values;
				}
				if (values_result != VK_INCOMPLETE)
				{
					return lux::cxx::unexpected(SwapchainBuildError{
						stage,
						values_result,
					});
				}
			}

			return lux::cxx::unexpected(SwapchainBuildError{
				stage,
				VK_INCOMPLETE,
			});
		}

		[[nodiscard]] inline SurfaceCapabilitiesResult querySurfaceCapabilities(
			VkPhysicalDevice physical_device,
			VkSurfaceKHR surface,
			SurfaceQueryOps ops = SurfaceQueryOps::defaults()) noexcept
		{
			if (physical_device == VK_NULL_HANDLE
				|| surface == VK_NULL_HANDLE
				|| ops.capabilities == nullptr)
			{
				return lux::cxx::unexpected(SwapchainBuildError{
					ESwapchainBuildStage::SURFACE_CAPABILITIES,
					std::nullopt,
				});
			}

			VkSurfaceCapabilitiesKHR capabilities{};
			const VkResult result = ops.capabilities(
				physical_device,
				surface,
				&capabilities
			);
			if (result != VK_SUCCESS)
			{
				return lux::cxx::unexpected(SwapchainBuildError{
					ESwapchainBuildStage::SURFACE_CAPABILITIES,
					result,
				});
			}
			return capabilities;
		}

		[[nodiscard]] inline SurfaceFormatsResult querySurfaceFormats(
			VkPhysicalDevice physical_device,
			VkSurfaceKHR surface,
			SurfaceQueryOps ops = SurfaceQueryOps::defaults()) noexcept
		{
			return enumerateSurfaceValues<VkSurfaceFormatKHR>(
				physical_device,
				surface,
				ESwapchainBuildStage::SURFACE_FORMATS,
				ops.formats
			);
		}

		[[nodiscard]] inline SurfacePresentModesResult querySurfacePresentModes(
			VkPhysicalDevice physical_device,
			VkSurfaceKHR surface,
			SurfaceQueryOps ops = SurfaceQueryOps::defaults()) noexcept
		{
			return enumerateSurfaceValues<VkPresentModeKHR>(
				physical_device,
				surface,
				ESwapchainBuildStage::SURFACE_PRESENT_MODES,
				ops.present_modes
			);
		}

		[[nodiscard]] inline SwapChainSupportResult querySwapChainSupport(
			VkPhysicalDevice physical_device,
			VkSurfaceKHR surface,
			SurfaceQueryOps ops = SurfaceQueryOps::defaults()) noexcept
		{
			auto capabilities = querySurfaceCapabilities(
				physical_device,
				surface,
				ops
			);
			if (!capabilities)
				return lux::cxx::unexpected(capabilities.error());

			auto formats = querySurfaceFormats(physical_device, surface, ops);
			if (!formats)
				return lux::cxx::unexpected(formats.error());

			auto present_modes = querySurfacePresentModes(
				physical_device,
				surface,
				ops
			);
			if (!present_modes)
				return lux::cxx::unexpected(present_modes.error());

			return SwapChainSupportDetails{
				*capabilities,
				std::move(*formats),
				std::move(*present_modes),
			};
		}
	} // namespace detail

	// TODO remove redundant member
	class PhysicalDevice
	{
		friend class Instance;
	public:

		PhysicalDevice() : _physical_device(VK_NULL_HANDLE) {}

		/*
		 * Find the queue family index that supports the requested queue flags
		 * @param flag Queue flags to find
		 *  VK_QUEUE_GRAPHICS_BIT = 0x00000001,
		 *	VK_QUEUE_COMPUTE_BIT = 0x00000002,
		 *	VK_QUEUE_TRANSFER_BIT = 0x00000004,
		 *	VK_QUEUE_SPARSE_BINDING_BIT = 0x00000008,
		 *	VK_QUEUE_PROTECTED_BIT = 0x00000010,
		 *	VK_QUEUE_VIDEO_DECODE_BIT_KHR = 0x00000020,
		 *	VK_QUEUE_VIDEO_ENCODE_BIT_KHR = 0x00000040,
		 *	VK_QUEUE_OPTICAL_FLOW_BIT_NV = 0x00000100,
		 *	VK_QUEUE_FLAG_BITS_MAX_ENUM = 0x7FFFFFFF
		 *
		 * @return Queue family index that supports the requested queue flags
		 *		   if not found, return UINT32_MAX
		 */
		uint32_t findQueueFamilyIndexByFlags(VkQueueFlags flag)
		{
			for (uint32_t i = 0; i < _queue_family_properties.size(); i++)
			{
				if (_queue_family_properties[i].queueFamilyProperties.queueFlags & flag)
				{
					return i;
				}
			}
			return UINT32_MAX;
		}

		// return UINT32_MAX if not found
		uint32_t findMemoryTypeIndex(uint32_t typeFilter, VkMemoryPropertyFlags properties)
		{
			auto& memProperties = _memory_properties.memoryProperties;
			for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
			{
				if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
				{
					return i;
				}
			}

			return UINT32_MAX;
		}

		std::string_view deviceName()
		{
			return _properties.properties.deviceName;
		}

		EDeviceType type()
		{
			switch (_properties.properties.deviceType)
			{
			case VK_PHYSICAL_DEVICE_TYPE_OTHER:
			{
				return EDeviceType::OTHER;
			}
			case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
			{
				return EDeviceType::INTEGRATED_GPU;
			}
			case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
			{
				return EDeviceType::DISCRETE_GPU;
			}
			case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
			{
				return EDeviceType::VIRTUAL_GPU;
			}
			case VK_PHYSICAL_DEVICE_TYPE_CPU:
			{
				return EDeviceType::CPU;
			}
			case VK_PHYSICAL_DEVICE_TYPE_MAX_ENUM:
			{
				return EDeviceType::OTHER;
			}
			}
			return EDeviceType::OTHER;
		}

		[[nodiscard]] SurfaceCapabilitiesResult surfaceCapabilities(
			VkSurfaceKHR surface,
			SurfaceQueryOps ops = SurfaceQueryOps::defaults()) const noexcept
		{
			return detail::querySurfaceCapabilities(
				_physical_device,
				surface,
				ops
			);
		}

		bool surfaceSupport(VkSurfaceKHR surface, uint32_t queue_family_index) const
		{
			VkBool32 supported = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(_physical_device, queue_family_index, surface, &supported);
			return supported == VK_TRUE;
		}

		[[nodiscard]] SurfaceFormatsResult supportedSurfaceFormats(
			VkSurfaceKHR surface,
			SurfaceQueryOps ops = SurfaceQueryOps::defaults()) const noexcept
		{
			return detail::querySurfaceFormats(_physical_device, surface, ops);
		}

		[[nodiscard]] SurfacePresentModesResult supportedPresentModes(
			VkSurfaceKHR surface,
			SurfaceQueryOps ops = SurfaceQueryOps::defaults()) const noexcept
		{
			return detail::querySurfacePresentModes(
				_physical_device,
				surface,
				ops
			);
		}

		VkPresentModeKHR selectPresentMode(VkSurfaceKHR surface, const std::vector<VkPresentModeKHR>& modes, bool vsync) const
		{
			if (!vsync) {
				return VK_PRESENT_MODE_IMMEDIATE_KHR;
			}

			VkPresentModeKHR selectedPresentMode = VK_PRESENT_MODE_FIFO_KHR;
			for (const auto& mode : modes) {
				if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
					selectedPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
					break;
				}
			}

			return selectedPresentMode;
		}

		VkSurfaceFormatKHR selectSurfaceFormat(VkSurfaceKHR surface, const std::vector<VkSurfaceFormatKHR>& formats, VkColorSpaceKHR request_color_space) const
		{
			static const VkFormat requestSurfaceImageFormat[]{
				VK_FORMAT_B8G8R8A8_UNORM,
				VK_FORMAT_R8G8B8A8_UNORM,
				VK_FORMAT_B8G8R8_UNORM,
				VK_FORMAT_R8G8B8_UNORM
			};

			if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED)
			{
				return { VK_FORMAT_B8G8R8A8_UNORM, request_color_space };
			}

			for (const auto& format : formats)
			{
				for (const auto& request_format : requestSurfaceImageFormat)
				{
					if (format.format == request_format && format.colorSpace == request_color_space)
					{
						return format;
					}
				}
			}

			return formats[0];
		}

		[[nodiscard]] SwapChainSupportResult swapChainSupport(
			VkSurfaceKHR surface,
			SurfaceQueryOps ops = SurfaceQueryOps::defaults()) const noexcept
		{
			return detail::querySwapChainSupport(
				_physical_device,
				surface,
				ops
			);
		}

		bool presentSupport(VkSurfaceKHR surface, uint32_t queue_family_index) const
		{
			VkBool32 present_support = VK_FALSE;
			vkGetPhysicalDeviceSurfaceSupportKHR(_physical_device, queue_family_index, surface, &present_support);
			return present_support == VK_TRUE;
		}

		const std::vector<VkQueueFamilyProperties2>& queueFamilyProperties() const noexcept { return _queue_family_properties; }
		const VkPhysicalDeviceMemoryProperties2& memoryProperties() const noexcept { return _memory_properties; }
		const VkPhysicalDeviceProperties2& properties() const noexcept { return _properties; }
		const VkPhysicalDeviceFeatures2& features() const noexcept { return _features; }

		const VkPhysicalDeviceDescriptorIndexingProperties& descriptorIndexingProperties() const noexcept { return _descriptor_index_properties; }

		// External-interop GPU identity. CUDA selects the SAME physical GPU by matching
		// this against cudaDeviceProp.uuid (multi-GPU boxes: the Vulkan present GPU may
		// differ from the CUDA GPU). VkPhysicalDeviceIDProperties is Vulkan 1.1+ core.
		std::array<uint8_t, VK_UUID_SIZE> deviceUUID() const
		{
			VkPhysicalDeviceIDProperties idp{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
			VkPhysicalDeviceProperties2  p2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
			p2.pNext = &idp;
			vkGetPhysicalDeviceProperties2(_physical_device, &p2);
			std::array<uint8_t, VK_UUID_SIZE> out{};
			std::memcpy(out.data(), idp.deviceUUID, VK_UUID_SIZE);
			return out;
		}

		operator VkPhysicalDevice() const noexcept { return _physical_device; }
		const VkPhysicalDevice* operator&() const noexcept { return &_physical_device; }

		inline VkPhysicalDevice handle() const noexcept { return _physical_device; }
		inline const VkPhysicalDevice* handlePtr() const noexcept { return &_physical_device; }

	private:
		PhysicalDevice(VkPhysicalDevice device)
		{
			_physical_device = device;

			_properties.pNext = &_descriptor_index_properties;
			_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;

			_memory_properties.pNext = nullptr;
			_memory_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;

			_features.pNext = nullptr;
			_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

			vkGetPhysicalDeviceProperties2(device, &_properties);
			vkGetPhysicalDeviceMemoryProperties2(device, &_memory_properties);
			vkGetPhysicalDeviceFeatures2(device, &_features);

			uint32_t queue_family_count;
			vkGetPhysicalDeviceQueueFamilyProperties2(device, &queue_family_count, nullptr);
			_queue_family_properties.resize(queue_family_count);
			for (uint32_t i = 0; i < queue_family_count; i++)
			{
				_queue_family_properties[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
				_queue_family_properties[i].pNext = nullptr;
			}

			vkGetPhysicalDeviceQueueFamilyProperties2(device, &queue_family_count, _queue_family_properties.data());
		}
		
		VkPhysicalDevice								_physical_device{ VK_NULL_HANDLE };
		VkPhysicalDeviceProperties2						_properties{};
		VkPhysicalDeviceDescriptorIndexingProperties 	_descriptor_index_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
		VkPhysicalDeviceFeatures2						_features{};
		VkPhysicalDeviceMemoryProperties2				_memory_properties{};
		std::vector<VkQueueFamilyProperties2>			_queue_family_properties{};
	};
}
