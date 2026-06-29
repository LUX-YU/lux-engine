#include "lux/engine/render/core/VulkanContext.hpp"
#include <vk_mem_alloc.h>
#include <cstdio>
#include <limits>
#include <iomanip>
#include <string_view>
#include <vector>

namespace lux::render
{
	/**
	 * @brief Vulkan debug report callback function
	 * @details Called by Vulkan validation layers to report debug information
	 */
	static inline VkBool32 debug_report_callback(
		VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType, uint64_t object, size_t location,
		int32_t messageCode, const char* pLayerPrefix, const char* pMessage, void* pUserData
	);

	// InstanceContext implementation
	InstanceContext::InstanceContext(const std::vector<const char*>& required_extensions, DebugCallback debug_callback, VkAllocationCallbacks* allocator)
		: debug_callback_(debug_callback), allocator_(allocator)
	{
		lux::gapi::vk::Instance::Builder instance_builder;
		instance_builder.addExtensions(required_extensions);

		if (debug_callback_)
		{
			instance_builder.enableDebugReport();
		}

		instance_ = instance_builder.build(allocator_);

		if (debug_callback_)
		{
			debug_report_ = instance_.createDebugReport(
				&debug_report_callback,
				&debug_callback_,
				allocator_
			);
		}
	}

	InstanceContext::~InstanceContext()
	{
		// Release resources in reverse creation order
		if (debug_callback_)
		{
			debug_report_.release(instance_, allocator_);
		}
		instance_.release(allocator_);
	}

	// DeviceContext implementation
	DeviceContext::DeviceContext(InstanceContext& instance_context)
		: instance_context_(instance_context)
	{
	}

