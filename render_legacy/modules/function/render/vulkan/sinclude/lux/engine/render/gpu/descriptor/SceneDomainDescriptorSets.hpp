#pragma once
// Complete per-scene GLOBAL/FEATURE set handles. The scene arena owns their pools;
// resources borrow a per-frame span and write their own domain binding range.
// BINDLESS remains global and PASS_LOCAL remains owned by each pass.

#include <lux/engine/description/LayoutContract.hpp>
#include <lux/engine/function/render/client/core/Errors.hpp>

#include <lux/engine/function/visibility.h>

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace lux::render
{
    class GeneralDescriptorSetLayout;
    class SceneDescriptorArena;

    class LUX_FUNCTION_PUBLIC SceneDomainDescriptorSets final
    {
    public:
        /// Domains allocated per scene. BINDLESS is not among them (see file header).
        static constexpr std::array<rdesc::EBindFrequency, 2> kPerSceneDomains{
            rdesc::EBindFrequency::GLOBAL,
            rdesc::EBindFrequency::FEATURE,
        };

        using CreateResult = Expected<std::unique_ptr<SceneDomainDescriptorSets>>;

        /// Allocations remain owned by the arena until its generation/scene safe point.
        [[nodiscard]] static CreateResult
        create(SceneDescriptorArena& arena, const GeneralDescriptorSetLayout& layouts, uint32_t slices) noexcept;

        SceneDomainDescriptorSets(const SceneDomainDescriptorSets&) = delete;
        SceneDomainDescriptorSets& operator=(const SceneDomainDescriptorSets&) = delete;
        SceneDomainDescriptorSets(SceneDomainDescriptorSets&&) = delete;
        SceneDomainDescriptorSets& operator=(SceneDomainDescriptorSets&&) = delete;

        /// Gets the set for a given domain and slice. Returns VK_NULL_HANDLE if not allocated.
        [[nodiscard]] VkDescriptorSet set(rdesc::EBindFrequency domain, uint32_t slice) const noexcept
        {
            const auto d = static_cast<std::size_t>(domain);
            if (d >= sets_.size() || slice >= sets_[d].size())
                return VK_NULL_HANDLE;
            return sets_[d][slice];
        }

        /// Borrowed per-frame set handles; the backing arena must remain alive.
        [[nodiscard]] std::span<const VkDescriptorSet> setsFor(rdesc::EBindFrequency domain) const noexcept
        {
            const auto d = static_cast<std::size_t>(domain);
            if (d >= sets_.size())
                return {};
            return {sets_[d].data(), sets_[d].size()};
        }

    private:
        using Sets = std::array<std::vector<VkDescriptorSet>, 4>;
        explicit SceneDomainDescriptorSets(Sets&& sets) noexcept;

        /// Borrowed allocations; PASS_LOCAL and BINDLESS remain empty.
        Sets sets_;
    };

} // namespace lux::render
