#pragma once
/**
 * @file ShaderResources.hpp
 * @brief Manages compiled shader modules (VkShaderModule + reflection info).
 *
 * Registered in the GlobalResourceRegistry so that RenderFeatures can
 * resolve ShaderHandle to ShaderObject during init().
 *
 * Usage:
 *   auto* sr = registry.find<ShaderResources>();
 *   ShaderHandle h = sr->add(spirv, info);   // compiles SPIR-V → VkShaderModule
 *   const ShaderObject* obj = sr->get(h);
 *   sr->remove(h);
 */

#include <lux/engine/render/resources/lifecycle/GPUResourceBase.hpp>
#include <lux/engine/render/core/ShaderObject.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/function/visibility.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace lux::rdesc { class Shader; class ShaderInfo; }

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC ShaderResources final
        : public GPUResourceBase<ShaderResources, EGPUResourceType::Shader>
    {
    public:
        struct InitInfo
        {
            VkDevice device{VK_NULL_HANDLE};
        };

        ShaderResources() = default;
        ~ShaderResources();

        ShaderResources(const ShaderResources&)            = delete;
        ShaderResources& operator=(const ShaderResources&) = delete;
        ShaderResources(ShaderResources&&)                 = default;
        ShaderResources& operator=(ShaderResources&&)      = default;

        void init(const InitInfo& info);
        void shutdown();

        /// Compile SPIR-V into a VkShaderModule and register it.
        /// Returns a valid ShaderHandle on success, invalid on failure.
        [[nodiscard]] ShaderHandle add(
            const lux::rdesc::Shader& spirv, 
            const lux::rdesc::ShaderInfo& info
        );

        /// Compile SPIR-V from a byte span (avoids heap allocation of Shader).
        [[nodiscard]] ShaderHandle add(
            std::span<const std::byte> spirv_bytes,
            const lux::rdesc::ShaderInfo& info
        );

        /// Register a pre-built ShaderObject. Returns a valid ShaderHandle.
        [[nodiscard]] ShaderHandle add(ShaderObject obj);

        /// Lookup a shader by handle. Returns nullptr for invalid/freed/stale handles.
        [[nodiscard]] const ShaderObject* get(ShaderHandle handle) const noexcept;

        /// Release one owner of the shader. The VkShaderModule is destroyed (and
        /// the slot freed + generation bumped) only when the LAST owner releases —
        /// identical SPIR-V is deduplicated to a single shared module (see `add`),
        /// so the module must outlive every material/instance that compiled it.
        void remove(ShaderHandle handle);

    private:
        /// One cached unique-SPIR-V → slot mapping. `bytes` is kept for an exact
        /// (collision-proof) compare on a hash hit; many materials that compile to
        /// byte-identical SPIR-V collapse onto one slot (→ one PSO downstream).
        struct SpirvCacheRec
        {
            std::vector<std::byte> bytes;
            uint32_t               slot{0};
        };

        VkDevice                    device_{VK_NULL_HANDLE};
        std::vector<ShaderObject>   records_;
        std::vector<uint32_t>       gens_;       ///< generation counter per slot
        std::vector<uint32_t>       refcount_;   ///< owners per slot (parallel to records_); 0 = dead
        std::vector<std::size_t>    slot_hash_;  ///< SPIR-V hash per slot (0 = not from a deduped SPIR-V add)
        std::vector<uint32_t>       free_;       ///< reusable slot indices
        std::unordered_map<std::size_t, std::vector<SpirvCacheRec>> spirv_cache_; ///< SPIR-V hash → slots
    };

} // namespace lux::render
