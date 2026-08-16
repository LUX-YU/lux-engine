#pragma once
// ============================================================================
//  VariantBucketManager.hpp — owns the variant-bucket registry.
//
//  Extracted from MaterialResources (structural refactor S9) so that ALL
//  bucket-routing keying lives in ONE cohesive owner: the
//  (family, feature_mask) -> bucket_id allocation, the descriptor list, and the
//  capacity-overflow accounting. A variant bucket is the routing unit the
//  GPU-driven draw uses to pick a PSO (the draw kernel indexes
//  pipeline_variants[bucket_id]).
//
//  R1 will swap the Graph-family bucket key from feature_mask to a baked-shader
//  fingerprint HERE, without touching MaterialResources' slot/SSBO APIs.
// ============================================================================
#include <lux/engine/render/resources/material/MaterialFamily.hpp>        // ELightingTechnique, kLightingTechniqueCount
#include <lux/engine/render/gpu/pipeline/ShaderPermutation.hpp> // ShaderFeatureMask
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>        // ShaderHandle
#include <lux/engine/description/MaterialEnums.hpp>           // rdesc::EAlphaMode
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lux::render
{
    /// A variant bucket descriptor. The routing unit for GPU-driven draw PSO
    /// selection: (family, feature_mask) today; R1 keys the Graph family by a
    /// baked-shader id.
    struct VariantBucketDesc
    {
        ELightingTechnique family{ ELightingTechnique::Unlit };
        ShaderFeatureMask  feature_mask{ 0 };
        // Graph family only: the material's baked per-pass frag shaders. The
        // mesh feature builds this bucket's own PSO from them (R1). Null for
        // builtin families and for Graph materials uploaded without a shader
        // (those fall back to the Graph family bootstrap pipeline).
        ShaderHandle graph_gbuffer_shader{};
        ShaderHandle graph_forward_shader{};
        // Graph family render-state (W3a). Folded into the bucket key (makeGraphKey)
        // so distinct render-state -> distinct PSO. graph_double_sided drives the
        // cull-mode tier in the per-bucket PSO build; graph_alpha_mode is carried
        // for the (deferred) transparent-routing decision (Blend is not yet a
        // separate pass -> currently rendered opaque, see GpuDrivenMeshFeatureBase).
        lux::rdesc::EAlphaMode graph_alpha_mode{ lux::rdesc::EAlphaMode::Opaque };
        bool                   graph_double_sided{ false };
    };

    class LUX_FUNCTION_PUBLIC VariantBucketManager
    {
    public:
        /// Seed one bootstrap bucket per family (feature_mask = 0). Graph
        /// materials add their own buckets on top. (Was
        /// MaterialResources::resetVariantBuckets — renamed for honesty.)
        void seedFamilyBootstrapBuckets();

        /// Look up or allocate the bucket for (family, feature_mask). Always
        /// allocates a distinct bucket on first sight — no cap, no degrade. The
        /// live bucket count is a pure mechanism number (see count()); any
        /// "too many" policy belongs to the upper layer, not here.
        [[nodiscard]] uint32_t getOrCreate(ELightingTechnique family,
                                           ShaderFeatureMask  feature_mask);

        /// Look up or allocate a bucket for a Graph material identified by its
        /// baked shader: distinct @p shader_key -> distinct bucket (its own PSO);
        /// same key -> shared bucket. Uses a SEPARATE key space (graph_lookup_)
        /// so the 64-bit shader_key never collides with the (family,feature_mask)
        /// key. Always allocates on first sight — there is no cap and no degrade
        /// (the draw path is MDC-dynamic; BucketPipelinePool is a dynamic vector).
        /// The live bucket count is exposed via count() for the upper layer to
        /// police; this layer enforces no PSO-count policy of its own.
        ///
        /// W3a: @p alpha_mode + @p double_sided are folded into the bucket key
        /// (makeGraphKey) so two materials sharing baked shaders but differing in
        /// render-state get DISTINCT buckets (distinct PSOs).
        [[nodiscard]] uint32_t getOrCreateGraph(uint64_t               shader_key,
                                                ShaderHandle           gbuffer_shader,
                                                ShaderHandle           forward_shader,
                                                lux::rdesc::EAlphaMode alpha_mode,
                                                bool                   double_sided);

        /// Release one reference to a bucket (call when a material using it is
        /// destroyed). When a GRAPH bucket's refcount hits zero its graph_lookup_
        /// key is erased and the id is recycled (reused by a later getOrCreateGraph
        /// before growing the vector). Family/bootstrap buckets
        /// (id < kLightingTechniqueCount) are permanent — release is a no-op for
        /// them. Safe to reuse a freed id because the caller (handleDestroyMaterial)
        /// invalidates all scene graphs, forcing a recompile that rebinds
        /// bucket_id -> PSO before any new instance can reference the reused id.
        void release(uint32_t bucket_id) noexcept;

        [[nodiscard]] uint32_t count() const noexcept
        {
            return static_cast<uint32_t>(buckets_.size());
        }

        [[nodiscard]] VariantBucketDesc at(uint32_t bucket_id) const noexcept
        {
            return bucket_id < buckets_.size() ? buckets_[bucket_id] : VariantBucketDesc{};
        }

        void clear() noexcept
        {
            buckets_.clear();
            lookup_.clear();
            graph_lookup_.clear();
            graph_refcount_.clear();
            graph_free_list_.clear();
        }

        [[nodiscard]] static uint64_t makeKey(ELightingTechnique family,
                                              ShaderFeatureMask  feature_mask) noexcept
        {
            return (static_cast<uint64_t>(family) << 32u)
                 | static_cast<uint64_t>(feature_mask);
        }

    private:
        // Graph bucket identity = the two baked shader handles + render-state.
        // Keyed on the FULL identity (not the client's lossy 64-bit shader_key,
        // whose XOR-packed fields overlap and collide — e.g. fw{2,1} and fw{3,257}
        // produce the same key, routing the second material to the first's PSO and
        // baked frag = silent wrong rendering). Both handles are already passed in,
        // so the full identity is free to key on.
        struct GraphBucketKey
        {
            uint32_t gb_index, gb_gen, fw_index, fw_gen;
            uint8_t  alpha_mode;
            bool     double_sided;
            bool operator==(const GraphBucketKey&) const noexcept = default;
        };
        struct GraphBucketKeyHash
        {
            size_t operator()(const GraphBucketKey& k) const noexcept
            {
                size_t h = k.gb_index;
                auto mix = [&h](uint64_t v) {
                    h ^= static_cast<size_t>(v) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
                };
                mix(k.gb_gen); mix(k.fw_index); mix(k.fw_gen);
                mix((static_cast<uint64_t>(k.alpha_mode) << 1) | (k.double_sided ? 1u : 0u));
                return h;
            }
        };

        std::vector<VariantBucketDesc>         buckets_;
        std::unordered_map<uint64_t, uint32_t> lookup_;        // (family,feature_mask) key
        std::unordered_map<GraphBucketKey, uint32_t, GraphBucketKeyHash> graph_lookup_; // full graph identity -> bucket
        // Per-bucket-slot live-material refcount (indexed by bucket id; family
        // bootstrap slots are pinned and ignored). When a graph slot hits 0 it is
        // freed: graph_lookup_ key erased, slot blanked, id pushed to graph_free_list_.
        std::vector<uint32_t>                  graph_refcount_;
        std::vector<uint32_t>                  graph_free_list_; // recyclable graph bucket ids
    };

} // namespace lux::render
