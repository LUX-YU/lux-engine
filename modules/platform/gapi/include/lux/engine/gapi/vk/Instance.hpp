#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/gapi/vk/PhysicalDevice.hpp>
#include <memory>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

namespace lux::gapi::vk
{
    namespace detail
    {
        // Instance discovery can change between count and fill. Bound retries and
        // preserve VK_INCOMPLETE instead of publishing an incomplete discovery set.
        template <class T, class Query>
        [[nodiscard]] lux::cxx::expected<std::vector<T>, VkResult> enumerateInstanceValues(Query query) noexcept
        {
            for (unsigned attempt = 0; attempt != 3; ++attempt)
            {
                uint32_t count{};
                const auto counted = query(&count, nullptr);
                if (counted != VK_SUCCESS)
                {
                    return lux::cxx::unexpected(counted);
                }
                if (!count)
                {
                    return std::vector<T>{};
                }
                std::vector<T> values(count);
                const auto filled = query(&count, values.data());
                if (filled == VK_SUCCESS)
                {
                    values.resize(count);
                    return values;
                }
                if (filled != VK_INCOMPLETE)
                {
                    return lux::cxx::unexpected(filled);
                }
            }
            return lux::cxx::unexpected(VK_INCOMPLETE);
        }
    } // namespace detail

    /// Native leaf owner. The instance and allocation callbacks outlive this report.
    class DebugReport final
    {
    public:
        DebugReport() noexcept = default;

        ~DebugReport() noexcept
        {
            reset();
        }

        DebugReport(const DebugReport&) = delete;
        DebugReport& operator=(const DebugReport&) = delete;

        DebugReport(DebugReport&& other) noexcept
            : instance_(std::exchange(other.instance_, VkInstance{})),
              report_(std::exchange(other.report_, VkDebugReportCallbackEXT{})),
              allocator_(std::exchange(other.allocator_, nullptr)), destroy_(std::exchange(other.destroy_, nullptr))
        {
        }

        DebugReport& operator=(DebugReport&& other) noexcept
        {
            if (this != std::addressof(other))
            {
                reset();
                instance_ = std::exchange(other.instance_, VkInstance{});
                report_ = std::exchange(other.report_, VkDebugReportCallbackEXT{});
                allocator_ = std::exchange(other.allocator_, nullptr);
                destroy_ = std::exchange(other.destroy_, nullptr);
            }
            return *this;
        }

        [[nodiscard]] static lux::cxx::expected<DebugReport, VkResult> create(
            VkInstance instance,
            const VkDebugReportCallbackCreateInfoEXT& info,
            const VkAllocationCallbacks* allocator = nullptr
        ) noexcept
        {
            const auto create = reinterpret_cast<PFN_vkCreateDebugReportCallbackEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugReportCallbackEXT")
            );
            const auto destroy = reinterpret_cast<PFN_vkDestroyDebugReportCallbackEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugReportCallbackEXT")
            );
            // Optional diagnostics are negotiated by the consumer. An explicitly
            // requested native report must have both creation and release endpoints.
            const bool is_unavailable = !create || !destroy;
            if (is_unavailable)
            {
                return lux::cxx::unexpected(VK_ERROR_EXTENSION_NOT_PRESENT);
            }
            VkDebugReportCallbackEXT report{};
            const auto result = create(instance, &info, allocator, &report);
            if (result != VK_SUCCESS)
            {
                return lux::cxx::unexpected(result);
            }
            return DebugReport(instance, report, allocator, destroy);
        }

        void reset() noexcept
        {
            if (report_)
            {
                destroy_(instance_, std::exchange(report_, VkDebugReportCallbackEXT{}), allocator_);
            }
        }

        [[nodiscard]] VkDebugReportCallbackEXT handle() const noexcept
        {
            return report_;
        }

    private:
        DebugReport(
            VkInstance instance,
            VkDebugReportCallbackEXT report,
            const VkAllocationCallbacks* allocator,
            PFN_vkDestroyDebugReportCallbackEXT destroy
        ) noexcept
            : instance_(instance), report_(report), allocator_(allocator), destroy_(destroy)
        {
        }

        VkInstance instance_{};
        VkDebugReportCallbackEXT report_{};
        const VkAllocationCallbacks* allocator_{};
        PFN_vkDestroyDebugReportCallbackEXT destroy_{};
    };

    /// Native leaf owner. Allocation callbacks and all child-release dependencies
    /// obey Vulkan lifetime rules; configuration policy belongs to the consumer.
    class Instance final
    {
    public:
        Instance() noexcept = default;

        ~Instance() noexcept
        {
            reset();
        }

        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;

        Instance(Instance&& other) noexcept
            : instance_(std::exchange(other.instance_, VkInstance{})),
              allocator_(std::exchange(other.allocator_, nullptr))
        {
        }

        Instance& operator=(Instance&& other) noexcept
        {
            if (this != std::addressof(other))
            {
                reset();
                instance_ = std::exchange(other.instance_, VkInstance{});
                allocator_ = std::exchange(other.allocator_, nullptr);
            }
            return *this;
        }

        [[nodiscard]] static lux::cxx::expected<Instance, VkResult> create(
            const VkInstanceCreateInfo& info,
            const VkAllocationCallbacks* allocator = nullptr
        ) noexcept
        {
            VkInstance instance{};
            const auto result = vkCreateInstance(&info, allocator, &instance);
            if (result != VK_SUCCESS)
            {
                return lux::cxx::unexpected(result);
            }
            return Instance(instance, allocator);
        }

        void reset() noexcept
        {
            if (instance_)
            {
                vkDestroyInstance(std::exchange(instance_, VkInstance{}), allocator_);
            }
        }

        [[nodiscard]] static lux::cxx::expected<std::vector<VkExtensionProperties>, VkResult> extensionProperties(
        ) noexcept
        {
            return detail::enumerateInstanceValues<VkExtensionProperties>(
                [](uint32_t* count, VkExtensionProperties* values) noexcept
                { return vkEnumerateInstanceExtensionProperties(nullptr, count, values); }
            );
        }

        [[nodiscard]] static lux::cxx::expected<std::vector<VkLayerProperties>, VkResult> layerProperties() noexcept
        {
            return detail::enumerateInstanceValues<VkLayerProperties>(vkEnumerateInstanceLayerProperties);
        }

        [[nodiscard]] lux::cxx::expected<std::vector<PhysicalDevice>, VkResult> listPhysicalDevices() const noexcept
        {
            auto devices = detail::enumerateInstanceValues<VkPhysicalDevice>(
                [this](uint32_t* count, VkPhysicalDevice* values) noexcept
                { return vkEnumeratePhysicalDevices(instance_, count, values); }
            );
            if (!devices)
            {
                return lux::cxx::unexpected(devices.error());
            }
            std::vector<PhysicalDevice> result;
            result.reserve(devices->size());
            for (auto device : *devices)
            {
                result.push_back(PhysicalDevice(device));
            }
            return result;
        }

        operator VkInstance() const noexcept
        {
            return instance_;
        }

        [[nodiscard]] VkInstance handle() const noexcept
        {
            return instance_;
        }

    private:
        Instance(VkInstance instance, const VkAllocationCallbacks* allocator) noexcept
            : instance_(instance), allocator_(allocator)
        {
        }

        VkInstance instance_{};
        const VkAllocationCallbacks* allocator_{};
    };
} // namespace lux::gapi::vk
