#pragma once
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/render/core/GPUResourceTypes.hpp>
#include <lux/engine/function/visibility.h>
#include <vulkan/vulkan.h>
#include <array>
#include <memory>
#include <vector>
#include <optional>

namespace lux::render
{
    // ---------------------------------------------------------------
    // Set-index → GPU resource type mapping (data-driven lookup table).
    // When adding a new EDescriptorSetSlot, add an entry here.
    // ---------------------------------------------------------------
    struct SetSlotMapping
    {
        uint32_t        set_index;
        EGPUResourceType resource_type;
    };

    /// Central mapping table: descriptor set slot → GPU resource type.
    /// Order matches EDescriptorSetSlot values.
    inline constexpr std::array<SetSlotMapping, kDescriptorSetCount> kSetSlotMappings = {{
        { static_cast<uint32_t>(EDescriptorSetSlot::Scene),      EGPUResourceType::Scene },
        { static_cast<uint32_t>(EDescriptorSetSlot::Instance),   EGPUResourceType::Instance },
        { static_cast<uint32_t>(EDescriptorSetSlot::Texture),    EGPUResourceType::Texture },
        { static_cast<uint32_t>(EDescriptorSetSlot::Light),      EGPUResourceType::Light },
        { static_cast<uint32_t>(EDescriptorSetSlot::Material),   EGPUResourceType::Material },
        { static_cast<uint32_t>(EDescriptorSetSlot::Particle),   EGPUResourceType::Particle },
        { static_cast<uint32_t>(EDescriptorSetSlot::Compute),    EGPUResourceType::Compute },
        { static_cast<uint32_t>(EDescriptorSetSlot::VertexPool), EGPUResourceType::VertexPool },
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

    /// Map a named descriptor set slot to the GPU resource type it conventionally carries.
    /// Uses the central kSetSlotMappings table; the slot index is assumed to equal the
    /// EDescriptorSetSlot ordinal value (Scene=0, Instance=1, …).
    constexpr inline EGPUResourceType resourceTypeForSlot(EDescriptorSetSlot slot) noexcept
    {
        const uint32_t idx = static_cast<uint32_t>(slot);
        return (idx < kDescriptorSetCount) ? kSetSlotMappings[idx].resource_type
                                           : EGPUResourceType::Scene; // safe fallback
    }

    class DeviceContext;
    
    class LUX_FUNCTION_PUBLIC GeneralDescriptorSetLayout
    {
    public:
        GeneralDescriptorSetLayout(DeviceContext& device_context) : device_context_(device_context){}
        ~GeneralDescriptorSetLayout();
        
        // Non-copyable, movable
        GeneralDescriptorSetLayout(const GeneralDescriptorSetLayout& other) = delete;
        GeneralDescriptorSetLayout& operator=(const GeneralDescriptorSetLayout& other) = delete;

        GeneralDescriptorSetLayout(GeneralDescriptorSetLayout&& other) noexcept
            : device_context_(other.device_context_)
            , layouts_(other.layouts_)
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
        VkDescriptorSetLayout getLayout(EDescriptorSetSlot slot) const {
            auto idx = static_cast<uint32_t>(slot);
            return (idx < kDescriptorSetCount) ? layouts_[idx] : VK_NULL_HANDLE;
        }
        
        /**
         * @brief Retrieves a descriptor set layout by its index.
         * 
         * @param set_index The index of the set (0 .. kDescriptorSetCount-1).
         * @return VkDescriptorSetLayout The requested layout, or VK_NULL_HANDLE if invalid.
         */
        VkDescriptorSetLayout getLayout(uint32_t set_index) const {
            return (set_index < kDescriptorSetCount) ? layouts_[set_index] : VK_NULL_HANDLE;
        }

        // Convenience getters (for backward compatibility)
        VkDescriptorSetLayout getSceneSetLayout() const    { return getLayout(EDescriptorSetSlot::Scene); }
        VkDescriptorSetLayout getGeneralSetLayout() const  { return getLayout(EDescriptorSetSlot::Instance); }
        VkDescriptorSetLayout getTextureSetLayout() const  { return getLayout(EDescriptorSetSlot::Texture); }
        VkDescriptorSetLayout getLightSetLayout() const    { return getLayout(EDescriptorSetSlot::Light); }
        VkDescriptorSetLayout getMaterialSetLayout() const { return getLayout(EDescriptorSetSlot::Material); }
        VkDescriptorSetLayout getParticleSetLayout() const   { return getLayout(EDescriptorSetSlot::Particle); }
        VkDescriptorSetLayout getComputeSetLayout() const    { return getLayout(EDescriptorSetSlot::Compute); }
        VkDescriptorSetLayout getVertexPoolSetLayout() const { return getLayout(EDescriptorSetSlot::VertexPool); }

        /**
         * @brief Returns a vector containing all managed descriptor set layouts.
         * @return std::vector<VkDescriptorSetLayout> List of all layouts in slot order.
         */
        std::vector<VkDescriptorSetLayout> getAllLayouts() const;

        /// Total number of descriptor set slots managed by this class.
        static constexpr uint32_t setCount() { return kDescriptorSetCount; }

    private:
        DeviceContext& device_context_;
        
        /// Descriptor set layouts indexed by EDescriptorSetSlot.
        std::array<VkDescriptorSetLayout, kDescriptorSetCount> layouts_{};
    };
}