	Expected<void> DeviceContext::init(EPhysicalDeviceSelectionPolicy policy)
	{
		// Select physical device
		auto devices = instance_context_.instance().listPhysicalDevices();
		if (devices.empty())
			return lux::cxx::unexpected(make_error_code(ERenderError::GpuResourceCreationFailed));

		switch (policy) 
		{
			case EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED:
			{
				// First try to find a discrete GPU
				for (auto& device : devices)
				{
					if (device.type() == lux::gapi::EDeviceType::DISCRETE_GPU)
					{
						physical_device_ = std::move(device);
						break;
					}
				}
				// If no discrete GPU found, try integrated GPU as fallback
				if (!physical_device_)
				{
					for (auto& device : devices)
					{
						if (device.type() == lux::gapi::EDeviceType::INTEGRATED_GPU)
						{
							physical_device_ = std::move(device);
							break;
						}
					}
				}
				break;
			}
			case EPhysicalDeviceSelectionPolicy::INTEGRATED_GPU_PREFERRED:
			{
				// First try to find an integrated GPU
				for (auto& device : devices)
				{
					if (device.type() == lux::gapi::EDeviceType::INTEGRATED_GPU)
					{
						physical_device_ = std::move(device);
						break;
					}
				}
				// If no integrated GPU found, try discrete GPU as fallback
				if (!physical_device_)
				{
					for (auto& device : devices)
					{
						if (device.type() == lux::gapi::EDeviceType::DISCRETE_GPU)
						{
							physical_device_ = std::move(device);
							break;
						}
					}
				}
				break;
			}
		}

		// Final fallback: use the first available device if we haven't found a suitable one
		if (!physical_device_ && !devices.empty())
		{
			physical_device_ = std::move(devices.front());
		}

		if (!physical_device_)
			return lux::cxx::unexpected(make_error_code(ERenderError::GpuResourceCreationFailed));

		// S-10: Verify all required Vulkan features are supported BEFORE attempting
		// logical device creation.  Without this check, vkCreateDevice returns
		// VK_ERROR_FEATURE_NOT_PRESENT with no indication of which feature is absent.
		{
			VkPhysicalDeviceVulkan13Features feat13{};
			feat13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

			VkPhysicalDeviceVulkan12Features feat12{};
			feat12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
			feat12.pNext = &feat13;

			VkPhysicalDeviceVulkan11Features feat11{};
			feat11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
			feat11.pNext = &feat12;

			VkPhysicalDeviceFeatures2 feat2{};
			feat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
			feat2.pNext = &feat11;

			vkGetPhysicalDeviceFeatures2(physical_device_, &feat2);

			std::string missing;
			auto require = [&](VkBool32 supported, const char* name)
			{
				if (!supported) { missing += ' '; missing += name; }
			};

			// Vulkan 1.3
			require(feat13.synchronization2,   "synchronization2");
			require(feat13.dynamicRendering,   "dynamicRendering");
			// Vulkan 1.2
			require(feat12.descriptorIndexing,                           "descriptorIndexing");
			require(feat12.runtimeDescriptorArray,                       "runtimeDescriptorArray");
			require(feat12.descriptorBindingVariableDescriptorCount,     "descriptorBindingVariableDescriptorCount");
			require(feat12.descriptorBindingPartiallyBound,              "descriptorBindingPartiallyBound");
			require(feat12.descriptorBindingSampledImageUpdateAfterBind, "descriptorBindingSampledImageUpdateAfterBind");
			require(feat12.drawIndirectCount,                            "drawIndirectCount");
			require(feat12.shaderOutputLayer,                            "shaderOutputLayer");
			require(feat12.bufferDeviceAddress,                          "bufferDeviceAddress");
			// Vulkan 1.1
			require(feat11.shaderDrawParameters, "shaderDrawParameters");
			// Vulkan 1.0 base features
			require(feat2.features.samplerAnisotropy,         "samplerAnisotropy");
			require(feat2.features.multiDrawIndirect,         "multiDrawIndirect");
			require(feat2.features.drawIndirectFirstInstance, "drawIndirectFirstInstance");
			require(feat2.features.wideLines,                 "wideLines");
			require(feat2.features.shaderClipDistance,         "shaderClipDistance");
			// buffer_reference (GL_EXT_buffer_reference) dereferences via 64-bit
			// addresses → the SPIR-V declares the Int64 capability, which requires
			// shaderInt64. Pairs with bufferDeviceAddress above (used by the
			// GPU-driven cull's data-driven instance-cull-mask address).
			require(feat2.features.shaderInt64,               "shaderInt64");

			if (!missing.empty())
				return lux::cxx::unexpected(make_error_code(ERenderError::GpuResourceCreationFailed));
		}

		// Create logical device
		constexpr uint32_t INVALID_QUEUE_FAMILY_INDEX = std::numeric_limits<uint32_t>::max();

		graphics_queue_family_index_ = physical_device_.findQueueFamilyIndexByFlags(VK_QUEUE_GRAPHICS_BIT);

		if (graphics_queue_family_index_ == INVALID_QUEUE_FAMILY_INDEX)
			return lux::cxx::unexpected(make_error_code(ERenderError::GpuResourceCreationFailed));

		// Discover dedicated async compute queue (COMPUTE but NOT GRAPHICS)
		{
			const auto& families = physical_device_.queueFamilyProperties();
			for (uint32_t i = 0; i < static_cast<uint32_t>(families.size()); ++i)
			{
				const auto& props = families[i].queueFamilyProperties;
				if ((props.queueFlags & VK_QUEUE_COMPUTE_BIT) &&
					!(props.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
					props.queueCount > 0)
				{
					async_compute_queue_family_index_ = i;
					has_async_compute_ = true;
					break;
				}
			}
		}

		// Discover dedicated transfer queue (TRANSFER but NOT GRAPHICS and NOT COMPUTE)
		{
			const auto& families = physical_device_.queueFamilyProperties();
			for (uint32_t i = 0; i < static_cast<uint32_t>(families.size()); ++i)
			{
				const auto& props = families[i].queueFamilyProperties;
				if ((props.queueFlags & VK_QUEUE_TRANSFER_BIT) &&
					!(props.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
					!(props.queueFlags & VK_QUEUE_COMPUTE_BIT) &&
					props.queueCount > 0)
				{
					transfer_queue_family_index_ = i;
					has_transfer_ = true;
					break;
				}
			}
		}

		float queue_priority[] = { 1.0f };
		
		// CRITICAL FIX: Enable necessary Vulkan features for bindless descriptor indexing
		VkPhysicalDeviceFeatures device_features{};
		device_features.samplerAnisotropy = VK_TRUE;
		device_features.multiDrawIndirect = VK_TRUE;
		device_features.drawIndirectFirstInstance = VK_TRUE;
		device_features.wideLines = VK_TRUE;
		device_features.shaderClipDistance = VK_TRUE;
		// buffer_reference shaders (e.g. mesh_cull_unified.comp reading the instance
		// cull-mask via a 64-bit address) declare the SPIR-V Int64 capability, which
		// requires shaderInt64. Without it the validation layer raises
		// VUID-VkShaderModuleCreateInfo-pCode-08740 on every such shader module.
		device_features.shaderInt64 = VK_TRUE;

		// Enable Vulkan 1.2 features for descriptor indexing
		// NOTE: VkPhysicalDeviceVulkan12Features includes all descriptor indexing features,
		// so we don't need a separate VkPhysicalDeviceDescriptorIndexingFeatures struct
		VkPhysicalDeviceVulkan12Features vulkan12_features{};
		vulkan12_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		vulkan12_features.pNext = nullptr;
		
		// Core descriptor indexing feature (required when using VK_EXT_descriptor_indexing)
		vulkan12_features.descriptorIndexing = VK_TRUE;
		
		// Bindless descriptor features
		vulkan12_features.runtimeDescriptorArray = VK_TRUE;
		vulkan12_features.descriptorBindingVariableDescriptorCount = VK_TRUE;
		vulkan12_features.descriptorBindingPartiallyBound = VK_TRUE;
		vulkan12_features.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
		vulkan12_features.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
		vulkan12_features.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
		vulkan12_features.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
		vulkan12_features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
		vulkan12_features.drawIndirectCount = VK_TRUE;
		vulkan12_features.timelineSemaphore = VK_TRUE;
		vulkan12_features.shaderOutputLayer = VK_TRUE;
		// Buffer device address (BDA): lets a shader read an SSBO via a 64-bit
		// address carried in push-constants instead of a dedicated descriptor
		// binding. Used by the GPU-driven cull so the world-partition active-mask
		// is data-driven (no fixed binding 8) and large-world is opt-in. The VMA
		// allocator below must also opt in (VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT).
		vulkan12_features.bufferDeviceAddress = VK_TRUE;
		
		// Enable Vulkan 1.1 features for shader draw parameters
		VkPhysicalDeviceVulkan11Features vulkan11_features{};
		vulkan11_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
		vulkan11_features.pNext = nullptr;
		vulkan11_features.shaderDrawParameters = VK_TRUE;
		
		// Enable synchronization2 feature for modern barrier commands
		VkPhysicalDeviceVulkan13Features vulkan13_features{};
		vulkan13_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		vulkan13_features.pNext = nullptr;
		vulkan13_features.synchronization2 = VK_TRUE;
		vulkan13_features.dynamicRendering = VK_TRUE;

		// Query and conditionally enable VK_EXT_robustness2 nullDescriptor.
		// When enabled, VUID-vkCmdDraw-None-04008's guard condition
		// ("If the nullDescriptor feature is not enabled") becomes false, which
		// suppresses spurious validation errors that fire when a pipeline with zero
		// vertex input bindings is drawn after a pipeline that did bind vertex buffers.
		// This is a known Validation Layer 1.3.280 bug.
		bool has_robustness2 = false;
		{
			uint32_t ext_count = 0;
			vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &ext_count, nullptr);
			std::vector<VkExtensionProperties> device_exts(ext_count);
			vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &ext_count, device_exts.data());
			for (const auto& e : device_exts)
				if (std::string_view(e.extensionName) == VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)
					{ has_robustness2 = true; break; }
		}
		VkPhysicalDeviceRobustness2FeaturesEXT robustness2_features{};
		robustness2_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
		robustness2_features.pNext = nullptr;
		robustness2_features.nullDescriptor = VK_TRUE;

		// Chain the features: vulkan12_features -> vulkan11_features -> vulkan13_features [-> robustness2]
		vulkan12_features.pNext = &vulkan11_features;
		vulkan11_features.pNext = &vulkan13_features;
		if (has_robustness2)
			vulkan13_features.pNext = &robustness2_features;

		// External-memory/semaphore interop (CUDA<->Vulkan zero-copy direct-display).
		// On a 1.3 device the base external_memory/semaphore + properties2 are CORE; only
		// the platform handle-export extensions need explicit enabling: Win32 handle on
		// Windows, opaque fd on POSIX. Conditional: absent (non-NV / unsupported driver)
		// -> interop stays off and callers fall back to host upload.
		bool has_external_interop = false;
		{
			uint32_t ext_count = 0;
			vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &ext_count, nullptr);
			std::vector<VkExtensionProperties> exts(ext_count);
			vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &ext_count, exts.data());
			auto has_ext = [&](const char* name) {
				for (const auto& e : exts)
					if (std::string_view(e.extensionName) == name) return true;
				return false;
			};
#if defined(VK_USE_PLATFORM_WIN32_KHR)
			has_external_interop =
				has_ext(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) &&
				has_ext(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
#else
			has_external_interop =
				has_ext(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
				has_ext(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
#endif
		}

		// Build logical device with all needed queues
		// Vulkan requires unique queue family indices in create infos,
		// so we need to collect unique families and request appropriate counts
		auto builder = lux::gapi::vk::LogicalDevice::Builder()
			.addExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
			.addExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
		if (has_robustness2)
			builder.addExtension(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
		if (has_external_interop)
		{
#if defined(VK_USE_PLATFORM_WIN32_KHR)
			builder.addExtension(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME)
			       .addExtension(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
#else
			builder.addExtension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)
			       .addExtension(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
#endif
		}
		builder
			.setEnabledFeatures(&device_features)
			.setNextChain(&vulkan12_features)  // Head of the feature chain
			.addQueueCreateInfo(graphics_queue_family_index_, 1, queue_priority);

		// Request async compute queue if it's a different family
		if (has_async_compute_ && async_compute_queue_family_index_ != graphics_queue_family_index_)
		{
			builder.addQueueCreateInfo(async_compute_queue_family_index_, 1, queue_priority);
		}

		// Request transfer queue if it's a different family (and different from compute)
		if (has_transfer_ && transfer_queue_family_index_ != graphics_queue_family_index_ &&
			transfer_queue_family_index_ != async_compute_queue_family_index_)
		{
			builder.addQueueCreateInfo(transfer_queue_family_index_, 1, queue_priority);
		}

		logical_device_ = builder.build(physical_device_, instance_context_.allocator());

		if (!logical_device_)
			return lux::cxx::unexpected(make_error_code(ERenderError::VulkanObjectCreationFailed));

		// Record the interop gate: the platform external extensions are now enabled on
		// the device (or were unavailable). Consumers query supportsExternalMemory().
		supports_external_interop_ = has_external_interop;

		graphics_queue_ = logical_device_.getQueue(graphics_queue_family_index_, 0);

		// Retrieve async compute queue
		if (has_async_compute_)
		{
			async_compute_queue_ = logical_device_.getQueue(async_compute_queue_family_index_, 0);
		}

		// Retrieve transfer queue
		if (has_transfer_)
		{
			transfer_queue_ = logical_device_.getQueue(transfer_queue_family_index_, 0);
		}

		// Create VMA allocator for memory management.
		// BUFFER_DEVICE_ADDRESS_BIT: required so VMA-managed buffers created with
		// VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT can be queried for their GPU
		// address (vkGetBufferDeviceAddress). Pairs with the device feature above.
		VmaAllocatorCreateInfo vma_info{
			.flags          		= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
			.physicalDevice 		= physical_device_,
			.device         		= logical_device_,
			.pAllocationCallbacks 	= instance_context_.allocator(),
			.instance       		= instance_context_.instance(),
			.vulkanApiVersion 		= VK_API_VERSION_1_3,  // S-06: match actual API level used
		};

		if (vmaCreateAllocator(&vma_info, &vma_allocator_) != VK_SUCCESS) 
			return lux::cxx::unexpected(make_error_code(ERenderError::VulkanObjectCreationFailed));

		return {};
	}

	DeviceContext::~DeviceContext()
	{
		// Destroy VMA allocator
		if (vma_allocator_){
			// ── Leak diagnostic (self-gating) ────────────────────────────────
			// DeviceContext is destroyed LAST (it is created first in the render
			// server's Impl, so reverse-order member destruction frees it after
			// every render resource). Therefore ANY VMA allocation still live at
			// this point is a genuine leak — a buffer/image whose owner forgot to
			// free it before teardown. Only dumps when something actually leaked,
			// so it is silent on a clean exit. The detailed map names each live
			// allocation (render-graph buffers carry names like "ClusterParams"),
			// which pinpoints the leaking owner without guessing.
			VmaTotalStatistics vma_stats{};
			vmaCalculateStatistics(vma_allocator_, &vma_stats);
			if (vma_stats.total.statistics.allocationCount > 0)
			{
				char* json = nullptr;
				vmaBuildStatsString(vma_allocator_, &json, VK_TRUE);
				std::fprintf(stderr,
					"[VMA LEAK] %u allocation(s) / %llu byte(s) still live at "
					"device teardown (each is an un-freed buffer/image):\n%s\n",
					vma_stats.total.statistics.allocationCount,
					static_cast<unsigned long long>(
						vma_stats.total.statistics.allocationBytes),
					json ? json : "(no detail map)");
				if (json)
					vmaFreeStatsString(vma_allocator_, json);
			}

			vmaDestroyAllocator(vma_allocator_);
			vma_allocator_ = nullptr;
		}

		// Release logical device
		logical_device_.release(instance_context_.allocator());
	}

	// ResourceContext implementation
	ResourceContext::ResourceContext(DeviceContext& device_context)
		: device_context_(device_context)
	{
	}

	Expected<void> ResourceContext::init(const DescriptorPoolConfig& pool_config)
	{
		// Create descriptor pool
		descriptor_pool_ = lux::gapi::vk::DescriptorPool::Builder()
			.setFlags(VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)
			.addPoolSize(VK_DESCRIPTOR_TYPE_SAMPLER,                  pool_config.sampler)
			.addPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   pool_config.combined_image_sampler)
			.addPoolSize(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,            pool_config.sampled_image)
			.addPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            pool_config.storage_image)
			.addPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,     pool_config.uniform_texel_buffer)
			.addPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,     pool_config.storage_texel_buffer)
			.addPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,           pool_config.uniform_buffer)
			.addPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           pool_config.storage_buffer)
			.addPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,   pool_config.uniform_buffer_dynamic)
			.addPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,   pool_config.storage_buffer_dynamic)
			.addPoolSize(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,         pool_config.input_attachment)
			.setMaxSets(pool_config.max_sets)
			.build(device_context_.logicalDevice());

		if (!descriptor_pool_) 
			return lux::cxx::unexpected(make_error_code(ERenderError::VulkanObjectCreationFailed));

		// Create command pool (for graphics queue)
		command_pool_ = lux::gapi::vk::CommandPool::Builder()
			.setQueueFamilyIndex(device_context_.graphicsQueueFamilyIndex())
			.build(device_context_.logicalDevice(), device_context_.instanceContext().allocator());

		// Create compute command pool (for async compute queue)
		compute_command_pool_ = lux::gapi::vk::CommandPool::Builder()
			.setQueueFamilyIndex(device_context_.asyncComputeQueueFamilyIndex())
			.build(device_context_.logicalDevice(), device_context_.instanceContext().allocator());

		// Create transfer command pool (for dedicated transfer queue)
		transfer_command_pool_ = lux::gapi::vk::CommandPool::Builder()
			.setQueueFamilyIndex(device_context_.transferQueueFamilyIndex())
			.build(device_context_.logicalDevice(), device_context_.instanceContext().allocator());

		return {};
	}

	ResourceContext::~ResourceContext()
	{
		// Release resources in reverse creation order
		transfer_command_pool_.release(device_context_.logicalDevice(), device_context_.instanceContext().allocator());
		compute_command_pool_.release(device_context_.logicalDevice(), device_context_.instanceContext().allocator());
		command_pool_.release(device_context_.logicalDevice(), device_context_.instanceContext().allocator());
		descriptor_pool_.release(device_context_.logicalDevice(), device_context_.instanceContext().allocator());
	}

	VkBool32 debug_report_callback(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType, uint64_t object, size_t location,
		int32_t messageCode, const char* pLayerPrefix, const char* pMessage, void* pUserData)
	{
		DebugCallbackInfo info{
			.flags			= flags,
			.objectType		= objectType,
			.object			= object,
			.location		= location,
			.messageCode	= messageCode,
			.layerPrefix	= pLayerPrefix,
			.message		= pMessage
		};

		auto callback = reinterpret_cast<DebugCallback*>(pUserData);
		return (*callback)(info) ? VK_TRUE : VK_FALSE;
	}
}