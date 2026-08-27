#pragma once
#include <lux/engine/function/visibility.h>
#include <lux/engine/function/render/client/core/Errors.hpp>

#include <vulkan/vulkan.h>

#include <span>
#include <string>
#include <vector>

namespace lux::render
{
    struct PipelineLayoutDesc
    {
        std::span<const VkDescriptorSetLayout> set_layouts;
        std::span<const VkPushConstantRange> push_constants;
        std::string debug_name{};
    };

    class LUX_FUNCTION_PUBLIC PipelineLayoutService
    {
    public:
        /// @param max_bound_descriptor_sets Device limit
        ///        (VkPhysicalDeviceLimits::maxBoundDescriptorSets). getOrCreate
        ///        fails fast with a readable error when a layout would exceed it —
        ///        mobile GPUs (Mali) commonly report 4, where an oversized layout
        ///        violates VUID-VkPipelineLayoutCreateInfo-setLayoutCount-00286
        ///        instead of failing cleanly.
        PipelineLayoutService(VkDevice device, uint32_t max_bound_descriptor_sets);
        ~PipelineLayoutService();

        PipelineLayoutService(const PipelineLayoutService&) = delete;
        PipelineLayoutService& operator=(const PipelineLayoutService&) = delete;

        [[nodiscard]] Expected<VkPipelineLayout> getOrCreate(const PipelineLayoutDesc& desc);

        /// The device's maxBoundDescriptorSets (0 = unknown/unbounded). Used
        /// as the budget criterion for the ≤4-set projection — this limit
        /// is itself the hard constraint that mobile compatibility has to
        /// satisfy.
        [[nodiscard]] uint32_t maxBoundDescriptorSets() const noexcept
        {
            return max_bound_descriptor_sets_;
        }

    private:
        struct Entry
        {
            std::vector<VkDescriptorSetLayout> set_layouts{};
            std::vector<VkPushConstantRange> push_constants{};
            VkPipelineLayout layout{VK_NULL_HANDLE};
            std::string debug_name{};
        };

        VkDevice device_{VK_NULL_HANDLE};
        uint32_t max_bound_descriptor_sets_{0};
        std::vector<Entry> entries_{};
    };

} // namespace lux::render
