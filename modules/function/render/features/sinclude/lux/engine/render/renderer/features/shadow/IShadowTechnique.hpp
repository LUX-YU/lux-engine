#pragma once
/**
 * @file IShadowTechnique.hpp
 * @brief Polymorphic interface for shadow rendering techniques.
 *
 * Each shadow technique (PCF, EVSM, MSM, VSM ...) owns ALL of its
 * technique-specific resources (atlas, page tables, pipelines), shader
 * variant id (which deferred_lighting.frag SPIR-V to bind), and per-frame
 * pass scheduling (caster passes, post passes like blur, page allocation
 * compute for VSM).
 *
 * `ShadowMapFeature` keeps the **shared** per-light state — slice metadata
 * (light VP matrices, atlas UV/tile coords), CSM cascade splits, Top-K
 * candidate selection — and dispatches the per-frame render work through
 * the active technique's virtual hooks.
 *
 * The interface is intentionally rich enough to host VSM-class techniques
 * (multi-pass with cross-frame state, feedback loops, per-page rendering).
 * See .internal/plan/evsm-shadow-implementation-guide.md §8 for the
 * compatibility argument.
 *
 * C2 status: the interface ships with empty default implementations for
 * every hook so PCF and EVSM stub classes can declare themselves without
 * fully rewiring `MeshShadowFeature` yet. C3+ progressively fills in real
 * caster/post hook bodies for EVSM; the PCF path keeps using the legacy
 * `ShadowResources` + `MeshShadowFeature` data flow until everything is
 * routed through this interface.
 */

#include <lux/engine/function/render/client/resources/lighting/EShadowTechnique.hpp>
#include <lux/engine/function/render/client/resources/EBuiltinShader.hpp>
// EBuiltinShader enum only (avoids pulling embed byte arrays into shadow TUs)

#include <cstdint>

namespace lux::render
{
    class RGBuilder;
    class ShadowResources;
    class RenderScene;

    /// Per-frame context handed to a technique's render hooks. Carries what a
    /// technique's post pass (e.g. EVSM blur) needs to record itself: the graph
    /// builder, the shared shadow resources (per-view slice cache + atlas
    /// resolution) and the scene. Filled by MeshShadowFeature::addPasses.
    struct ShadowFrameContext
    {
        RGBuilder* builder = nullptr;
        ShadowResources* shadow_res = nullptr;
        RenderScene* scene = nullptr;
        uint32_t atlas_resolution = 0;
    };

    /// Polymorphic shadow-technique contract. PCF / EVSM / VSM all implement
    /// this; ShadowMapFeature owns one of each and dispatches per-frame work
    /// through `current()`.
    class IShadowTechnique
    {
    public:
        virtual ~IShadowTechnique() = default;

        // ── Startup / shutdown (once per technique lifetime). ───────────
        virtual void buildResources()
        {
        }
        virtual void destroyResources()
        {
        }

        // ── Per-frame scheduling. Each technique chooses how many and what
        //    kind of GPU passes it emits. ─────────────────────────────────
        virtual void recordFrameSetup(const ShadowFrameContext&)
        {
        }
        virtual void recordShadowPasses(const ShadowFrameContext&)
        {
        }
        virtual void recordPostFrame(const ShadowFrameContext&)
        {
        }

        // ── Caster pass declaration. The mesh shadow caster pipeline depends on
        //    the mesh vertex shader (owned by MeshShadowFeature), so a technique
        //    only DECLARES its caster shape here and MeshShadowFeature assembles
        //    the pipeline from it. Adding a technique = return its variants;
        //    MeshShadowFeature needs zero change. ──────────────────────────────
        virtual EBuiltinShader casterVertVariant() const = 0; ///< SHADOW_DEPTH_VERT / MESH_SHADOW_VERT / ...
        virtual EBuiltinShader casterFragVariant() const = 0; ///< SHADOW_DEPTH_FRAG / SHADOW_EVSM_CASTER_FRAG / ...
        virtual const char* casterColorTarget() const
        {
            return nullptr;
        } ///< null = depth-only; else a named RG color target (EVSM moment atlas)
        virtual uint32_t casterColorWriteMask() const
        {
            return 0u;
        } ///< 0 = depth-only; 0xF = RGBA color (EVSM)

        // ── Lighting-side binding. ──────────────────────────────────────
        /// Returns the SPIR-V variant to use for the deferred-lighting (and
        /// forward PBR) fragment shader for this technique. Baked into the
        /// asset registry at build time by `compile_and_pack_shader_variants`
        /// — see modules/function/render/CMakeLists.txt and §3 / C1 of the
        /// EVSM plan.
        virtual EBuiltinShader lightingFragVariantDeferred() const = 0;
        virtual EBuiltinShader lightingFragVariantForwardPBR() const
        {
            return lightingFragVariantDeferred();
        }

        // ── Metadata. ───────────────────────────────────────────────────
        virtual EShadowTechnique id() const = 0;
        virtual const char* name() const
        {
            return toString(id());
        }
    };

} // namespace lux::render
