#pragma once

#include <lux/engine/description/PassContract.hpp>
#include <lux/engine/render/vulkan/device/Device.hpp>
#include <string>
#include <vector>

namespace lux::render::vulkan
{
    struct ShaderOwnerTag;
    using ShaderOwnerId = cxx::StrongId<ShaderOwnerTag, std::uint64_t, 0>;

    // Cold owner declaration. Logical identity never comes from provisional set numbers.
    struct OwnerField
    {
        std::string semantic;
        VkDescriptorType type;
        std::uint32_t count, stages;
        VkDescriptorBindingFlags flags{};
        std::uint32_t element_stride{}, element_alignment{1};
        std::string dimension, image_format;
        bool operator==(const OwnerField&) const noexcept = default;
    };

    struct OwnerShape
    {
        ShaderOwnerId identity;
        std::string canonical_name;
        std::uint32_t revision;
        rdesc::EFieldOwner category;
        std::vector<OwnerField> fields;
        bool operator==(const OwnerShape&) const noexcept = default;
    };

    // Full schemas supplied by the actual owner, not a reflected shader subset.
    [[nodiscard]] RenderResult<OwnerShape> declareOwnerShape(
        std::string_view name,
        std::uint32_t revision,
        rdesc::EFieldOwner category,
        std::span<const rdesc::PassShaderContract> complete_schemas
    ) noexcept;

    struct DeviceLayoutCaps
    {
        VkPhysicalDeviceLimits limits{};
        std::uint32_t api_version{}, vendor_id{}, device_id{}, driver_version{};
        // Descriptor indexing is not enabled by the Device factory. Support is a separate diagnostic fact.
        VkPhysicalDeviceDescriptorIndexingFeatures supported_indexing{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES
        };
    };

    [[nodiscard]] DeviceLayoutCaps queryLayoutCaps(const VulkanDevice& device) noexcept;

    struct OwnerAssignment
    {
        ShaderOwnerId scene, feature, pass_local;
    };

    struct DescriptorLocation
    {
        std::uint32_t field_index, set, binding;
        ShaderOwnerId owner;
        VkDescriptorType type;
        std::uint32_t count, stages;
        VkDescriptorBindingFlags flags;
        std::uint32_t dynamic_index;
        bool operator==(const DescriptorLocation&) const noexcept = default;
    };

    struct LayoutBinding
    {
        ShaderOwnerId owner;
        std::uint32_t owner_field, binding;
        bool operator==(const LayoutBinding&) const noexcept = default;
    };

    struct LayoutSet
    {
        std::vector<LayoutBinding> bindings;
        bool operator==(const LayoutSet&) const noexcept = default;
    };

    struct PushRange
    {
        std::uint32_t offset, size, stages;
        bool operator==(const PushRange&) const noexcept = default;
    };

    struct LayoutIdentity
    {
        std::vector<OwnerShape> owners;
        std::vector<LayoutSet> sets;
        std::vector<DescriptorLocation> fields;
        std::vector<PushRange> push_ranges;
        std::uint32_t dynamic_count{};
        bool operator==(const LayoutIdentity&) const noexcept = default;
    };

    // Immutable, fully validated cold result. No device or runtime registry ownership.
    class LayoutPlan
    {
    public:
        [[nodiscard]] const LayoutIdentity& identity() const noexcept
        {
            return identity_;
        }

        [[nodiscard]] bool matches(const LayoutPlan& other) const noexcept
        {
            return identity_ == other.identity_;
        }

        [[nodiscard]] std::string diagnostics() const noexcept;

    private:
        friend RenderResult<LayoutPlan>
        compileLayout(const rdesc::PassShaderContract&, const OwnerAssignment&, std::span<const OwnerShape>, const DeviceLayoutCaps&) noexcept;

        explicit LayoutPlan(LayoutIdentity identity) noexcept : identity_(std::move(identity)) {}

        LayoutIdentity identity_;
    };

    [[nodiscard]] RenderResult<LayoutPlan> compileLayout(
        const rdesc::PassShaderContract& schema,
        const OwnerAssignment& assignment,
        std::span<const OwnerShape> complete_owners,
        const DeviceLayoutCaps& caps
    ) noexcept;

    [[nodiscard]] VkShaderStageFlags nativeStages(std::uint32_t schema_stages) noexcept;
} // namespace lux::render::vulkan
