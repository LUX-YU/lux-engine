#pragma once
/**
 * @file GpuDrivenMeshExtFlags.hpp
 * @brief The extension-flag bit space shared by every GPU-driven mesh feature
 *        (DeferredGBuffer / ForwardMesh / MeshShadow).
 *
 * These bits travel in each feature's comm config `extension_flags` word and
 * select build-time pipeline variants (which cull kernel, which merged scope).
 * They were previously declared once per feature — five declarations in all,
 * three in Operation headers and two more as .cpp-local constants — with nothing
 * tying the numbering together.
 *
 * That is not a hypothetical hazard. It already cost a wire-format version bump:
 * DeferredGBuffer's LocalReadScope was 1<<1, the same bit a (never-consumed)
 * Bindless flag claimed, and untangling them required
 * kDeferredGBufferCommConfigVersion 3 → 4. The bit that collision freed is still
 * carrying a comment asking the next person not to reuse it.
 *
 * One enum removes the class of problem: the numbering exists once, the compiler
 * sees every bit at the point a new one is added, and Flags<EGpuDrivenMeshExt>
 * will not silently assign into some other feature family's flag word.
 *
 * NOT every feature accepts every bit — a feature validates the subset it
 * understands (MeshCommConfigValidation's known-flags check). Sharing the SPACE
 * is what matters; sharing the vocabulary is not required.
 */

#include <lux/cxx/core/EnumFlags.hpp>

#include <cstdint>

namespace lux::render
{
    /// Build-time pipeline-variant selectors for the GPU-driven mesh features.
    enum class EGpuDrivenMeshExt : std::uint32_t
    {
        None = 0,

        /// Use the HZB-occlusion variant of the unified cull kernel. The no-HZB
        /// variant does not declare descriptor set 1, so this is a genuine
        /// pipeline-layout fork, not a spec-constant fold
        /// (VUID-VkComputePipelineCreateInfo-layout-07988).
        HZB = 1u << 0,

        /// Sample material textures through the bindless combined set.
        /// Accepted by ForwardMesh and MeshShadow; DeferredGBuffer leaves this
        /// bit unused (see the note below).
        Bindless = 1u << 1,

        /// This G-buffer takes part in a local-read merged scope: the deferred
        /// lighting consumer reads it through input attachments inside ONE
        /// rendering scope. Effective only where the device enables
        /// KHR_dynamic_rendering_local_read — the feature gates on DeviceCaps
        /// with the SAME condition the lighting consumer resolves, so producer
        /// and consumer stay pipeline-remap consistent. DeferredGBuffer only.
        LocalReadScope = 1u << 2,
    };
} // namespace lux::render

template <> struct lux::cxx::enable_enum_flags<lux::render::EGpuDrivenMeshExt> : std::true_type
{
};

namespace lux::render
{
    using GpuDrivenMeshExtFlags = lux::cxx::EnumFlags<EGpuDrivenMeshExt>;

    // DeferredGBuffer historically reserved bit 1 after the v4 untangling and
    // asked that it not be reused "for a differently-meaning flag". Under one
    // shared space that request is now satisfied structurally rather than by
    // comment: bit 1 IS Bindless everywhere, and a feature that does not accept
    // it simply leaves it out of its known-flags set.
    static_assert(
        static_cast<std::uint32_t>(EGpuDrivenMeshExt::Bindless) == (1u << 1),
        "bit 1 stays Bindless — DeferredGBuffer's reserved bit is this one");

} // namespace lux::render
