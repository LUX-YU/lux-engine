#include <lux/engine/render/gpu/descriptor/SceneDomainDescriptorSets.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>

namespace lux::render
{

    bool SceneDomainDescriptorSets::init(
        SceneDescriptorArena& arena,
        const GeneralDescriptorSetLayout& layouts,
        uint32_t slices
    )
    {
        clear();
        slices_ = std::max(1u, slices);

        for (const auto domain : kPerSceneDomains)
        {
            const VkDescriptorSetLayout layout = layouts.getDomainLayout(domain);
            if (layout == VK_NULL_HANDLE)
                return false;

            auto& v = sets_[static_cast<std::size_t>(domain)];
            v.resize(slices_, VK_NULL_HANDLE);
            for (uint32_t i = 0; i < slices_; ++i)
            {
                v[i] = arena.allocate(layout, 0u);
                if (v[i] == VK_NULL_HANDLE)
                {
                    clear();
                    return false;
                }
            }
        }

        return true;
    }

} // namespace lux::render
