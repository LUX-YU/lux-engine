#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/pipeline/EngineSetShapes.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>

#include <algorithm>
#include <span>
#include <utility>
#include <vector>

namespace lux::render
{
    namespace
    {
        void expandEngineSet(
            const EngineSetShape& shape,
            uint32_t binding_offset,
            uint32_t bindless_2d_count,
            uint32_t bindless_cube_count,
            std::vector<VkDescriptorSetLayoutBinding>& bindings,
            std::vector<VkDescriptorBindingFlags>& flags
        )
        {
            const auto resolveCount = [&](const EngineSetBindingShape& binding) -> uint32_t
            {
                switch (binding.count_source)
                {
                case EBindingCountSource::BINDLESS_2D_TEXTURES:
                    return bindless_2d_count;
                case EBindingCountSource::BINDLESS_CUBE_TEXTURES:
                    return bindless_cube_count;
                case EBindingCountSource::VERTEX_POOL_SLOTS:
                    return kVertexPoolMaxCount;
                case EBindingCountSource::MATERIAL_FAMILIES:
                    return 1u;
                case EBindingCountSource::FIXED:
                default:
                    return binding.count;
                }
            };

            for (const auto& binding : shape.bindings)
            {
                const bool is_family_template =
                    shape.expand_by_count_source && binding.count_source == EBindingCountSource::MATERIAL_FAMILIES;
                const uint32_t repeat = is_family_template ? kMaterialFamilyBindingCount : 1u;
                for (uint32_t i = 0; i < repeat; ++i)
                {
                    VkDescriptorSetLayoutBinding value{};
                    value.binding = binding_offset + binding.binding + i;
                    value.descriptorType = binding.type;
                    value.descriptorCount = resolveCount(binding);
                    value.stageFlags = binding.stages;
                    bindings.push_back(value);
                    flags.push_back(binding.binding_flags);
                }
            }
        }

        Expected<DescriptorSetLayoutOwner> createLayout(
            VkDevice device,
            std::span<const VkDescriptorSetLayoutBinding> bindings,
            std::span<const VkDescriptorBindingFlags> flags,
            bool update_after_bind
        ) noexcept
        {
            const bool has_binding_flags =
                std::any_of(flags.begin(), flags.end(), [](VkDescriptorBindingFlags flag) { return flag != 0; });
            VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO
            };
            binding_flags.bindingCount = static_cast<uint32_t>(flags.size());
            binding_flags.pBindingFlags = flags.data();

            VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            info.bindingCount = static_cast<uint32_t>(bindings.size());
            info.pBindings = bindings.data();
            info.flags = update_after_bind ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
            info.pNext = has_binding_flags ? &binding_flags : nullptr;
            auto layout = DescriptorSetLayoutOwner::create(device, info);
            if (!layout)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(layout.error()));
            }
            return std::move(*layout);
        }
    } // namespace

    GeneralDescriptorSetLayout::GeneralDescriptorSetLayout(LayoutStorage&& storage) noexcept
        : storage_(std::move(storage))
    {
    }

    GeneralDescriptorSetLayout::CreateResult GeneralDescriptorSetLayout::create(DeviceContext& device_context) noexcept
    {
        LayoutStorage candidate;
        const VkDevice device = device_context.logicalDevice();
        const auto& index_properties = device_context.physicalDevice().descriptorIndexingProperties();
        const uint32_t raw_budget = std::min(
            index_properties.maxDescriptorSetUpdateAfterBindSampledImages,
            index_properties.maxPerStageDescriptorUpdateAfterBindSampledImages
        );
        constexpr uint32_t kNonTextureReserve = 8;
        const uint32_t budget = (raw_budget > kNonTextureReserve) ? (raw_budget - kNonTextureReserve) : raw_budget;
        candidate.bindless_cube_count = std::min(kBindlessCubeCeiling, budget);
        const uint32_t uncapped_2d =
            (budget > candidate.bindless_cube_count) ? (budget - candidate.bindless_cube_count) : 1u;
        candidate.bindless_2d_count = std::min(uncapped_2d, kBindlessTex2DCeiling);

        // One device-derived capacity governs both canonical and merged layouts, and is
        // retained for pool sizing. Applying the ceiling only at the pool caller used to
        // under-budget the actual layout on drivers that account descriptors strictly.
        for (const auto& shape : kEngineSetShapes)
        {
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            std::vector<VkDescriptorBindingFlags> flags;
            expandEngineSet(shape, 0u, candidate.bindless_2d_count, candidate.bindless_cube_count, bindings, flags);
            auto layout = createLayout(device, bindings, flags, shape.update_after_bind);
            if (!layout)
            {
                return lux::cxx::unexpected(layout.error());
            }
            candidate.layouts[static_cast<uint32_t>(shape.slot)] = std::move(*layout);
        }

        constexpr rdesc::EBindFrequency kMergeable[] =
            {rdesc::EBindFrequency::GLOBAL, rdesc::EBindFrequency::BINDLESS, rdesc::EBindFrequency::FEATURE};
        for (const auto domain : kMergeable)
        {
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            std::vector<VkDescriptorBindingFlags> flags;
            bool update_after_bind = false;
            // Domain offsets come from the engine contract, never from the subset seen
            // by a particular graph. PASS_LOCAL remains a per-pipeline layout.
            for (uint32_t slot = 0; slot < kEngineSetShapes.size(); ++slot)
            {
                const auto& shape = kEngineSetShapes[slot];
                if (shape.frequency != domain)
                {
                    continue;
                }
                expandEngineSet(
                    shape,
                    engineSetDomainOffset(slot),
                    candidate.bindless_2d_count,
                    candidate.bindless_cube_count,
                    bindings,
                    flags
                );
                update_after_bind = update_after_bind || shape.update_after_bind;
            }
            if (bindings.empty())
            {
                continue;
            }
            if (bindings.size() != domainBindingCount(domain))
            {
                return renderFailure<err::internal::Unspecified>();
            }
            auto layout = createLayout(device, bindings, flags, update_after_bind);
            if (!layout)
            {
                return lux::cxx::unexpected(layout.error());
            }
            candidate.domain_layouts[static_cast<std::size_t>(domain)] = std::move(*layout);
        }
        return std::unique_ptr<GeneralDescriptorSetLayout>(new GeneralDescriptorSetLayout(std::move(candidate)));
    }
} // namespace lux::render
