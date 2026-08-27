#pragma once
// =============================================================================
//  SceneDomainDescriptorSets.hpp — merged descriptor-set instances, grouped
//  by bind-frequency domain.
// -----------------------------------------------------------------------------
//  This is where the ownership-model change lands.
//
//  Old model: one set = one resource object's private property. SceneResources
//  allocated set0 itself, InstanceResources allocated set1 itself,
//  LightResources allocated set3 itself, and so on — each one allocated,
//  wrote, and destroyed its own set, one-to-one.
//
//  Merged model: one set now spans multiple owners. The FEATURE domain's set
//  holds the bindings of six different owners at once — Instance, Light,
//  Material, Particle, Compute, and VertexPool. So "who allocates" has to
//  converge on a single place: this class. Each owner is reduced to "write
//  into my slice of the binding range," where the range comes from the
//  per-domain offset handed down by LayoutPlan (engineSetDomainOffset).
//
//  Why keyed per scene: the domain set's members include per-scene resources
//  (Scene/Instance/Light/VertexPool/...), so the whole set can only take on
//  the shortest lifetime among them, i.e. one instance per scene. Global
//  owners (texture table, material table) then write into each active
//  scene's copy in turn.
//
//  Why BINDLESS isn't here: after merging, BINDLESS has only the global
//  texture table left in it, so it keeps using the existing global set as-is.
//  Forcing it down to per-scene would mean every scene's pool has to hold
//  thousands of bindless texture descriptors x frames-in-flight — pure
//  waste. See the comment on the vertex pool's domain assignment in
//  EngineSetShapes.
//
//  Design reference: .internal/lux-engine-descriptor-layout-current-state.md §8.0d
// =============================================================================

#include <lux/engine/render/gpu/pipeline/EngineSetShapes.hpp>
#include <lux/engine/render/gpu/descriptor/SceneDescriptorArena.hpp>
#include <lux/engine/function/visibility.h>

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <vector>

namespace lux::render
{
    class GeneralDescriptorSetLayout;

    class LUX_FUNCTION_PUBLIC SceneDomainDescriptorSets
    {
    public:
        /// Domains allocated per scene. BINDLESS is not among them (see file header).
        static constexpr std::array<rdesc::EBindFrequency, 2> kPerSceneDomains{
            rdesc::EBindFrequency::GLOBAL,
            rdesc::EBindFrequency::FEATURE,
        };

        /// Allocates one set per domain per frames-in-flight slice.
        /// Returns false on failure (pool exhausted / layout missing); the
        /// caller decides whether that's fatal.
        bool init(SceneDescriptorArena& arena, const GeneralDescriptorSetLayout& layouts, uint32_t slices);

        /// Gets the set for a given domain and slice. Returns VK_NULL_HANDLE if not allocated.
        [[nodiscard]] VkDescriptorSet set(rdesc::EBindFrequency domain, uint32_t slice) const noexcept
        {
            const auto d = static_cast<std::size_t>(domain);
            if (d >= sets_.size() || slice >= sets_[d].size())
                return VK_NULL_HANDLE;
            return sets_[d][slice];
        }

        /// Per-slice set handles for a given domain.
        ///
        /// Used by resource objects to dual-write: they only need the plain
        /// data of "which sets to write into," not knowledge of this class.
        /// Passing a span instead of `this` avoids pulling this header (and
        /// the whole EngineSetShapes -> LayoutContract chain it drags in)
        /// into the resource objects' headers.
        [[nodiscard]] std::span<const VkDescriptorSet> setsFor(rdesc::EBindFrequency domain) const noexcept
        {
            const auto d = static_cast<std::size_t>(domain);
            if (d >= sets_.size())
                return {};
            return {sets_[d].data(), sets_[d].size()};
        }

        [[nodiscard]] uint32_t slices() const noexcept
        {
            return slices_;
        }

        /// This class only holds set handles, not the pool itself — the pool
        /// belongs to SceneDescriptorArena and is reclaimed together when its
        /// whole generation retires (beginGeneration/releaseRetired), so
        /// there's no teardown logic here; clear() only needs to run on rebuild.
        void clear() noexcept
        {
            for (auto& v : sets_)
                v.clear();
            slices_ = 0;
        }

    private:
        /// [domain][slice]. The PASS_LOCAL and BINDLESS slots are always empty.
        std::array<std::vector<VkDescriptorSet>, 4> sets_{};
        uint32_t slices_{0};
    };

} // namespace lux::render
