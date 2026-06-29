#pragma once
#include <memory>
#include <vector>
#include <cstring>
#include "lux/engine/gapi/vk/Object.hpp"
#include "lux/engine/gapi/RenderInstance.hpp"
#include "lux/engine/gapi/vk/PhysicalDevice.hpp"
#include <vulkan/vulkan.h>

namespace lux::gapi::vk
{
	class Instance;
	class DebugReport
	{
	public:
		DebugReport() : debug_report(VK_NULL_HANDLE) {}

		DebugReport(VkInstance instance, PFN_vkDebugReportCallbackEXT cb, void* user_data = nullptr, VkAllocationCallbacks* allocator = nullptr)
		{
			auto vkCreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugReportCallbackEXT");
			VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
			debug_report_ci.sType		= VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
			debug_report_ci.pNext		= nullptr;
			debug_report_ci.flags		= VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
			debug_report_ci.pfnCallback = cb;
			debug_report_ci.pUserData	= user_data;
			VK_FUNC_INVOKE(vkCreateDebugReportCallbackEXT, "Failed to create DebugReport instance", instance, &debug_report_ci, allocator, &debug_report);
		}

		DebugReport(VkInstance instance, VkDebugReportCallbackCreateInfoEXT& ci, VkAllocationCallbacks* allocator = nullptr)
		{
			auto vkCreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugReportCallbackEXT");
			VK_FUNC_INVOKE(vkCreateDebugReportCallbackEXT, "Failed to create DebugReport instance", instance, &ci, allocator, &debug_report);
		}

		DebugReport(const DebugReport&) = delete;
		DebugReport& operator=(const DebugReport&) = delete;

		DebugReport(DebugReport&& other) noexcept
		{
			debug_report = other.debug_report;
			other.debug_report = VK_NULL_HANDLE;
		}

		DebugReport& operator=(DebugReport&& other) noexcept
		{
			debug_report = other.debug_report;
			other.debug_report = VK_NULL_HANDLE;
			return *this;
		}

		void release(VkInstance instance, VkAllocationCallbacks* allocator = nullptr)
		{
			if (debug_report)
			{
				auto vkDestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugReportCallbackEXT");
				vkDestroyDebugReportCallbackEXT(instance, debug_report, allocator);
			}
		}

		inline operator VkDebugReportCallbackEXT() const noexcept { return debug_report; }
		inline const VkDebugReportCallbackEXT* operator&() const noexcept { return &debug_report; }

		inline VkDebugReportCallbackEXT handle() const noexcept { return debug_report; }
		inline const VkDebugReportCallbackEXT* handlePtr() const noexcept { return &debug_report; }

	private:

		VkDebugReportCallbackEXT  debug_report;
	};

	class InstanceBuilder;
	class Instance
	{
	public:
		using Builder = InstanceBuilder;

		Instance() : instance(VK_NULL_HANDLE){}

		Instance(const VkInstanceCreateInfo& ci, VkAllocationCallbacks* allocator = nullptr)
		{
			auto err = vkCreateInstance(&ci, allocator, &instance);
			// VK_FUNC_INVOKE(vkCreateInstance, "Failed to create Instance object", &ci, allocator, &instance)
		}

		Instance(const Instance&) = delete;
		Instance& operator=(const Instance&) = delete;

		Instance(Instance&& other) noexcept
		{
			instance			= other.instance;
			other.instance		= VK_NULL_HANDLE;
		}

		Instance& operator=(Instance&& other) noexcept
		{
			instance			= other.instance;
			other.instance		= VK_NULL_HANDLE;
			return *this;
		}

		void release(VkAllocationCallbacks* allocator = nullptr)
		{
			if (instance != VK_NULL_HANDLE)
			{
				vkDestroyInstance(instance, allocator);
			}
		}

