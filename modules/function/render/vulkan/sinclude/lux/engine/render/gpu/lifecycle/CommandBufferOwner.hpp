#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <vulkan/vulkan.h>

#include <utility>

namespace lux::render
{
    /// Owns one command buffer allocated from a borrowed device and pool.
    /// Both must outlive the owner. Release is legal only before submission or
    /// after the original submission/retirement owner establishes GPU completion.
    class CommandBufferOwner final
    {
    public:
        using CreateResult = lux::cxx::expected<CommandBufferOwner, VkResult>;

        [[nodiscard]] static CreateResult create(
            VkDevice device,
            VkCommandPool pool,
            VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY
        ) noexcept
        {
            VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            info.commandPool = pool;
            info.level = level;
            info.commandBufferCount = 1;
            VkCommandBuffer buffer{};
            const auto result = vkAllocateCommandBuffers(device, &info, &buffer);
            if (result != VK_SUCCESS)
            {
                return lux::cxx::unexpected(result);
            }
            return CommandBufferOwner(device, pool, buffer);
        }

        CommandBufferOwner() noexcept = default;

        ~CommandBufferOwner() noexcept
        {
            reset();
        }

        CommandBufferOwner(const CommandBufferOwner&) = delete;
        CommandBufferOwner& operator=(const CommandBufferOwner&) = delete;

        CommandBufferOwner(CommandBufferOwner&& other) noexcept
            : device_(std::exchange(other.device_, {})), pool_(std::exchange(other.pool_, {})),
              buffer_(std::exchange(other.buffer_, {}))
        {
        }

        CommandBufferOwner& operator=(CommandBufferOwner&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                device_ = std::exchange(other.device_, {});
                pool_ = std::exchange(other.pool_, {});
                buffer_ = std::exchange(other.buffer_, {});
            }
            return *this;
        }

        [[nodiscard]] VkCommandBuffer get() const noexcept
        {
            return buffer_;
        }

        explicit operator bool() const noexcept
        {
            return buffer_ != VK_NULL_HANDLE;
        }

        void reset() noexcept
        {
            if (buffer_ != VK_NULL_HANDLE)
            {
                vkFreeCommandBuffers(device_, pool_, 1, &buffer_);
                buffer_ = VK_NULL_HANDLE;
                pool_ = VK_NULL_HANDLE;
                device_ = VK_NULL_HANDLE;
            }
        }

    private:
        CommandBufferOwner(VkDevice device, VkCommandPool pool, VkCommandBuffer buffer) noexcept
            : device_(device), pool_(pool), buffer_(buffer)
        {
        }

        VkDevice device_{};
        VkCommandPool pool_{};
        VkCommandBuffer buffer_{};
    };
} // namespace lux::render
