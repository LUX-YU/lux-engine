#pragma once
/**
 * @file VertexProduction.hpp
 * @brief Per-scene registry of compute-vertex *producers* + type-safe RG keys.
 *
 * Stage S1.1 of the render-architecture review follow-up
 * (.internal/render_architecture_review.md, issues #1 / H1 / 3.1 / 3.2).
 *
 * Problem this solves:
 *
 *   Before this, the only cross-feature link between a vertex *producer*
 *   (SkinningFeature: compute-writes skinned vertices into a transient pool)
 *   and its *consumers* (the mesh-draw features that read that pool so the RG
 *   orders production → draw) was a magic string literal "SkinnedVertexPool"
 *   duplicated on both sides. A typo silently dead-pruned the draw pass, and
 *   every consumer hard-coded skinning specifically — adding a morph/cloth
 *   producer meant editing every consumer.
 *
 * Design (mirrors the engine's two idioms — a contributor set + a per-scene
 * service locator):
 *
 *   - EVertexProductKind + vertexProductRgName(): an enum drives the stable
 *     RG buffer name, so a wrong name is a COMPILE error, not a silent prune.
 *   - VertexProductionRegistry: lives in RenderScene::sceneRegistry(). A
 *     producer publish()es itself once (in its init); mesh-draw consumers
 *     iterate producers() and declare an RG read on each output by name. The
 *     record carries only the STABLE name (not a per-compile RGResourceHandle),
 *     so binding stays order-independent via RGBuilder's forward-reference
 *     (referenceBuffer) resolution — the registry handles discovery, the enum
 *     handles naming, forward-ref handles ordering.
 *
 *   Net effect: add a new producer kind → every mesh consumer orders after it
 *   with zero consumer edits.
 */

#include <cstdint>
#include <span>
#include <vector>

#include <lux/engine/function/visibility.h>

namespace lux::render
{
    /// A compute-vertex producer category. Append new producers here; each maps
    /// to a stable RG buffer name via vertexProductRgName().
    enum class EVertexProductKind : std::uint8_t
    {
        Skinning = 0, ///< SkinningFeature — linear-blend skinned vertices
        Morph = 1,    ///< (reserved) morph-target blending
        Cloth = 2,    ///< (reserved) GPU cloth simulation
    };

    /// Stable RG buffer name a producer of @p kind imports (importBuffer) and
    /// consumers reference (referenceBuffer). Enum-driven so a typo is a compile
    /// error rather than the old stringly-typed "SkinnedVertexPool" magic
    /// literal. Returns a static string literal; never nullptr.
    [[nodiscard]] LUX_FUNCTION_PUBLIC const char* vertexProductRgName(EVertexProductKind kind) noexcept;

    /// One published producer. Carries only the kind + its stable RG name
    /// (== vertexProductRgName(kind)); intentionally NOT a per-compile
    /// RGResourceHandle (see header rationale). Room to grow metadata (barrier
    /// hints, output IVertexSource*) later without touching consumers.
    struct VertexProductionRecord
    {
        EVertexProductKind kind{};
        const char* rg_buffer_name{nullptr}; ///< == vertexProductRgName(kind)
    };

    /// Per-scene registry of compute-vertex producers. Owned by RenderScene's
    /// scene registry (one per scene). Producers publish in init(); consumers
    /// iterate producers() at graph-build time. Not thread-safe — touched only
    /// on the render thread (feature init + addPasses).
    class LUX_FUNCTION_PUBLIC VertexProductionRegistry
    {
    public:
        VertexProductionRegistry() = default;

        VertexProductionRegistry(const VertexProductionRegistry&) = delete;
        VertexProductionRegistry& operator=(const VertexProductionRegistry&) = delete;

        /// Register a producer of @p kind. Idempotent: re-publishing the same
        /// kind refreshes its record rather than duplicating it.
        void publish(EVertexProductKind kind);

        /// Remove a producer of @p kind (e.g. on feature shutdown). No-op if
        /// not present.
        void unpublish(EVertexProductKind kind) noexcept;

        /// All currently-published producers. Stable for the duration of a
        /// graph compile (producers publish in init, before addPasses runs).
        [[nodiscard]] std::span<const VertexProductionRecord> producers() const noexcept
        {
            return records_;
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return records_.empty();
        }

    private:
        std::vector<VertexProductionRecord> records_;
    };

} // namespace lux::render
