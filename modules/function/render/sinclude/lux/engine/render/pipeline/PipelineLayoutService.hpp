#pragma once
#include <lux/engine/function/visibility.h>
#include <lux/engine/render/core/Errors.hpp>

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
        explicit PipelineLayoutService(VkDevice device);
        ~PipelineLayoutService();

        PipelineLayoutService(const PipelineLayoutService &) = delete;
        PipelineLayoutService &operator=(const PipelineLayoutService &) = delete;

        [[nodiscard]] Expected<VkPipelineLayout> getOrCreate(const PipelineLayoutDesc &desc);

    private:
        struct Entry
        {
            std::vector<VkDescriptorSetLayout> set_layouts{};
            std::vector<VkPushConstantRange> push_constants{};
            VkPipelineLayout layout{VK_NULL_HANDLE};
            std::string debug_name{};
        };

        VkDevice device_{VK_NULL_HANDLE};
        std::vector<Entry> entries_{};
    };

} // namespace lux::render
