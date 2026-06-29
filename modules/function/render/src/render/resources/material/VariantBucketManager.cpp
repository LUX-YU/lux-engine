#include <lux/engine/render/resources/material/VariantBucketManager.hpp>

namespace lux::render
{
    uint32_t VariantBucketManager::getOrCreate(ELightingTechnique family,
                                               ShaderFeatureMask  feature_mask)
    {
        const uint64_t key = makeKey(family, feature_mask);
        if (auto it = lookup_.find(key); it != lookup_.end())
            return it->second;

        // Always allocate on first sight — no cap, no degrade. Mechanism only;
        // the upper layer polices the live bucket count (see count()).
        const uint32_t bucket = static_cast<uint32_t>(buckets_.size());
        buckets_.push_back(VariantBucketDesc{
            .family       = family,
            .feature_mask = feature_mask,
        });
        graph_refcount_.push_back(0u);  // keep parallel; family variants aren't refcounted
        lookup_.emplace(key, bucket);
        return bucket;
    }

    uint32_t VariantBucketManager::getOrCreateGraph(uint64_t               shader_key,
                                                    ShaderHandle           gbuffer_shader,
                                                    ShaderHandle           forward_shader,
                                                    lux::rdesc::EAlphaMode alpha_mode,
                                                    bool                   double_sided)
    {
        // Key on the FULL identity (both baked shader handles + render-state),
        // not the client's lossy shader_key. shader_key is only the caller's
        // "has a baked shader" gate (!= 0); trusting it as the bucket key let
        // its overlapping XOR fields collide two distinct materials onto one
        // bucket (shared PSO + frag = silent wrong rendering). W3a render-state
        // is part of the identity so distinct cull/alpha -> distinct PSO.
        (void)shader_key;
        const GraphBucketKey key{
            gbuffer_shader.index, gbuffer_shader.gen,
            forward_shader.index, forward_shader.gen,
            static_cast<uint8_t>(alpha_mode), double_sided,
        };
        if (auto it = graph_lookup_.find(key); it != graph_lookup_.end())
        {
            ++graph_refcount_[it->second];   // another material shares this bucket
            return it->second;
        }

        // Always allocate on first sight — no cap, no degrade (the draw path is
        // MDC-dynamic; BucketPipelinePool is a dynamic vector). The live bucket
        // count is reported via count() for the upper layer to police; this layer
        // enforces no PSO-count policy. Buckets are refcounted and recycled via
        // release() on material destroy.
        const VariantBucketDesc desc{
            .family               = ELightingTechnique::Graph,
            // DOUBLE_SIDED here drives BucketPipelinePool::pick()'s cull-tier
            // selection (it reads feature_mask); set in lock-step with
            // graph_double_sided from the same param so the PSO is registered under
            // the tier pick() will request. Other feature bits are inert for graph
            // buckets (the baked frag shader, not feature_mask, defines the variant).
            .feature_mask         = double_sided
                                      ? static_cast<ShaderFeatureMask>(EShaderFeature::DOUBLE_SIDED)
                                      : ShaderFeatureMask{ 0u },
            .graph_gbuffer_shader = gbuffer_shader,
            .graph_forward_shader = forward_shader,
            .graph_alpha_mode     = alpha_mode,
            .graph_double_sided   = double_sided,
        };

        // Reuse a freed graph-bucket id before growing (bounds the high-water mark
        // to peak concurrent live variants). Reuse is safe: release() is only
        // reached via handleDestroyMaterial, which invalidates all scene graphs,
        // so a recompile rebinds bucket_id -> PSO before any instance can reference
        // the reused id.
        uint32_t bucket;
        if (!graph_free_list_.empty())
        {
            bucket = graph_free_list_.back();
            graph_free_list_.pop_back();
            buckets_[bucket]        = desc;
            graph_refcount_[bucket] = 1u;
        }
        else
        {
            bucket = static_cast<uint32_t>(buckets_.size());
            buckets_.push_back(desc);
            graph_refcount_.push_back(1u);
        }
        graph_lookup_.emplace(key, bucket);
        return bucket;
    }

    void VariantBucketManager::release(uint32_t bucket_id) noexcept
    {
        // Family/bootstrap buckets are permanent.
        if (bucket_id < kLightingTechniqueCount)
            return;
        if (bucket_id >= graph_refcount_.size() || graph_refcount_[bucket_id] == 0u)
            return;  // defensive: never seen / already free
        if (--graph_refcount_[bucket_id] != 0u)
            return;  // still referenced by other materials

        // Last reference dropped: free the bucket. Erase its graph_lookup_ key
        // (cold path; the map is bounded by live graph buckets), blank the slot so
        // registerGraphBucketPipelines skips it (family != Graph), and recycle the
        // id. The slot stays in buckets_ so existing bucket ids remain stable.
        for (auto it = graph_lookup_.begin(); it != graph_lookup_.end(); ++it)
        {
            if (it->second == bucket_id)
            {
                graph_lookup_.erase(it);
                break;
            }
        }
        buckets_[bucket_id] = VariantBucketDesc{};  // family defaults to Unlit -> skipped
        graph_free_list_.push_back(bucket_id);
    }

    void VariantBucketManager::seedFamilyBootstrapBuckets()
    {
        buckets_.clear();
        lookup_.clear();
        graph_lookup_.clear();
        graph_refcount_.clear();
        graph_free_list_.clear();
        buckets_.reserve(kLightingTechniqueCount);

        for (uint32_t family_index = 0; family_index < kLightingTechniqueCount; ++family_index)
        {
            const auto family = static_cast<ELightingTechnique>(family_index);
            buckets_.push_back(VariantBucketDesc{
                .family       = family,
                .feature_mask = 0u,
            });
            graph_refcount_.push_back(0u);  // family bootstrap buckets are pinned
            lookup_.emplace(makeKey(family, 0u), family_index);
        }
    }

} // namespace lux::render
