#pragma once
// =============================================================================
//  SpirvPatcher.hpp — overwrites descriptor positions in SPIR-V according to
//  a LayoutPlan.
// -----------------------------------------------------------------------------
//  The emitter injects the canonical position into GLSL offline, and glslc
//  compiles it into two decoration instructions in the SPIR-V:
//      OpDecorate %var DescriptorSet <literal>
//      OpDecorate %var Binding       <literal>
//  This file rewrites those two literals at runtime to the position
//  computed by LayoutPlan. From this point on, canonical is demoted to
//  just "the initial value before patching" — it still needs to exist
//  (offline compilation needs some position to compile against), but it is
//  no longer the final truth.
//
//  Why rewrite the literals instead of recompiling: changing these two
//  32-bit words changes neither the instruction length nor any id, so
//  there is no need to re-layout the module, invoke shaderc, or re-run
//  reflection. The whole operation is a single O(instruction count) linear
//  scan, and it's fully reversible. By contrast, "rerun glslc against the
//  Plan" would drag the compiler into the runtime, with unacceptable
//  startup cost and dependencies.
//
//  Why not use spirv-cross / spirv-opt: they could do this, but they'd
//  bring in a full SPIR-V semantic model just to perform one purely
//  literal substitution. All that's needed here is understanding a single
//  instruction, OpDecorate.
//
//  Design: .internal/lux-engine-descriptor-layout-architecture.md §4.4
// =============================================================================

#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <span>
#include <vector>

namespace lux::render
{
    /// One position-relocation instruction: moves the resource at
    /// (from_set, from_binding) to (to_set, to_binding). Matched by its
    /// *pre-patch position* — i.e. the canonical position.
    struct SpirvRelocation
    {
        uint32_t from_set{0};
        uint32_t from_binding{0};
        uint32_t to_set{0};
        uint32_t to_binding{0};
    };

    /// The result of a rewrite.
    struct SpirvPatchResult
    {
        /// How many variables were actually changed (a variable counts at
        /// most once, even if both its set and binding changed).
        uint32_t relocated{0};
        /// The total number of variables in the module carrying a (set,
        /// binding) decoration. The caller reconciles this against the
        /// reflection's total binding count: a descriptor existing in the
        /// module but missing from reflection means reflection is a subset
        /// of the module (e.g. the asset-baking reflection tool
        /// under-reported something), and relocating by a subset of
        /// reflection leaves un-relocated leftover references — the layout
        /// is built from reflection, so a leftover reference's set doesn't
        /// exist in the layout.
        uint32_t descriptor_count{0};
        /// Whether parsing succeeded. When false, `words` was left
        /// unmodified — better to use the original as-is than to produce a
        /// module that may have been corrupted.
        bool ok{false};
        /// The failure reason (diagnostics only).
        const char* error{nullptr};
    };

    /// Rewrites descriptor positions in `words` in place.
    ///
    /// Matching approach: first scan once to collect each variable id's
    /// current (set, binding), then decide where it moves to using the
    /// *pre-patch position* in `relocations`. Two passes are necessary — a
    /// variable's DescriptorSet and Binding are two separate instructions
    /// whose order isn't guaranteed, so a single-pass rewrite could read a
    /// half-old, half-new position.
    ///
    /// Idempotence: running the same batch of relocations again after a
    /// rewrite does not relocate a second time (unless a relocation's
    /// target happens to be another relocation's source, in which case the
    /// caller has specified a chained relocation on purpose).
    [[nodiscard]] LUX_FUNCTION_PUBLIC SpirvPatchResult
    patchSpirvDescriptorPositions(std::span<uint32_t> words, std::span<const SpirvRelocation> relocations) noexcept;

    /// The copying variant: leaves the input untouched and returns the
    /// rewritten copy in `out`. On failure, `out` remains a copy of the
    /// input.
    [[nodiscard]] LUX_FUNCTION_PUBLIC SpirvPatchResult patchSpirvDescriptorPositions(
        std::span<const uint32_t> words,
        std::span<const SpirvRelocation> relocations,
        std::vector<uint32_t>& out
    );

} // namespace lux::render
