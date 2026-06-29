#pragma once
#include <lux/engine/function/visibility.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lux::render
{

using DescriptorLayoutId = uint32_t;
static constexpr DescriptorLayoutId kInvalidDescriptorLayoutId = ~uint32_t{0};

struct DescriptorLayoutDesc
{
    std::span<const VkDescriptorSetLayoutBinding>  bindings;
    std::span<const VkDescriptorBindingFlags>      binding_flags{};  // per-binding flags (e.g. UPDATE_AFTER_BIND)
    VkDescriptorSetLayoutCreateFlags               flags{0};
    std::string                                    debug_name{};
};

class LUX_FUNCTION_PUBLIC DescriptorService
{
public:
    DescriptorService(VkDevice device, VkDescriptorPool descriptor_pool);
    ~DescriptorService();

    DescriptorService(const DescriptorService&) = delete;
    DescriptorService& operator=(const DescriptorService&) = delete;

    [[nodiscard]] DescriptorLayoutId registerLayout(const DescriptorLayoutDesc& desc);
    [[nodiscard]] VkDescriptorSetLayout layout(DescriptorLayoutId id) const noexcept;

    [[nodiscard]] VkDescriptorSet allocate(DescriptorLayoutId layout_id, uint32_t variable_count = 0) const;

private:
    struct LayoutEntry
    {
        std::vector<VkDescriptorSetLayoutBinding> bindings{};
        std::vector<VkDescriptorBindingFlags>     binding_flags{};
        VkDescriptorSetLayoutCreateFlags          flags{0};
        VkDescriptorSetLayout                     layout{VK_NULL_HANDLE};
        std::string                               debug_name{};
    };

    VkDevice                      device_{VK_NULL_HANDLE};
    VkDescriptorPool              descriptor_pool_{VK_NULL_HANDLE};
    std::vector<LayoutEntry>      layouts_{};
};

} // namespace lux::render

