#include <lux/engine/render/gpu/pipeline/PipelineLayoutService.hpp>

#include <cstdio>

namespace lux::render
{

    namespace
    {
        bool isEqual(const VkPushConstantRange& a, const VkPushConstantRange& b) noexcept
        {
            return a.stageFlags == b.stageFlags && a.offset == b.offset && a.size == b.size;
        }

        template <class T> bool isEqual(const T& a, const T& b) noexcept
        {
            return a == b;
        }

        template <class T> bool vectorEquals(std::span<const T> a, const std::vector<T>& b) noexcept
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (!isEqual(a[i], b[i]))
                    return false;
            return true;
        }
    } // namespace

    PipelineLayoutService::PipelineLayoutService(VkDevice device, uint32_t max_bound_descriptor_sets)
        : device_(device), max_bound_descriptor_sets_(max_bound_descriptor_sets)
    {
    }

    PipelineLayoutService::~PipelineLayoutService()
    {
        for (auto& entry : entries_)
        {
            vkDestroyPipelineLayout(device_, entry.layout, nullptr);
        }
    }

    Expected<VkPipelineLayout> PipelineLayoutService::getOrCreate(const PipelineLayoutDesc& desc)
    {
        for (const auto& entry : entries_)
        {
            if (vectorEquals(desc.set_layouts, entry.set_layouts) &&
                vectorEquals(desc.push_constants, entry.push_constants))
                return entry.layout;
        }

        // Fail fast with a readable error instead of tripping
        // VUID-VkPipelineLayoutCreateInfo-setLayoutCount-00286 in the driver.
        // Mali devices commonly report maxBoundDescriptorSets = 4.
        if (max_bound_descriptor_sets_ != 0 && desc.set_layouts.size() > max_bound_descriptor_sets_)
        {
            return renderFailure<err::device::VulkanObjectCreationFailed>();
        }

        Entry new_entry{};
        new_entry.set_layouts.assign(desc.set_layouts.begin(), desc.set_layouts.end());
        new_entry.push_constants.assign(desc.push_constants.begin(), desc.push_constants.end());
        new_entry.debug_name = desc.debug_name;

        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = static_cast<uint32_t>(new_entry.set_layouts.size());
        ci.pSetLayouts = new_entry.set_layouts.data();
        ci.pushConstantRangeCount = static_cast<uint32_t>(new_entry.push_constants.size());
        ci.pPushConstantRanges = new_entry.push_constants.data();

        if (vkCreatePipelineLayout(device_, &ci, nullptr, &new_entry.layout) != VK_SUCCESS)
            return renderFailure<err::device::VulkanObjectCreationFailed>();

        entries_.push_back(std::move(new_entry));
        return entries_.back().layout;
    }

} // namespace lux::render
