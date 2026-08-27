#pragma once
#include <lux/engine/function/visibility.h>
#include <lux/engine/function/render/client/core/RenderTypes.hpp> // SamplerDesc(中性采样器描述,缓存键)

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
        std::span<const VkDescriptorSetLayoutBinding> bindings;
        std::span<const VkDescriptorBindingFlags> binding_flags{}; // per-binding flags (e.g. UPDATE_AFTER_BIND)
        VkDescriptorSetLayoutCreateFlags flags{0};
        std::string debug_name{};
    };

    /// The one-storage-buffer-read-by-the-vertex-stage shape, spelled once.
    ///
    /// Named after its *shape*, not after any of its users: four features register
    /// exactly this layout for their cull→draw visible-instance set, and each used
    /// to spell the five-field binding out by hand. registerLayout() deduplicates,
    /// so all four already shared one VkDescriptorSetLayout — which is precisely
    /// why the copies were dangerous. Changing one of them (adding a stage, say)
    /// would have handed that feature a *different* layout than the transient sets
    /// its siblings allocate, with nothing anywhere to say the shapes had parted.
    inline constexpr VkDescriptorSetLayoutBinding kStorageBufferVertexBinding{
        0,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        1,
        VK_SHADER_STAGE_VERTEX_BIT,
        nullptr,
    };

    /// @param debug_name Per-user label. Only the first registration's name sticks
    ///        (the layouts dedupe), so treat it as a hint, not an identity.
    [[nodiscard]] inline DescriptorLayoutDesc storageBufferVertexLayout(std::string debug_name)
    {
        return {.bindings = {&kStorageBufferVertexBinding, 1}, .debug_name = std::move(debug_name)};
    }

    // 采样器缓存的键 = core/RenderTypes.hpp 的**中性 SamplerDesc** ——
    // 它早已存在(全字段 + pinclude 的 vk_convert::toVk() 现成转换),三个特性
    // 常用预置也加在它身上。最初这里定义了一个同名新类型,构建撞名才发现 ——
    // **造新类型前先搜同名**,这次撞出的恰是"该复用而不该新造"的正确答案。
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

        /// 按描述取共享采样器 —— 同描述返回同句柄,生命周期随本服务
        /// (设备关停时统一销毁)。取代各特性 init() 里手写的
        /// VkSamplerCreateInfo + vkCreateSampler + FifOwned 三件套。
        [[nodiscard]] VkSampler sampler(const SamplerDesc& desc);

    private:
        struct LayoutEntry
        {
            std::vector<VkDescriptorSetLayoutBinding> bindings{};
            std::vector<VkDescriptorBindingFlags> binding_flags{};
            VkDescriptorSetLayoutCreateFlags flags{0};
            VkDescriptorSetLayout layout{VK_NULL_HANDLE};
            std::string debug_name{};
        };

        VkDevice device_{VK_NULL_HANDLE};
        VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};
        std::vector<LayoutEntry> layouts_{};
        /// 采样器缓存。N ≤ 个位数,线性扫描 —— 不为四个条目上哈希。
        std::vector<std::pair<SamplerDesc, VkSampler>> samplers_{};
    };

} // namespace lux::render
