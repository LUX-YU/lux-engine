#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/gpu/pipeline/EngineSetShapes.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace lux::render
{
    GeneralDescriptorSetLayout::~GeneralDescriptorSetLayout()
    {
        auto& device = device_context_.logicalDevice();
        for (auto& layout : layouts_)
        {
            if (layout != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorSetLayout(device, layout, nullptr);
                layout = VK_NULL_HANDLE;
            }
        }
        for (auto& layout : domain_layouts_)
        {
            if (layout != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorSetLayout(device, layout, nullptr);
                layout = VK_NULL_HANDLE;
            }
        }
    }

    GeneralDescriptorSetLayout& GeneralDescriptorSetLayout::operator=(GeneralDescriptorSetLayout&& other) noexcept
    {
        if (this != &other)
        {
            // Destroy our own layouts first
            auto& device = device_context_.logicalDevice();
            for (auto& layout : layouts_)
            {
                if (layout != VK_NULL_HANDLE)
                {
                    vkDestroyDescriptorSetLayout(device, layout, nullptr);
                    layout = VK_NULL_HANDLE;
                }
            }
            for (auto& layout : domain_layouts_)
            {
                if (layout != VK_NULL_HANDLE)
                {
                    vkDestroyDescriptorSetLayout(device, layout, nullptr);
                    layout = VK_NULL_HANDLE;
                }
            }
            // Take ownership from other
            layouts_ = other.layouts_;
            other.layouts_.fill(VK_NULL_HANDLE);
            domain_layouts_ = other.domain_layouts_;
            other.domain_layouts_.fill(VK_NULL_HANDLE);
            bindless_2d_count_   = other.bindless_2d_count_;
            bindless_cube_count_ = other.bindless_cube_count_;
        }
        return *this;
    }

    bool GeneralDescriptorSetLayout::init()
    {
        auto& device = device_context_.logicalDevice();

        // Shape is data, not code — the per-set binding definitions live in
        // EngineSetShapes.hpp (kEngineSetShapes), and all that's left here is
        // a single generic build loop. An engine set's shape must be
        // declared by the engine itself: the reflection union at
        // graph-compile time only sees the subset of bindings this graph
        // actually uses, while the set instance is always allocated with the
        // full shape (binding a full set with a subset layout is
        // VUID-00358); and binding 0 of the Scene set isn't declared by any
        // shader at all.

        // Bindless capacity is derived from the device limits (aligned with
        // the UPDATE_AFTER_BIND pool limit), reserving headroom for the CIS
        // bindings of other sets in the same pipeline layout (e.g. the Light
        // set's SHADOW_ATLAS).
        const auto& idx_props = device_context_.physicalDevice().descriptorIndexingProperties();
        const uint32_t raw_budget = std::min(
            idx_props.maxDescriptorSetUpdateAfterBindSampledImages,
            idx_props.maxPerStageDescriptorUpdateAfterBindSampledImages);
        constexpr uint32_t kNonTextureReserve = 8;
        const uint32_t budget = (raw_budget > kNonTextureReserve)
                                  ? (raw_budget - kNonTextureReserve) : raw_budget;
        const uint32_t kCubeMaxCount = std::min(kBindlessCubeCeiling, budget);
        const uint32_t uncapped_2d   = (budget > kCubeMaxCount) ? (budget - kCubeMaxCount) : 1u;
        // The ceiling is part of THIS derivation, not a caller's afterthought.
        // It used to live only in RenderServer, which meant the layout declared
        // the full device budget while the descriptor pool was sized for the
        // capped one. On a driver that accounts pool capacity strictly, that is
        // vkAllocateDescriptorSets -> VK_ERROR_OUT_OF_POOL_MEMORY: Adreno 830
        // reports ~16.7M UAB sampled images, so the layout asked for 16,776,952
        // descriptors out of a pool holding 65,792. Desktop NVIDIA hid it by
        // not accounting per-type strictly.
        const uint32_t k2DMaxCount   = std::min(uncapped_2d, kBindlessTex2DCeiling);

        // The device-derived bindless capacity is kept as a member — the
        // domain-merged layout must use these exact same values, AND every
        // other consumer (descriptor pool sizing above all) must read them
        // from here rather than recompute. If both paths recomputed it
        // independently, they would sooner or later drift apart on some
        // detail, and the symptom of that drift is a misaligned descriptor
        // with no Vulkan error at all.
        //
        // That is not hypothetical any more: RenderServer did recompute it,
        // the two derivations differed by exactly this ceiling, and it took a
        // phone to notice.
        bindless_2d_count_   = k2DMaxCount;
        bindless_cube_count_ = kCubeMaxCount;

        for (const auto& shape : kEngineSetShapes)
        {
            const uint32_t set_index = static_cast<uint32_t>(shape.slot);

            std::vector<VkDescriptorSetLayoutBinding> bindings;
            std::vector<VkDescriptorBindingFlags>     flags;
            expandEngineSet(shape, 0u, bindings, flags);

            const bool any_flags = std::any_of(flags.begin(), flags.end(),
                                               [](VkDescriptorBindingFlags f) { return f != 0; });

            VkDescriptorSetLayoutBindingFlagsCreateInfo bf{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
            bf.bindingCount  = static_cast<uint32_t>(flags.size());
            bf.pBindingFlags = flags.data();

            VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            ci.bindingCount = static_cast<uint32_t>(bindings.size());
            ci.pBindings    = bindings.data();
            if (shape.update_after_bind)
                ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            if (any_flags)
                ci.pNext = &bf;

            if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &layouts_[set_index]) != VK_SUCCESS)
                return false;
        }

        return initDomainLayouts(device);
    }

    bool GeneralDescriptorSetLayout::initDomainLayouts(VkDevice device)
    {
        // The three mergeable domains. PASS_LOCAL is the domain for
        // single-pipeline private sets, and it is never merged.
        constexpr rdesc::EBindFrequency kMergeable[] = {
            rdesc::EBindFrequency::GLOBAL,
            rdesc::EBindFrequency::BINDLESS,
            rdesc::EBindFrequency::FEATURE,
        };

        for (const auto domain : kMergeable)
        {
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            std::vector<VkDescriptorBindingFlags>     flags;
            bool update_after_bind = false;

            // Expand in ascending canonical-set-number order, shifting each
            // binding number to its own intra-domain offset. The offset is
            // taken from an engine-level constant (not accumulated from
            // whichever sets this particular call happens to see) — a domain
            // set is one scene-level instance, so it must be the same layout
            // for every graph.
            for (uint32_t s = 0; s < kEngineSetShapes.size(); ++s)
            {
                const auto& shape = kEngineSetShapes[s];
                if (shape.frequency != domain)
                    continue;
                expandEngineSet(shape, engineSetDomainOffset(s), bindings, flags);
                update_after_bind = update_after_bind || shape.update_after_bind;
            }

            if (bindings.empty())
                continue;

            // Self-check: the number of expanded bindings must equal the
            // domain capacity computed from the constant. A mismatch means
            // the expansion and the offset-allocation algorithm have drifted
            // apart — that would misalign every intra-domain offset, with no
            // Vulkan error at all (each binding is individually legal, it's
            // just sitting in the wrong place).
            if (bindings.size() != domainBindingCount(domain))
                return false;

            const bool any_flags = std::any_of(flags.begin(), flags.end(),
                                               [](VkDescriptorBindingFlags f) { return f != 0; });

            VkDescriptorSetLayoutBindingFlagsCreateInfo bf{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
            bf.bindingCount  = static_cast<uint32_t>(flags.size());
            bf.pBindingFlags = flags.data();

            VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            ci.bindingCount = static_cast<uint32_t>(bindings.size());
            ci.pBindings    = bindings.data();
            if (update_after_bind)
                ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            if (any_flags)
                ci.pNext = &bf;

            auto& out = domain_layouts_[static_cast<std::size_t>(domain)];
            if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &out) != VK_SUCCESS)
                return false;
        }

        return true;
    }

    void GeneralDescriptorSetLayout::expandEngineSet(
        const EngineSetShape& shape,
        uint32_t binding_offset,
        std::vector<VkDescriptorSetLayoutBinding>& bindings,
        std::vector<VkDescriptorBindingFlags>&     flags) const
    {
        const auto resolveCount = [this](const EngineSetBindingShape& b) -> uint32_t
        {
            switch (b.count_source)
            {
            case EBindingCountSource::Bindless2DTextures:   return bindless_2d_count_;
            case EBindingCountSource::BindlessCubeTextures: return bindless_cube_count_;
            case EBindingCountSource::VertexPoolSlots:      return kVertexPoolMaxCount;
            case EBindingCountSource::MaterialFamilies:     return 1u;   // expanded into N bindings, each with count 1
            case EBindingCountSource::Fixed:
            default:                                        return b.count;
            }
        };

        for (const auto& b : shape.bindings)
        {
            // expand_by_count_source: a template entry is expanded into N
            // identically-shaped bindings driven by the constant (Material's
            // per-family SSBO).
            const uint32_t repeat = (shape.expand_by_count_source &&
                                     b.count_source == EBindingCountSource::MaterialFamilies)
                                  ? kMaterialFamilyBindingCount : 1u;
            for (uint32_t i = 0; i < repeat; ++i)
            {
                VkDescriptorSetLayoutBinding vb{};
                vb.binding         = binding_offset + b.binding + i;
                vb.descriptorType  = b.type;
                vb.descriptorCount = resolveCount(b);
                vb.stageFlags      = b.stages;
                bindings.push_back(vb);
                flags.push_back(b.binding_flags);
            }
        }
    }

    // (getAllLayouts 已删:零调用点,且每次调用都要堆分配一个 vector 拷贝。
    //  现役取法是按槽位 getLayout(EDescriptorSetSlot)。)
}
