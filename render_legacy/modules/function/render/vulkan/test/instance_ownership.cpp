#define VMA_IMPLEMENTATION
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <type_traits>
#include <vk_mem_alloc.h>

namespace
{
    std::map<VkInstance, const VkAllocationCallbacks*> instances;

    struct Report
    {
        VkInstance instance;
        const VkAllocationCallbacks* allocator;
        VkDebugReportCallbackCreateInfoEXT info;
    };

    std::map<VkDebugReportCallbackEXT, Report> reports;
    unsigned create_count{}, destroy_count{}, report_calls{}, callback_count{}, query_calls{};
    unsigned fail_query{}, incomplete_fills{};
    bool fail_create{}, fail_report{}, omit_layers{}, omit_report_destroy{};

    VkResult VKAPI_CALL
    createInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator, VkInstance* output)
    {
        ++create_count;
        if (fail_create)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        const auto result = vkCreateInstance(info, allocator, output);
        if (result == VK_SUCCESS)
        {
            assert(instances.emplace(*output, allocator).second);
        }
        return result;
    }

    void VKAPI_CALL destroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator)
    {
        assert(instances.contains(instance) && instances.at(instance) == allocator);
        for (const auto& [handle, report] : reports)
        {
            assert(report.instance != instance);
        }
        instances.erase(instance);
        ++destroy_count;
        vkDestroyInstance(instance, allocator);
    }

    VkResult VKAPI_CALL extensions(const char* layer, uint32_t* count, VkExtensionProperties* values)
    {
        if (++query_calls == fail_query)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        if (values && incomplete_fills)
        {
            --incomplete_fills;
            return VK_INCOMPLETE;
        }
        return vkEnumerateInstanceExtensionProperties(layer, count, values);
    }

    VkResult VKAPI_CALL layers(uint32_t* count, VkLayerProperties* values)
    {
        if (++query_calls == fail_query)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        if (omit_layers)
        {
            *count = 0;
            return VK_SUCCESS;
        }
        return vkEnumerateInstanceLayerProperties(count, values);
    }

    VkResult VKAPI_CALL physicalDevices(VkInstance instance, uint32_t* count, VkPhysicalDevice* values)
    {
        if (++query_calls == fail_query)
        {
            return VK_ERROR_DEVICE_LOST;
        }
        return vkEnumeratePhysicalDevices(instance, count, values);
    }

    void invoke(const VkDebugReportCallbackCreateInfoEXT& info)
    {
        info.pfnCallback(
            VK_DEBUG_REPORT_WARNING_BIT_EXT,
            VK_DEBUG_REPORT_OBJECT_TYPE_UNKNOWN_EXT,
            0,
            0,
            0,
            "lifetime-test",
            "stable callback storage",
            info.pUserData
        );
    }

    VkResult VKAPI_CALL createReport(
        VkInstance instance,
        const VkDebugReportCallbackCreateInfoEXT* info,
        const VkAllocationCallbacks* allocator,
        VkDebugReportCallbackEXT* output
    )
    {
        ++report_calls;
        invoke(*info); // Native entry may synchronously invoke the supplied callback.
        if (fail_report)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        const auto create = reinterpret_cast<PFN_vkCreateDebugReportCallbackEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugReportCallbackEXT")
        );
        assert(create);
        const auto result = create(instance, info, allocator, output);
        if (result == VK_SUCCESS)
        {
            assert(reports.emplace(*output, Report{instance, allocator, *info}).second);
        }
        return result;
    }

    void VKAPI_CALL
    destroyReport(VkInstance instance, VkDebugReportCallbackEXT report, const VkAllocationCallbacks* allocator)
    {
        const auto found = reports.find(report);
        assert(found != reports.end());
        assert(found->second.instance == instance && found->second.allocator == allocator);
        invoke(found->second.info); // Verify stable callback backing through release, too.
        reports.erase(found);
        const auto destroy = reinterpret_cast<PFN_vkDestroyDebugReportCallbackEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugReportCallbackEXT")
        );
        assert(destroy);
        destroy(instance, report, allocator);
    }

    PFN_vkVoidFunction VKAPI_CALL instanceProc(VkInstance instance, const char* name)
    {
        if (std::strcmp(name, "vkCreateDebugReportCallbackEXT") == 0)
        {
            return reinterpret_cast<PFN_vkVoidFunction>(&createReport);
        }
        if (std::strcmp(name, "vkDestroyDebugReportCallbackEXT") == 0)
        {
            return omit_report_destroy ? nullptr : reinterpret_cast<PFN_vkVoidFunction>(&destroyReport);
        }
        return vkGetInstanceProcAddr(instance, name);
    }
} // namespace

