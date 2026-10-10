#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/gpu/utils/vk_convert.hpp> // toVk(SamplerDesc)

#include <limits>

namespace lux::render
{

    namespace
    {
        bool equalBindings(
            const std::vector<VkDescriptorSetLayoutBinding>& a,
            std::span<const VkDescriptorSetLayoutBinding> b
        ) noexcept
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (a[i].binding != b[i].binding)
                {
                    return false;
                }
                if (a[i].descriptorType != b[i].descriptorType)
                {
                    return false;
                }
                if (a[i].descriptorCount != b[i].descriptorCount)
                {
                    return false;
                }
                if (a[i].stageFlags != b[i].stageFlags)
                {
                    return false;
                }
            }
            return true;
        }

        bool equalBindingFlags(
            const std::vector<VkDescriptorBindingFlags>& stored,
            std::span<const VkDescriptorBindingFlags> incoming
        ) noexcept
        {
            // Both empty → equal (no per-binding flags).
            if (stored.empty() && incoming.empty())
            {
                return true;
            }
            if (stored.size() != incoming.size())
            {
                return false;
            }
            for (size_t i = 0; i < stored.size(); ++i)
            {
                if (stored[i] != incoming[i])
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    DescriptorService::DescriptorService(VkDevice device) noexcept : device_(device) {}

    // Shared sampler cache.
    //
    // Callers hand in a SamplerDesc and get a sampler owned by this service for the
    // device's lifetime, replacing the hand-written VkSamplerCreateInfo +
    // vkCreateSampler + deferred-retire trio each site used to carry.
    //
    // Every feature-layer and resource-layer site now goes through here. Three that
    // deliberately do not:
    //   · RenderServer's REPEAT sampler — a default create-info template for the
    //     unbound-texture table, not a sampler anyone samples with;
    //   · BindlessCombinedSet — per-texture custom samplers (opt_sampler_ci), an
    //     unbounded set of distinct descs, so caching buys nothing;
    //   · GpuTransferPipeline — upload resources carry their own sampler state.
    //
    // The first sweep also excluded EVSMShadowResources "because it sets a border
    // colour". That reason did not hold: all three of its address modes are
    // clamp-to-edge, so the border is never sampled and the shape is plain
    // linearClamp(). ShadowResources and LightResources were in neither list —
    // the sweep simply missed them. ShadowResources was the only genuine blocker,
    // and it was a gap in SamplerDesc rather than in the site: compare_op and
    // border_color did not exist, so a comparison sampler was inexpressible here.
    Expected<VkSampler> DescriptorService::sampler(const SamplerDesc& desc)
    {
        for (const auto& [d, s] : samplers_)
        {
            if (d == desc)
            {
                return s.get();
            }
        }

        const VkSamplerCreateInfo si = vk_convert::toVk(desc);

        auto candidate = SamplerOwner::create(device_, si);
        if (!candidate)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(candidate.error()));
        }
        const auto sampler = candidate->get();
        samplers_.emplace_back(desc, std::move(*candidate));
        return sampler;
    }

    Expected<DescriptorLayoutId> DescriptorService::registerLayout(const DescriptorLayoutDesc& desc) noexcept
    {
        const bool is_invalid_device = device_ == VK_NULL_HANDLE;
        const bool has_invalid_flags = !desc.binding_flags.empty() && desc.binding_flags.size() != desc.bindings.size();
        const bool exceeds_binding_count = desc.bindings.size() > std::numeric_limits<uint32_t>::max();
        const bool is_invalid_descriptor = is_invalid_device || has_invalid_flags || exceeds_binding_count;
        if (is_invalid_descriptor)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }
        for (size_t i = 0; i < layouts_.size(); ++i)
        {
            const auto& e = layouts_[i];
            const bool has_same_shape = e.flags == desc.flags && equalBindings(e.bindings, desc.bindings) &&
                                        equalBindingFlags(e.binding_flags, desc.binding_flags);
            if (has_same_shape)
            {
                return static_cast<uint32_t>(i);
            }
        }

        LayoutEntry entry{};
        entry.flags = desc.flags;
        entry.debug_name = desc.debug_name;
        entry.bindings.assign(desc.bindings.begin(), desc.bindings.end());
        entry.binding_flags.assign(desc.binding_flags.begin(), desc.binding_flags.end());

        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.flags = entry.flags;
        ci.bindingCount = static_cast<uint32_t>(entry.bindings.size());
        ci.pBindings = entry.bindings.data();

        // Attach per-binding flags (e.g. UPDATE_AFTER_BIND) when provided.
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO
        };
        if (!entry.binding_flags.empty())
        {
            bf.bindingCount = static_cast<uint32_t>(entry.binding_flags.size());
            bf.pBindingFlags = entry.binding_flags.data();
            ci.pNext = &bf;
        }

        auto candidate = DescriptorSetLayoutOwner::create(device_, ci);
        if (!candidate)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(candidate.error()));
        }
        entry.layout = std::move(*candidate);

        layouts_.push_back(std::move(entry));
        return static_cast<uint32_t>(layouts_.size() - 1);
    }

    VkDescriptorSetLayout DescriptorService::layout(DescriptorLayoutId id) const noexcept
    {
        if (id == kInvalidDescriptorLayoutId)
        {
            return VK_NULL_HANDLE;
        }
        const auto idx = static_cast<size_t>(id);
        if (idx >= layouts_.size())
        {
            return VK_NULL_HANDLE;
        }
        return layouts_[idx].layout.get();
    }

} // namespace lux::render
