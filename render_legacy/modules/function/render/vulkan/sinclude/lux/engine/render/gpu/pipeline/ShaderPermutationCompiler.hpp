#pragma once
// =============================================================================
//  ShaderPermutationCompiler.hpp — specialization-constant utilities for
//  shader variants.
// -----------------------------------------------------------------------------
//  Variant mechanism: a single VkShaderModule serves every feature
//  combination — specialization constants are injected at *pipeline
//  creation time*, not at module creation time. So all that's needed here
//  is one pure function: "feature mask -> VkSpecializationInfo."
//
//  This class used to do more than that; that part has been removed. It
//  originally had its own "build a VkShaderModule" path
//  (registerBaseShader / getOrCompile / compileModule — the second
//  vkCreateShaderModule call site in the whole repo), with zero callers.
//  Its existence violated single-ingestion: finalizing descriptor
//  positions (the domain-merge patch, see
//  ShaderResources::mergedOrOriginal) is a property of the one chokepoint
//  where "SPIR-V becomes a module," and that chokepoint must have exactly
//  one owner — ShaderResources. One extra bypass means one more class of
//  silent bug: an unpatched module sneaking into a pipeline. After removal,
//  vkCreateShaderModule exists in exactly one place in the whole repo,
//  ShaderResources.cpp; if a second module-creation site is ever needed,
//  answer "how does it apply position finalization" first, before writing
//  it.
//
//  Convention (GLSL side):
//    layout(constant_id = 0) const bool HAS_NORMAL_MAP = false;
//    layout(constant_id = 1) const bool HAS_SKINNING   = false;
//    ...  — constant_id i corresponds to bit i of EShaderFeature.
// =============================================================================

#include <lux/engine/render/gpu/pipeline/ShaderPermutation.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace lux::render
{
    class ShaderPermutationCompiler
    {
    public:
        /**
         * @brief Build VkSpecializationInfo for a feature mask.
         *
         * The caller must keep the returned SpecializationData alive until
         * pipeline creation is complete (it holds the backing storage).
         */
        struct SpecializationData
        {
            std::vector<VkSpecializationMapEntry> entries;
            std::vector<VkBool32> values;
            VkSpecializationInfo info{};

            /// @brief Update the info pointers (call after moves / copies of entries/values)
            void finalize()
            {
                info.mapEntryCount = static_cast<uint32_t>(entries.size());
                info.pMapEntries = entries.data();
                info.dataSize = values.size() * sizeof(VkBool32);
                info.pData = values.data();
            }
        };

        static SpecializationData buildSpecializationData(ShaderFeatureMask features, uint32_t max_features = 32)
        {
            SpecializationData sd;
            sd.entries.reserve(max_features);
            sd.values.reserve(max_features);

            for (uint32_t i = 0; i < max_features; ++i)
            {
                VkSpecializationMapEntry entry{};
                entry.constantID = i;
                entry.offset = static_cast<uint32_t>(i * sizeof(VkBool32));
                entry.size = sizeof(VkBool32);
                sd.entries.push_back(entry);

                sd.values.push_back((features & (1u << i)) ? VK_TRUE : VK_FALSE);
            }
            sd.finalize();
            return sd;
        }
    };

} // namespace lux::render