// The production providers are compiled at their native boundary. Successful
// acquisition/release uses real Vulkan; injected faults preserve native VkResult.
// clang-format off
#define vkCreateInstance createInstance
#define vkDestroyInstance destroyInstance
#define vkEnumerateInstanceExtensionProperties extensions
#define vkEnumerateInstanceLayerProperties layers
#define vkEnumeratePhysicalDevices physicalDevices
#define vkGetInstanceProcAddr instanceProc
#include "../src/gpu/memory/VmaTypes.cpp"
#include "../src/gpu/VulkanContext.cpp"
#undef vkGetInstanceProcAddr
#undef vkEnumeratePhysicalDevices
#undef vkEnumerateInstanceLayerProperties
#undef vkEnumerateInstanceExtensionProperties
#undef vkDestroyInstance
#undef vkCreateInstance
// clang-format on

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    using namespace lux::render;
    using lux::gapi::vk::DebugReport;
    using lux::gapi::vk::Instance;
    static_assert(!std::is_copy_constructible_v<Instance> && !std::is_copy_assignable_v<Instance>);
    static_assert(std::is_nothrow_move_constructible_v<Instance> && std::is_nothrow_move_assignable_v<Instance>);
    static_assert(!std::is_copy_constructible_v<DebugReport> && !std::is_copy_assignable_v<DebugReport>);
    static_assert(std::is_nothrow_move_constructible_v<DebugReport> && std::is_nothrow_move_assignable_v<DebugReport>);
    static_assert(!std::is_default_constructible_v<InstanceContext>);
    static_assert(!std::is_copy_constructible_v<InstanceContext> && !std::is_move_constructible_v<InstanceContext>);
    auto check_failure = [](const auto& result, VkResult error)
    {
        assert(!result && isError<err::device::VulkanCallFailed>(result.error()));
        assert(result.error().args[0] == encodeVkResult(error));
        assert(instances.empty() && reports.empty());
    };
    auto callback = [](const DebugCallbackInfo& info)
    {
        if (std::strcmp(info.layerPrefix, "lifetime-test") == 0)
        {
            ++callback_count;
        }
        return false;
    };
    const char* missing = "VK_LUX_nonexistent_instance_extension";
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = &missing;
    const auto leaf_failed = Instance::create(info);
    assert(!leaf_failed && leaf_failed.error() == VK_ERROR_EXTENSION_NOT_PRESENT);
    check_failure(InstanceContext::create({missing}), VK_ERROR_EXTENSION_NOT_PRESENT);
    assert(!InstanceContext::create({nullptr}));
    assert(!InstanceContext::create({""}));

    // Count and fill failures: instance extensions followed by validation layers.
    for (unsigned failure = 1; failure <= 4; ++failure)
    {
        query_calls = 0;
        fail_query = failure;
        const auto before = create_count;
        check_failure(InstanceContext::create({}, callback), VK_ERROR_OUT_OF_HOST_MEMORY);
        assert(create_count == before);
    }
    fail_query = 0;
    incomplete_fills = 3;
    check_failure(InstanceContext::create({}), VK_INCOMPLETE);
    incomplete_fills = 1;
    {
        auto retry = InstanceContext::create({});
        assert(retry && incomplete_fills == 0);
    }
    fail_create = true;
    check_failure(InstanceContext::create({}), VK_ERROR_OUT_OF_HOST_MEMORY);
    fail_create = false;
    omit_layers = true;
    {
        const auto before = report_calls;
        auto release = InstanceContext::create({}, callback);
        assert(release && report_calls == before);
        assert(!(*release)->isInstanceExtensionEnabled(VK_EXT_DEBUG_REPORT_EXTENSION_NAME));
    }
    omit_layers = false;

    // Installed validation is required by this GPU qualifier, not by production startup.
    const auto available_layers = Instance::layerProperties();
    assert(available_layers);
    assert(std::ranges::any_of(
        *available_layers,
        [](const auto& entry) { return std::strcmp(entry.layerName, "VK_LAYER_KHRONOS_validation") == 0; }
    ));
    fail_report = true;
    check_failure(InstanceContext::create({}, callback), VK_ERROR_OUT_OF_HOST_MEMORY);
    fail_report = false;
    omit_report_destroy = true;
    {
        const auto before = report_calls;
        check_failure(InstanceContext::create({}, callback), VK_ERROR_EXTENSION_NOT_PRESENT);
        assert(report_calls == before);
    }
    omit_report_destroy = false;
    {
        const auto callbacks = callback_count;
        auto context = InstanceContext::create({}, callback);
        assert(context && instances.size() == 1 && reports.size() == 1);
        assert(callback_count == callbacks + 1);
        assert((*context)->isInstanceExtensionEnabled(VK_KHR_SURFACE_EXTENSION_NAME));
        assert((*context)->isInstanceExtensionEnabled(VK_EXT_DEBUG_REPORT_EXTENSION_NAME));
        for (unsigned failure = 1; failure <= 2; ++failure)
        {
            query_calls = 0;
            fail_query = failure;
            const auto failed = (*context)->instance().listPhysicalDevices();
            assert(!failed && failed.error() == VK_ERROR_DEVICE_LOST);
        }
        fail_query = 0;
        const auto devices = (*context)->instance().listPhysicalDevices();
        assert(devices && !devices->empty());
        context->reset();
        assert(callback_count == callbacks + 2);
    }
    {
        auto context = InstanceContext::create({}, callback);
        assert(context && reports.size() == 1);
        VkDebugReportCallbackCreateInfoEXT report_info{};
        report_info.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
        report_info.flags = VK_DEBUG_REPORT_WARNING_BIT_EXT;
        report_info.pfnCallback = &lux::render::debug_report_callback;
        DebugCallback owned_callback = callback;
        report_info.pUserData = &owned_callback;
        auto first = DebugReport::create((*context)->instance(), report_info);
        auto second = DebugReport::create((*context)->instance(), report_info);
        assert(first && second && reports.size() == 3);
        const auto kept = second->handle();
        *first = std::move(*second);
        assert(first->handle() == kept && !second->handle() && reports.size() == 2);
        *first = std::move(*first);
        DebugReport moved(std::move(*first));
        assert(moved.handle() == kept && !first->handle());
        moved.reset();
        moved.reset();
        assert(reports.size() == 1);
    }
    info.enabledExtensionCount = 0;
    info.ppEnabledExtensionNames = nullptr;
    {
        auto first = Instance::create(info);
        auto second = Instance::create(info);
        assert(first && second && instances.size() == 2);
        const auto kept = second->handle();
        *first = std::move(*second);
        assert(first->handle() == kept && !second->handle() && instances.size() == 1);
        *first = std::move(*first);
        assert(first->handle() == kept && instances.size() == 1);
        Instance moved(std::move(*first));
        assert(moved.handle() == kept && !first->handle());
        moved.reset();
        moved.reset();
        assert(instances.empty());
    }
    assert(instances.empty() && reports.empty());
    std::printf(
        "PASS: native instance failure; complete prefix; discovery errors/retry; optional validation; stable callback; "
        "owner move/reset. destroyed=%u callbacks=%u\n",
        destroy_count,
        callback_count
    );
}
