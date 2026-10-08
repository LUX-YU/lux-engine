#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/gapi/vk/Queue.hpp>
#include <memory>
#include <utility>

namespace lux::gapi::vk
{
    /// Native device owner. Release requires all child resources and submitted
    /// work to have finished through their original owners; destruction never waits.
    class LogicalDevice final
    {
    public:
        LogicalDevice() noexcept = default;

        ~LogicalDevice() noexcept
        {
            reset();
        }

        LogicalDevice(const LogicalDevice&) = delete;
        LogicalDevice& operator=(const LogicalDevice&) = delete;

        LogicalDevice(LogicalDevice&& other) noexcept
            : device_(std::exchange(other.device_, VkDevice{})), allocator_(std::exchange(other.allocator_, nullptr))
        {
        }

        LogicalDevice& operator=(LogicalDevice&& other) noexcept
        {
            if (this != std::addressof(other))
            {
                reset();
                device_ = std::exchange(other.device_, VkDevice{});
                allocator_ = std::exchange(other.allocator_, nullptr);
            }
            return *this;
        }

        [[nodiscard]] static lux::cxx::expected<LogicalDevice, VkResult> create(
            VkPhysicalDevice physical_device,
            const VkDeviceCreateInfo& info,
            const VkAllocationCallbacks* allocator = nullptr
        ) noexcept
        {
            VkDevice device{};
            const auto result = vkCreateDevice(physical_device, &info, allocator, &device);
            if (result != VK_SUCCESS)
            {
                return lux::cxx::unexpected(result);
            }
            return LogicalDevice(device, allocator);
        }

        void reset() noexcept
        {
            if (device_)
            {
                vkDestroyDevice(std::exchange(device_, VkDevice{}), allocator_);
            }
        }

        [[nodiscard]] VkResult waitIdle() const noexcept
        {
            return device_ ? vkDeviceWaitIdle(device_) : VK_ERROR_INITIALIZATION_FAILED;
        }

        [[nodiscard]] Queue getQueue(uint32_t family_index, uint32_t queue_index) const noexcept
        {
            return Queue{device_, family_index, queue_index};
        }

        operator VkDevice() const noexcept
        {
            return device_;
        }

        [[nodiscard]] VkDevice handle() const noexcept
        {
            return device_;
        }

    private:
        LogicalDevice(VkDevice device, const VkAllocationCallbacks* allocator) noexcept
            : device_(device), allocator_(allocator)
        {
        }

        VkDevice device_{};
        const VkAllocationCallbacks* allocator_{};
    };
} // namespace lux::gapi::vk