		static std::vector<VkExtensionProperties> extensionProperties()
		{
			uint32_t properties_count;
			std::vector<VkExtensionProperties> properties;
			vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, nullptr);
			properties.resize(properties_count);
			vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.data());
			return properties;
		}

		// only works if the VK_EXT_debug_report extension is enabled
		// see InstanceBuilder::enableDebugReport()
		DebugReport createDebugReport(PFN_vkDebugReportCallbackEXT cb, void* user_data = nullptr, VkAllocationCallbacks* allocator = nullptr)
		{
			if (!cb) return DebugReport{};

			VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
			debug_report_ci.sType		= VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
			debug_report_ci.pNext		= nullptr;
			debug_report_ci.flags		= VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
			debug_report_ci.pfnCallback = cb;
			debug_report_ci.pUserData	= user_data;

			return DebugReport{ instance, debug_report_ci, allocator };
		}

		std::vector<PhysicalDevice> listPhysicalDevices() const
		{
			VkResult err;
			uint32_t device_count;
			vkEnumeratePhysicalDevices(instance, &device_count, nullptr);

			std::vector<VkPhysicalDevice> physical_devices(device_count);
			vkEnumeratePhysicalDevices(instance, &device_count, physical_devices.data());

			std::vector<PhysicalDevice> ret_physical_devices;
			for (VkPhysicalDevice& device : physical_devices)
			{
				PhysicalDevice new_device(device);

				ret_physical_devices.push_back(std::move(new_device));
			}

			return ret_physical_devices;
		}

		inline operator VkInstance() const { return instance; }
		inline const VkInstance* operator&() const noexcept { return &instance; }

		inline VkInstance handle() const noexcept { return instance; }
		inline const VkInstance* handlePtr() const noexcept { return &instance; }

		VkInstance	instance{ VK_NULL_HANDLE };
	};

	class InstanceBuilder
	{
	public:
		InstanceBuilder()
		{
			app_info.sType					= VK_STRUCTURE_TYPE_APPLICATION_INFO;
			app_info.pApplicationName		= "Default";
			app_info.applicationVersion		= VK_MAKE_VERSION(1, 3, 0);
			app_info.pEngineName			= "Default";
			app_info.engineVersion			= VK_MAKE_VERSION(1, 3, 0);
			app_info.apiVersion				= VK_API_VERSION_1_3;
			app_info.pNext					= nullptr;
			create_info.sType				= VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
			// Enumerate available extensions
			create_info.pApplicationInfo	= &app_info;
			create_info.pNext				= nullptr;
			create_info.flags				= 0;
			create_info.enabledLayerCount		= 0;
			create_info.ppEnabledLayerNames		= nullptr;

			// enable the VK_KHR_surface extension if it is supported
			auto supported_extensions = Instance::extensionProperties();
			for (const auto& ext : supported_extensions) {
				if (strcmp(ext.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0) {
					extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
					break;
				}
			}
		}

		inline InstanceBuilder& setAppName(const char* name) noexcept
		{
			app_info.pApplicationName = name;
			return *this;
		}

		inline InstanceBuilder& setEngineName(const char* name) noexcept
		{
			app_info.pEngineName = name;
			return *this;
		}

		inline InstanceBuilder& setAppVersion(uint32_t version) noexcept
		{
			app_info.applicationVersion = version;
			return *this;
		}

		inline InstanceBuilder& setEngineVersion(uint32_t version) noexcept
		{
			app_info.engineVersion = version;
			return *this;
		}

		inline InstanceBuilder& setApiVersion(uint32_t version) noexcept
		{
			app_info.apiVersion = version;
			return *this;
		}

		inline InstanceBuilder& enableDebugReport() noexcept
		{
			layers.push_back("VK_LAYER_KHRONOS_validation");
			extensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
			// VK_EXT_debug_utils is needed for vkCmdBeginDebugUtilsLabelEXT (pass
			// name labels in command buffers) and vkSetDebugUtilsObjectNameEXT
			// (human-readable pipeline/buffer names in validation messages).
			// It is always available alongside the validation layers on LunarG SDK
			// 1.1.106+ / Vulkan 1.3 targets.
			extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			create_info.enabledLayerCount = 1;
			create_info.ppEnabledLayerNames = layers.data();

			return *this;
		}

		inline InstanceBuilder& addExtension(const char* extension) noexcept
		{
			extensions.push_back(extension);
			return *this;
		}

		inline InstanceBuilder& addExtensions(const std::vector<const char*>& extensions) noexcept
		{
			this->extensions.insert(this->extensions.end(), extensions.begin(), extensions.end());
			return *this;
		}

		Instance build(VkAllocationCallbacks* allocator = nullptr)
		{
			create_info.enabledExtensionCount	= (uint32_t)extensions.size();
			create_info.ppEnabledExtensionNames = extensions.data();

			return Instance{ create_info, allocator };
		}

	private:
		std::vector<const char*>			layers;
		std::vector<const char*>			extensions;
		VkApplicationInfo					app_info;
		VkInstanceCreateInfo				create_info;
	};
}
