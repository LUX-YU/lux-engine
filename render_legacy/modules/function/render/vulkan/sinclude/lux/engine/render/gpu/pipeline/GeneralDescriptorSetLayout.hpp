#pragma once
#include <array>
#include <cstdint>
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/render/gpu/lifecycle/DeviceObject.hpp>
#include <lux/engine/render/gpu/lifecycle/GPUResourceTypes.hpp>
#include <memory>
#include <optional>
#include <vulkan/vulkan.h>

// Domain values are sufficient here; shape expansion stays in the implementation.
namespace lux::rdesc
{
    enum class EBindFrequency : uint8_t;
}

namespace lux::render
{
    // ---------------------------------------------------------------
    // Set-index → GPU resource type mapping (data-driven lookup table).
    // When adding a new EDescriptorSetSlot, add an entry here.
    // ---------------------------------------------------------------
    struct SetSlotMapping
    {
        uint32_t set_index;
        EGPUResourceType resource_type;
    };

    /// Central mapping table: descriptor set slot → GPU resource type.
    /// Order matches EDescriptorSetSlot values.
    inline constexpr std::array<SetSlotMapping, kDescriptorSetCount> kSetSlotMappings = {{
        {static_cast<uint32_t>(EDescriptorSetSlot::SCENE), EGPUResourceType::SCENE},
        {static_cast<uint32_t>(EDescriptorSetSlot::INSTANCE), EGPUResourceType::INSTANCE},
        {static_cast<uint32_t>(EDescriptorSetSlot::TEXTURE), EGPUResourceType::TEXTURE},
        {static_cast<uint32_t>(EDescriptorSetSlot::LIGHT), EGPUResourceType::LIGHT},
        {static_cast<uint32_t>(EDescriptorSetSlot::MATERIAL), EGPUResourceType::MATERIAL},
        {static_cast<uint32_t>(EDescriptorSetSlot::PARTICLE), EGPUResourceType::PARTICLE},
        {static_cast<uint32_t>(EDescriptorSetSlot::COMPUTE), EGPUResourceType::COMPUTE},
        {static_cast<uint32_t>(EDescriptorSetSlot::VERTEX_POOL), EGPUResourceType::VERTEX_POOL},
    }};

    /**
     * @brief Maps a descriptor set index to its corresponding GPU resource type.
     *
     * @param set_index The index of the descriptor set.
     * @return std::optional<EGPUResourceType> The corresponding resource type, or std::nullopt if not found.
     */
    constexpr inline std::optional<EGPUResourceType> mapSetIndexToResourceType(uint32_t set_index)
    {
        for (const auto& m : kSetSlotMappings)
        {
            if (m.set_index == set_index)
            {
                return m.resource_type;
            }
        }
        return std::nullopt;
    }

    class DeviceContext;

    class LUX_FUNCTION_PUBLIC GeneralDescriptorSetLayout
    {
    public:
        using CreateResult = Expected<std::unique_ptr<GeneralDescriptorSetLayout>>;

        [[nodiscard]] static CreateResult create(DeviceContext& device_context) noexcept;

        ~GeneralDescriptorSetLayout() noexcept = default;

        // Other services retain references to this shared layout table.
        GeneralDescriptorSetLayout(const GeneralDescriptorSetLayout&) = delete;
        GeneralDescriptorSetLayout& operator=(const GeneralDescriptorSetLayout&) = delete;
        GeneralDescriptorSetLayout(GeneralDescriptorSetLayout&&) = delete;
        GeneralDescriptorSetLayout& operator=(GeneralDescriptorSetLayout&&) = delete;

        /**
         * @brief Retrieves a descriptor set layout by slot enum.
         */
        VkDescriptorSetLayout getLayout(EDescriptorSetSlot slot) const
        {
            auto idx = static_cast<uint32_t>(slot);
            return (idx < kDescriptorSetCount) ? storage_.layouts[idx].get() : VK_NULL_HANDLE;
        }

        /**
         * @brief Retrieves a descriptor set layout by its index.
         *
         * @param set_index The index of the set (0 .. kDescriptorSetCount-1).
         * @return VkDescriptorSetLayout The requested layout, or VK_NULL_HANDLE if invalid.
         */
        VkDescriptorSetLayout getLayout(uint32_t set_index) const
        {
            return (set_index < kDescriptorSetCount) ? storage_.layouts[set_index].get() : VK_NULL_HANDLE;
        }

        // (8 个 "for backward compatibility" 便利访问器全部退役 ——
        //  B1 删了 3 个零调用的;剩余 5 个里 getVertexPoolSetLayout 也是零调用
        //  (唯一"引用"是别处一句注释),其余 4 个各只有一个调用点,已就地内联为
        //  getLayout(EDescriptorSetSlot::X)。按名字的每槽访问器是同一信息的第二份
        //  记录 —— 通用访问器 + 槽位枚举本来就够。)

        /// The domain-merged layout: the layout obtained by folding all
        /// engine sets of the same frequency domain into one set.
        ///
        /// This **coexists** with the per-set layouts above — this is a
        /// temporary dual-track setup during migration: first the domain
        /// layout is built and its shape verified to match, then resource
        /// objects and pipelines switch over to it. Once the switch is
        /// complete, the per-set layouts will have no more users.
        ///
        /// Both the domain and the offset are engine-level constants
        /// (engineSetDomainOffset / domainBindingCount in
        /// EngineSetShapes.hpp), independent of "which sets a given graph
        /// happens to use" — the domain set is a single scene-level
        /// instance, and computing it per graph would make different
        /// graphs produce incompatible layouts.
        ///
        /// PASS_LOCAL doesn't participate in merging (it's the domain of
        /// single-pipeline private sets); passing it returns
        /// VK_NULL_HANDLE.
        [[nodiscard]] VkDescriptorSetLayout getDomainLayout(rdesc::EBindFrequency domain) const noexcept
        {
            const auto i = static_cast<std::size_t>(domain);
            return i < storage_.domain_layouts.size() ? storage_.domain_layouts[i].get() : VK_NULL_HANDLE;
        }

        // ── Bindless capacity: THE single source of truth ────────────────
        // The Texture set's binding counts are what the driver charges a
        // descriptor pool for, so anything that SIZES such a pool must read
        // these rather than re-derive them from device limits. A second
        // derivation that differs by even one clamp produces
        // VK_ERROR_OUT_OF_POOL_MEMORY on any driver that accounts strictly —
        // and nothing at all on one that doesn't, which is how it hides.
        [[nodiscard]] uint32_t bindless2DCount() const noexcept
        {
            return storage_.bindless_2d_count;
        }

        [[nodiscard]] uint32_t bindlessCubeCount() const noexcept
        {
            return storage_.bindless_cube_count;
        }

        /// Addressable-range ceilings, applied on top of the device budget.
        /// A few thousand textures is a realistic working set; the raw device
        /// limit (~1M desktop, ~16.7M on Adreno) would have the bindless
        /// bookkeeping reserve host-side slot arrays for nothing.
        static constexpr uint32_t kBindlessTex2DCeiling = 64u * 1024u;
        static constexpr uint32_t kBindlessCubeCeiling = 256u;

    private:
        struct LayoutStorage
        {
            std::array<DescriptorSetLayoutOwner, kDescriptorSetCount> layouts;
            std::array<DescriptorSetLayoutOwner, 4> domain_layouts;
            uint32_t bindless_2d_count{};
            uint32_t bindless_cube_count{};
        };

        explicit GeneralDescriptorSetLayout(LayoutStorage&& storage) noexcept;

        LayoutStorage storage_;
    };
} // namespace lux::render
