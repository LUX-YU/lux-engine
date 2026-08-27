#pragma once
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/render/gpu/lifecycle/GPUResourceTypes.hpp>
#include <lux/engine/function/visibility.h>
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <optional>

// The frequency-domain enum only ever appears by value in signatures here,
// so a forward declaration is enough (see the note on EngineSetShape below:
// this header is transitively included by a huge number of translation
// units, so it shouldn't drag in the whole LayoutContract chain).
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
        {static_cast<uint32_t>(EDescriptorSetSlot::Scene), EGPUResourceType::Scene},
        {static_cast<uint32_t>(EDescriptorSetSlot::Instance), EGPUResourceType::Instance},
        {static_cast<uint32_t>(EDescriptorSetSlot::Texture), EGPUResourceType::Texture},
        {static_cast<uint32_t>(EDescriptorSetSlot::Light), EGPUResourceType::Light},
        {static_cast<uint32_t>(EDescriptorSetSlot::Material), EGPUResourceType::Material},
        {static_cast<uint32_t>(EDescriptorSetSlot::Particle), EGPUResourceType::Particle},
        {static_cast<uint32_t>(EDescriptorSetSlot::Compute), EGPUResourceType::Compute},
        {static_cast<uint32_t>(EDescriptorSetSlot::VertexPool), EGPUResourceType::VertexPool},
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
                return m.resource_type;
        }
        return std::nullopt;
    }

    class DeviceContext;

    /// Only appears by reference here, so a forward declaration is enough.
    ///
    /// Why not #include EngineSetShapes.hpp: this header is transitively
    /// included by a huge number of translation units (ecs / editor /
    /// etc.); pulling it in would drag along the entire LayoutContract
    /// chain and significantly widen those TUs' include surface. **Tested:
    /// widening it causes compile failures elsewhere** — the source is
    /// UTF-8 without a BOM, and MSVC interprets the Chinese-comment bytes
    /// using codepage 936; some byte sequences end in a byte that decodes
    /// as a backslash, which inside a `//` comment triggers a line
    /// continuation and swallows the next line of code. The real fix is
    /// adding `/utf-8` (or a BOM) repo-wide, which is a separate piece of
    /// work; until then, don't widen the include surface.
    struct EngineSetShape;

    class LUX_FUNCTION_PUBLIC GeneralDescriptorSetLayout
    {
    public:
        GeneralDescriptorSetLayout(DeviceContext& device_context) : device_context_(device_context)
        {
        }
        ~GeneralDescriptorSetLayout();

        // Non-copyable, movable
        GeneralDescriptorSetLayout(const GeneralDescriptorSetLayout& other) = delete;
        GeneralDescriptorSetLayout& operator=(const GeneralDescriptorSetLayout& other) = delete;

        GeneralDescriptorSetLayout(GeneralDescriptorSetLayout&& other) noexcept
            : device_context_(other.device_context_), layouts_(other.layouts_)
        {
            other.layouts_.fill(VK_NULL_HANDLE);
        }

        GeneralDescriptorSetLayout& operator=(GeneralDescriptorSetLayout&& other) noexcept;

        /**
         * @brief Initializes all descriptor set layouts.
         * @return True if initialization is successful, false otherwise.
         */
        bool init();

        /**
         * @brief Retrieves a descriptor set layout by slot enum.
         */
        VkDescriptorSetLayout getLayout(EDescriptorSetSlot slot) const
        {
            auto idx = static_cast<uint32_t>(slot);
            return (idx < kDescriptorSetCount) ? layouts_[idx] : VK_NULL_HANDLE;
        }

        /**
         * @brief Retrieves a descriptor set layout by its index.
         *
         * @param set_index The index of the set (0 .. kDescriptorSetCount-1).
         * @return VkDescriptorSetLayout The requested layout, or VK_NULL_HANDLE if invalid.
         */
        VkDescriptorSetLayout getLayout(uint32_t set_index) const
        {
            return (set_index < kDescriptorSetCount) ? layouts_[set_index] : VK_NULL_HANDLE;
        }

        // (8 个 "for backward compatibility" 便利访问器全部退役 ——
        //  B1 删了 3 个零调用的;剩余 5 个里 getVertexPoolSetLayout 也是零调用
        //  (唯一"引用"是别处一句注释),其余 4 个各只有一个调用点,已就地内联为
        //  getLayout(EDescriptorSetSlot::X)。按名字的每槽访问器是同一信息的第二份
        //  记录 —— 通用访问器 + 槽位枚举本来就够。)

        /// Resolves an engine set's shape into an array of Vulkan bindings,
        /// with binding numbers shifted as a whole by `binding_offset`,
        /// appending to the given output arrays.
        ///
        /// Why this is public: the domain-merged layout has to expand
        /// several sets of the same domain into one array, using the
        /// within-domain offsets that LayoutPlan provides. There are two
        /// details in this resolution that only this function knows —
        /// bindless capacity is device-derived, and Material is a template
        /// entry expanded by family count — and letting the domain-merge
        /// code re-implement the resolution would eventually diverge on
        /// exactly those two points. A divergence like that shows up as
        /// misaligned descriptors, with no Vulkan error at all. So there is
        /// only ever this one implementation.
        void expandEngineSet(
            const EngineSetShape& shape,
            uint32_t binding_offset,
            std::vector<VkDescriptorSetLayoutBinding>& bindings,
            std::vector<VkDescriptorBindingFlags>& flags
        ) const;

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
            return i < domain_layouts_.size() ? domain_layouts_[i] : VK_NULL_HANDLE;
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
            return bindless_2d_count_;
        }
        [[nodiscard]] uint32_t bindlessCubeCount() const noexcept
        {
            return bindless_cube_count_;
        }

        /// Addressable-range ceilings, applied on top of the device budget.
        /// A few thousand textures is a realistic working set; the raw device
        /// limit (~1M desktop, ~16.7M on Adreno) would have the bindless
        /// bookkeeping reserve host-side slot arrays for nothing.
        static constexpr uint32_t kBindlessTex2DCeiling = 64u * 1024u;
        static constexpr uint32_t kBindlessCubeCeiling = 256u;

    private:
        /// Builds the three domain layouts (GLOBAL / BINDLESS / FEATURE).
        /// Called at the end of init.
        bool initDomainLayouts(VkDevice device);

        DeviceContext& device_context_;

        /// Descriptor set layouts indexed by EDescriptorSetSlot.
        std::array<VkDescriptorSetLayout, kDescriptorSetCount> layouts_{};

        /// Device-derived bindless capacity (computed during init, reused
        /// by expandEngineSet).
        uint32_t bindless_2d_count_{1};
        uint32_t bindless_cube_count_{1};

        /// The domain-merged layouts, indexed by EBindFrequency (the
        /// PASS_LOCAL slot is always empty).
        std::array<VkDescriptorSetLayout, 4> domain_layouts_{};
    };
}
