#include <lux/engine/render/renderer/features/shadow/MeshShadowFeature.hpp>
#include <lux/engine/function/render/client/resources/lighting/EShadowTechnique.hpp> // EShadowTechnique (technique id)
#include <lux/engine/render/renderer/features/shadow/IShadowTechnique.hpp>
// IShadowTechnique + ShadowFrameContext — polymorphic caster/post dispatch
#include <vk_mem_alloc.h>
#include <lux/engine/render/core/FrustumCuller.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/EngineSetShapes.hpp>
#include <lux/engine/render/gpu/pipeline/ShaderPermutation.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/gpu/pipeline/PipelinePresets.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/resources/lighting/ShadowResources.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/gpu/lifecycle/DeferredDestroyQueue.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp> // set-3 bind
#include <lux/engine/render/resources/vertex/VertexProduction.hpp>   // producer registry
#include <lux/engine/render/gpu/pipeline/VertexLayoutRegistry.hpp>   // vertex-layout SSOT
#include <lux/engine/render/gpu/pipeline/VertexLayoutSpec.hpp>       // appendVertexLayoutSpecs
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/function/render/client/features/deferred/DeferredGBufferOperation.hpp>
// kDeferredGBufferDrawPassName
#include <lux/engine/function/render/client/features/shadow/ShadowMapOperation.hpp>         // kShadowViewUploadPassName
#include <lux/engine/function/render/client/features/shadow/MeshShadowOperation.hpp>        // kMeshShadowDrawPassName
#include <lux/engine/render/scene/View.hpp> // View::handle (canonical-view resolution)
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/VulkanCheck.hpp>
#include <lux/engine/function/render/client/resources/lighting/ShadowMapTypes.hpp>
#include <lux/engine/render/resources/mesh/GpuDrivenMeshConsts.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

namespace lux::render
{
    namespace
    {

#if !defined(LUX_SHADOW_DEBUG_MULTI_VIEW)
#if !defined(NDEBUG)
#define LUX_SHADOW_DEBUG_MULTI_VIEW 1
#else
#define LUX_SHADOW_DEBUG_MULTI_VIEW 0
#endif
#endif

#if LUX_SHADOW_DEBUG_MULTI_VIEW
        [[nodiscard]] uint64_t fnv1a64(std::span<const std::byte> bytes)
        {
            uint64_t h = 1469598103934665603ull;
            for (std::byte b : bytes)
            {
                h ^= static_cast<uint8_t>(b);
                h *= 1099511628211ull;
            }
            return h;
        }

        [[nodiscard]] uint64_t hashLightVp(const Eigen::Matrix4f& m)
        {
            const auto* p = reinterpret_cast<const std::byte*>(m.data());
            return fnv1a64({p, sizeof(float) * 16});
        }

        [[nodiscard]] uint64_t summarizeSlices(std::span<const ShadowSliceGPU> slices)
        {
            if (slices.empty())
                return 0ull;
            const uint64_t first = hashLightVp(slices.front().light_vp);
            const uint64_t last = hashLightVp(slices.back().light_vp);
            return first ^ ((last << 1) | (last >> 63)) ^ static_cast<uint64_t>(slices.size());
        }
#endif

        /// Compute the shadow cull SSBO size for a given number of slices.
        /// Layout (std430): 6*S planes, S group indices, S origin pages and
        /// S origin-local/page-size records.
        [[nodiscard]] VkDeviceSize shadowCullSsboSize(uint32_t slice_count)
        {
            const VkDeviceSize planes_bytes = static_cast<VkDeviceSize>(slice_count) * 6u * sizeof(Frustum::Plane);
            const VkDeviceSize metadata_bytes = static_cast<VkDeviceSize>(slice_count) * 3u * sizeof(Frustum::Plane);
            return planes_bytes + metadata_bytes;
        }

        [[nodiscard]] uint32_t clampShadowSliceCount(std::span<const ShadowSliceGPU> slices, uint32_t max_shadow_slices)
        {
            return static_cast<uint32_t>(std::min<size_t>(slices.size(), max_shadow_slices));
        }

        [[nodiscard]] uint32_t atlasUvToPixel(float uv, uint32_t atlas_resolution)
        {
            const float scaled = uv * static_cast<float>(atlas_resolution);
            const int32_t rounded = static_cast<int32_t>(std::lround(scaled));
            return static_cast<uint32_t>(std::clamp(rounded, 0, static_cast<int32_t>(atlas_resolution)));
        }

        [[nodiscard]] VkRect2D makeSliceTileScissor(const ShadowSliceGPU& slice, uint32_t atlas_resolution)
        {
            const uint32_t safe_resolution = std::max(atlas_resolution, 1u);

            uint32_t x0 = std::min(atlasUvToPixel(slice.atlas_uv_bias.x(), safe_resolution), safe_resolution);
            uint32_t y0 = std::min(atlasUvToPixel(slice.atlas_uv_bias.y(), safe_resolution), safe_resolution);
            uint32_t x1 = std::min(
                atlasUvToPixel(slice.atlas_uv_bias.x() + slice.atlas_uv_scale.x(), safe_resolution),
                safe_resolution
            );
            uint32_t y1 = std::min(
                atlasUvToPixel(slice.atlas_uv_bias.y() + slice.atlas_uv_scale.y(), safe_resolution),
                safe_resolution
            );

            if (x0 >= safe_resolution)
            {
                x0 = safe_resolution - 1u;
                x1 = safe_resolution;
            }
            if (y0 >= safe_resolution)
            {
                y0 = safe_resolution - 1u;
                y1 = safe_resolution;
            }

            if (x1 <= x0)
                x1 = std::min(x0 + 1u, safe_resolution);
            if (y1 <= y0)
                y1 = std::min(y0 + 1u, safe_resolution);

            VkRect2D scissor{};
            scissor.offset = {
                static_cast<int32_t>(x0),
                static_cast<int32_t>(y0),
            };
            scissor.extent = {
                x1 - x0,
                y1 - y0,
            };
            return scissor;
        }

    } // namespace

    // =========================================================================
    //  Construction / destruction
    // =========================================================================

    MeshShadowFeature::MeshShadowFeature(Config cfg) : cfg_(cfg)
    {
    }

    MeshShadowFeature::~MeshShadowFeature()
    {
        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
        {
            if (vma_ && shadow_cull_ubo_[i])
                vmaDestroyBuffer(vma_, shadow_cull_ubo_[i], shadow_cull_ubo_alloc_[i]);
            if (vma_ && shadow_mdc_info_buf_[i])
                vmaDestroyBuffer(vma_, shadow_mdc_info_buf_[i], shadow_mdc_info_alloc_[i]);
        }
        // §2.2: Layouts owned by DescriptorService — no manual destroy.
        shadow_clear_ds_layout_ = VK_NULL_HANDLE;
        shadow_visible_set_layout_ = VK_NULL_HANDLE;

        // shadow_indirect_/count_/visible_ are now RG-transient (builder.createBuffer)
        // — nothing feature-owned to destroy here.
        destroyCommon();
    }

    // =========================================================================
    //  Lifecycle
    // =========================================================================

    lux::render::Expected<void> MeshShadowFeature::initAndAttachTo(RenderScene& scene)
    {
        // ---- Ensure builtin shader defaults ----
        {
            auto& shaders = renderContext().globalRegistry().must<ShaderResources>();
            const std::array backfill{
                ShaderStageSlot{EBuiltinShader::MESH_CULL_UNIFIED_COMP, &cfg_.shadow_cull_shader},
                ShaderStageSlot{EBuiltinShader::MDC_COMPACT_COMP, &cfg_.shadow_compact_shader},
                ShaderStageSlot{EBuiltinShader::CLEAR_COUNT_BUFFERS_COMP, &cfg_.shadow_clear_shader}};
            if (auto filled = resolveShaderStages(shaders, backfill); !filled)
                return filled;
            // Legacy PCF caster shaders (shadow_vert_shader / shadow_frag_shader): the
            // caster pipeline now resolves vert/frag from the active IShadowTechnique
            // (casterVertVariant / casterFragVariant — see ensureCasterPipeline), so these
            // Config fields are read by NO pipeline. They stay in the WIRE Config for ABI
            // compatibility, but the dead builtin resolution into the runtime
            // Config is removed here; drop the fields entirely at the next ABI major.
            // (audit R-5)
        }

        auto& ctx = renderContext();
        const uint32_t hzb_mode_spec = cfg_.extension_flags.containsAll(EGpuDrivenMeshExt::HZB) ? 1u : 0u;
        vma_ = ctx.vmaAllocator();
        device_ = ctx.deviceContext().logicalDevice();

        auto* shadow_res = scene.resources().find<ShadowResources>();
        max_shadow_slices_ = (shadow_res && shadow_res->isInitialized()) ? shadow_res->maxSlices() : kMaxShadowSlices;

        // Shared instance storage: OWNED (ensure<>d AND init()ed) by
        // StandardMeshStack. MeshShadow only find<>s it, so installing MeshShadow
        // without StandardMeshStack leaves it null. Fail the install explicitly
        // instead of dereferencing null. (— this dependency should also be declared
        // in the MeshShadow FeatureDescriptor once StandardMeshStack has a
        // FeatureTypeId.)
        instance_res_ = scene.resources().find<InstanceResources>();
        if (!instance_res_)
            return renderFailure<err::resource::NotFound>();

        // Shadow indirect / count / visible buffers are RG-transient: created
        // per-frame via builder.createBuffer() in addPasses(), sized from the
        // live shadow MDC count (no fixed cap → no GPU OOB on large scenes).

        // Cull descriptor layout + frustum UBO (no persistent descriptor set)
        createCullResources();

        if (shadow_visible_set_layout_ == VK_NULL_HANDLE)
        {
            auto id = ctx.descriptorService().registerLayout(storageBufferVertexLayout("ShadowVisibleSetLayout"));
            shadow_visible_set_layout_ = ctx.descriptorService().layout(id);
        }

        // Shadow cull compute pipeline
        {
            auto& shaders = ctx.globalRegistry().must<ShaderResources>();
            auto* shader_obj = shaders.get(cfg_.shadow_cull_shader);
            // A missing cull shader is a HARD init failure. Returning {} (== success)
            // here would install a half-built feature whose later dispatch binds a null
            // pipeline — the exact Release-only silent-success the audit flagged.
            if (!shader_obj)
                return renderFailure<err::asset::Invalid>();

            const VkPushConstantRange pc{
                VK_SHADER_STAGE_COMPUTE_BIT,
                0,
                static_cast<uint32_t>(sizeof(MeshCullPushConstants))};
            const std::array layouts{cull_set_layout_};
            const std::array pcs{pc};
            auto pl = ctx.pipelineLayoutService().getOrCreate(
                {.set_layouts = layouts, .push_constants = pcs, .debug_name = "MeshShadowCullLayout"}
            );
            if (!pl)
                return lux::cxx::unexpected(pl.error());
            const std::array<GraphicsPipelineTemplate::ShaderSpecializationValue, 3> cull_specs{{
                {VK_SHADER_STAGE_COMPUTE_BIT, 0u, 1u},
                {VK_SHADER_STAGE_COMPUTE_BIT, 2u, kGeometryKindCount},
                {VK_SHADER_STAGE_COMPUTE_BIT, kSpecConstHZBMode, hzb_mode_spec},
            }};
            shadow_cull_pipeline_ =
                ctx.pipelineManager().registerComputePipeline(shader_obj->module, pl.value(), cull_specs);
        }

        // Shadow compact compute pipeline (replaces finalize for MDC mode)
        if (auto r = initCompactPipeline(cfg_.shadow_compact_shader, "MeshShadowCompactLayout"); !r)
            return lux::cxx::unexpected(r.error()); // propagate, don't swallow

        // Shadow MDC info buffer (CPU-writable, persistent mapping) — per-FIF so
        // the CPU never overwrites a slot an in-flight frame's cull/compact reads.
        {
            constexpr VkDeviceSize kInitialMdcInfoSize = 256u * 1024u; // 256KB — enough for 16K shadow MDCs
            for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
            {
                VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                ci.size = kInitialMdcInfoSize;
                ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
                aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

                VmaAllocationInfo alloc_info{};
                VK_CHECK(vmaCreateBuffer(
                    vma_,
                    &ci,
                    &aci,
                    &shadow_mdc_info_buf_[i],
                    &shadow_mdc_info_alloc_[i],
                    &alloc_info)
                );
                shadow_mdc_info_mapped_[i] = alloc_info.pMappedData;
                shadow_mdc_info_buf_size_[i] = kInitialMdcInfoSize;
            }
        }

        if (cfg_.shadow_clear_shader.isValid())
        {
            auto& shaders = ctx.globalRegistry().must<ShaderResources>();
            auto* shader_obj = shaders.get(cfg_.shadow_clear_shader);
            // Configured (valid()) but unresolvable clear shader is a HARD failure, not
            // a silent skip that leaves shadow_clear_pipeline_ null.
            if (!shader_obj)
                return renderFailure<err::asset::Invalid>();
            {
                std::array<VkDescriptorSetLayoutBinding, 8> bindings{};
                for (uint32_t i = 0; i < static_cast<uint32_t>(bindings.size()); ++i)
                {
                    bindings[i].binding = i;
                    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    bindings[i].descriptorCount = 1;
                    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                    bindings[i].pImmutableSamplers = nullptr;
                }

                auto clear_id =
                    ctx.descriptorService().registerLayout({.bindings = bindings, .debug_name = "ShadowClearDSLayout"});
                shadow_clear_ds_layout_ = ctx.descriptorService().layout(clear_id);

                const VkPushConstantRange clear_pc{
                    VK_SHADER_STAGE_COMPUTE_BIT,
                    0,
                    static_cast<uint32_t>(sizeof(MeshCullPushConstants))};
                const std::array clear_layouts{shadow_clear_ds_layout_};
                const std::array clear_pcs{clear_pc};
                auto clear_layout = ctx.pipelineLayoutService().getOrCreate(
                    {.set_layouts = clear_layouts,
                     .push_constants = clear_pcs,
                     .debug_name = "MeshShadowClearCountersLayout"}
                );
                if (!clear_layout)
                    return lux::cxx::unexpected(clear_layout.error());

                shadow_clear_pipeline_ =
                    ctx.pipelineManager().registerComputePipeline(shader_obj->module, clear_layout.value());
            }
        }

        return {};
    }

    void MeshShadowFeature::populateFrameContext(RGFrameContext& frame_ctx)
    {
        GpuDrivenMeshFeatureBase::populateFrameContext(frame_ctx);

        // §2.6: Use member shadow_frame_ext_data_ (already filled by onFrameBegin)
        // instead of thread_local, eliminating TLS lookup overhead.
        const auto sslot = shadowFrameExtSlot();
        if (sslot != kInvalidExtSlot)
            frame_ctx.ext_data[sslot] = &shadow_frame_ext_data_;
    }

    void MeshShadowFeature::onFrameBegin(const FeatureFrameContext& ctx)
    {
        GpuDrivenMeshFeatureBase::onFrameBegin(ctx);

        // Pre-compute bias groups, frustum upload payload, and lane table for
        // native shadow execution.  The Renderer reads shadowFrameData() and
        // injects it into RGFrameContext before recording.
        shadow_frame_data_ = {}; // reset
        // Reset the ext-data view in lock-step: it holds .data() pointers into
        // shadow_frame_data_'s payload vectors, which the line above just freed.
        // The early-return paths below leave it injected into RGFrameContext via
        // populateFrameContext(), so without this reset a transition to 0 shadow
        // slices (last light removed / disabled / out of range) leaves dangling
        // pointers that kUploadFrustums reads every frame (UAF).
        shadow_frame_ext_data_ = {};

        // Rotate the per-FIF MDC-info slot once per frame (mirrors the gizmo
        // transient features). The cull/compact import getter returns this same
        // slot at record time, so the CPU memcpy below never races an in-flight
        // frame still reading a different slot via binding 7.
        mdc_info_slot_ = frame_counter_++ % kMaxFramesInFlight;

        if (shadow_res_cache_ == nullptr)
            shadow_res_cache_ = renderScene().resources().find<ShadowResources>();
        auto* shadow_res = shadow_res_cache_;
        if (!shadow_res || !shadow_res->isInitialized())
        {
            shadow_mdc_count_ = 0;
            shadow_total_visible_capacity_ = 0;
            checkShadowGraphInvalidation();
            return;
        }

        // C-8: a runtime updateQuality can RAISE the slice budget and rebuild
        // ShadowResources, so re-read maxSlices() each frame. max_shadow_slices_
        // was captured once in initAndAttachTo; if it is now stale the clamp below
        // would drop the extra slices (those lights render unshadowed — their atlas
        // tiles stay at clear depth) and the cull SSBO would be undersized. Resize:
        // retire the old cull buffer through the FIF deferred-destroy queue
        // (in-flight frames still read it) and recreate at the new size. The cull
        // UBO is imported via buffer_getter and bound through a per-frame transient
        // DS, so it re-resolves with no graph rebuild.
        if (const uint32_t cur_max = shadow_res->maxSlices(); cur_max != max_shadow_slices_)
        {
            max_shadow_slices_ = cur_max;
            shadow_cull_ssbo_size_ = shadowCullSsboSize(cur_max);

            VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            ci.size = shadow_cull_ssbo_size_;
            ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
            {
                if (shadow_cull_ubo_[i] != VK_NULL_HANDLE)
                    renderContext().deferredDestroyQueue().retireBuffer(shadow_cull_ubo_[i], shadow_cull_ubo_alloc_[i]);
                vmaCreateBuffer(vma_, &ci, &aci, &shadow_cull_ubo_[i], &shadow_cull_ubo_alloc_[i], nullptr);
            }
        }

        const uint32_t scene_key = this->renderScene().sceneGlobalSlot().index;
        // Resolve the canonical view dynamically for frame-begin computation
        // (bias-group / atlas layout). View handle 0 is merely the first-created
        // view's slot id; if that view is destroyed while other views remain
        // active, findViewCache(scene,0) returns empty, slice_count collapses to 0,
        // and ALL shadow rendering stops for the surviving views even though valid
        // slices are still being cached for them. Instead, pick the first active
        // view that has a populated slice cache. The actual per-view slices are
        // still resolved at record time. Hold the snapshot for as long as `slices`
        // is read below. (medium)
        std::shared_ptr<const ShadowResources::PerViewCache> slice_cache;
        this->renderScene().forEachActiveView([&](const View& view) {
            if (slice_cache)
                return;
            auto c = shadow_res->findViewCache(scene_key, view.handle.index);
            if (c && !c->slices.empty())
                slice_cache = std::move(c);
        }
        );
        const std::span<const ShadowSliceGPU> slices =
            slice_cache ? std::span<const ShadowSliceGPU>{slice_cache->slices} : std::span<const ShadowSliceGPU>{};
        const uint32_t slice_count = clampShadowSliceCount(slices, max_shadow_slices_);
        if (slice_count == 0)
        {
            shadow_mdc_count_ = 0;
            shadow_total_visible_capacity_ = 0;
            checkShadowGraphInvalidation();
            return;
        }

        const uint32_t atlas_res = shadow_res->atlasResolution();
        buildBiasGroups(slices.subspan(0, slice_count), atlas_res);
        // Bind THIS frame's cull-UBO slot (same rotation as the MDC-info slot). The
        // kUploadFrustums write and the cull DS both resolve through this slot, so
        // they target the same buffer while in-flight frames keep their own. (P1#24)
        shadow_frame_data_.shadow_cull_ubo = shadow_cull_ubo_[mdc_info_slot_];

        // Populate generic extension data (the kernel-facing, feature-agnostic path).
        // (Already reset to {} at the top of onFrameBegin, before the early
        // returns, so its pointers never outlive shadow_frame_data_'s payloads.)
        {
            const auto& fd = shadow_frame_data_;
            shadow_frame_ext_data_.group_count = fd.bias_group_count;
            shadow_frame_ext_data_.slice_count = fd.shadow_slice_count;
            shadow_frame_ext_data_.cull_ubo = fd.shadow_cull_ubo;
            shadow_frame_ext_data_.frustum_data = fd.frustum_planes_payload.data();
            shadow_frame_ext_data_.frustum_size = static_cast<uint32_t>(fd.frustum_planes_payload.size());
            shadow_frame_ext_data_.group_map_data = fd.group_map_payload.data();
            shadow_frame_ext_data_.group_map_size = static_cast<uint32_t>(fd.group_map_payload.size());
            for (uint32_t g = 0; g < fd.bias_group_count; ++g)
            {
                auto& lane = shadow_frame_ext_data_.draw_lanes[g];
                lane.scissor = fd.bias_groups[g].scissor;
                lane.depth_bias = fd.bias_groups[g].depth_bias;
                lane.slope_bias = fd.bias_groups[g].slope_bias;
                lane.active = true;
            }
        }

        // ── Build shadow MDC info buffer ────────────────────────────────
        {
            view_mdc_count_ = mdcCount();
            const auto& fd = shadow_frame_data_;
            const uint32_t bias_group_count = fd.bias_group_count;
            shadow_mdc_count_ = bias_group_count * view_mdc_count_;

            if (shadow_mdc_count_ == 0)
            {
                shadow_total_visible_capacity_ = 0;
                // buildBiasGroups() above already populated bias_group_count from
                // the live slices, but with view_mdc_count_ == 0 there are no
                // shadow MDCs to draw, so the count/indirect buffers get sized to
                // max(shadow_mdc_count_,1) == 1 slot. Leaving the stale (>0)
                // bias_group_count would make emitShadowDrawKernel emit one draw
                // lane PER bias group (offset g*4) into that 1-slot count buffer →
                // vkCmdDrawIndexedIndirectCount countBufferOffset OOB (VUID-04129).
                // Reset it so the invariant shadow_mdc_count_ == bias_group_count *
                // view_mdc_count_ holds on this early-out and zero lanes are emitted.
                shadow_frame_data_.bias_group_count = 0;
                checkShadowGraphInvalidation();
                return;
            }

            // Count slices per bias group
            uint32_t slices_per_group[ShadowFrameData::kMaxBiasGroups]{};
            for (uint32_t s = 0; s < fd.shadow_slice_count; ++s)
                slices_per_group[fd.slice_to_group_map[s]]++;

            // Build shadow MDC data: interleaved {offset, section_id} + sentinel
            const auto& view_entries = instance_res_->mdcTable().entries();
            shadow_mdc_gpu_data_.clear();
            shadow_mdc_gpu_data_.reserve((shadow_mdc_count_ + 1) * 2);

            uint32_t running_offset = 0;
            for (uint32_t g = 0; g < bias_group_count; ++g)
            {
                for (uint32_t m = 0; m < view_mdc_count_; ++m)
                {
                    shadow_mdc_gpu_data_.push_back(running_offset);
                    shadow_mdc_gpu_data_.push_back(view_entries[m].section_id);
                    // Reserve the same sticky power-of-two capacity band the view
                    // path uses (× this group's slice count), so shadow offsets stay
                    // in lock-step with the view and stay stable across same-band
                    // instance count changes — the cull bounds its writes by these
                    // offset deltas, so the headroom is never written. (P-7)
                    running_offset += view_entries[m].capacity * slices_per_group[g];
                }
            }
            // Sentinel entry
            shadow_mdc_gpu_data_.push_back(running_offset);
            shadow_mdc_gpu_data_.push_back(0xFFFFFFFFu);
            shadow_total_visible_capacity_ = running_offset;

            // Upload into the current per-FIF slot's persistent-mapped buffer.
            const uint32_t slot = mdc_info_slot_;
            const VkDeviceSize required = static_cast<VkDeviceSize>(shadow_mdc_gpu_data_.size()) * sizeof(uint32_t);
            if (required > shadow_mdc_info_buf_size_[slot])
            {
                // Grow only this slot. Retire the old buffer through the
                // DeferredDestroyQueue (a prior in-flight frame may still
                // reference it) rather than destroying it synchronously.
                renderContext().deferredDestroyQueue().retireBuffer(
                    shadow_mdc_info_buf_[slot],
                    shadow_mdc_info_alloc_[slot]
                );
                shadow_mdc_info_mapped_[slot] = nullptr;

                VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                ci.size = required * 2; // 2× headroom
                ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
                aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo alloc_info{};
                VK_CHECK(vmaCreateBuffer(
                    vma_,
                    &ci,
                    &aci,
                    &shadow_mdc_info_buf_[slot],
                    &shadow_mdc_info_alloc_[slot],
                    &alloc_info)
                );
                shadow_mdc_info_mapped_[slot] = alloc_info.pMappedData;
                shadow_mdc_info_buf_size_[slot] = ci.size;
                // No descriptor rewrite: binding 7 is bound via the per-frame
                // transient cull DS, and the import getter is refreshed each
                // frame by the recorder.
            }

            if (shadow_mdc_info_mapped_[slot])
                std::memcpy(shadow_mdc_info_mapped_[slot], shadow_mdc_gpu_data_.data(), required);
        }

        checkShadowGraphInvalidation();
    }

    void MeshShadowFeature::checkShadowGraphInvalidation()
    {
        // Recompile when the shadow MDC count changes (lane plan + per-MDC buffers)
        // OR when the shadow visible-buffer capacity changes. The latter is sized
        // from shadow_total_visible_capacity_ = Σ(view capacity band × slices),
        // which the base layoutSerial trigger does NOT cover for a SLICE-count
        // change (new shadow light / cascade count) — only for view capacity-band
        // crossings. Because view capacity is the sticky power-of-two band, this
        // does NOT re-fire on same-band instance spawn/despawn. (P-7)
        if (shadow_mdc_count_ != last_compiled_shadow_mdc_ ||
            shadow_total_visible_capacity_ != last_compiled_shadow_capacity_)
        {
            renderScene().invalidateGraph(EGraphInvalidationReason::MDC_STORAGE_GENERATION);
            last_compiled_shadow_mdc_ = shadow_mdc_count_;
            last_compiled_shadow_capacity_ = shadow_total_visible_capacity_;
        }
    }

    // =========================================================================
    //  Bias-group computation
    // =========================================================================

    void MeshShadowFeature::buildBiasGroups(std::span<const ShadowSliceGPU> slices, uint32_t atlas_resolution)
    {
        auto& fd = shadow_frame_data_;
        fd.shadow_slice_count = static_cast<uint32_t>(slices.size());
        fd.slice_to_group_map.assign(fd.shadow_slice_count, 0u);

        // Group slices by quantized (bias, slope_bias) — bitwise comparison on
        // the float pair is intentional: slices from the same light type normally
        // share exact float values.
        struct GroupKey
        {
            float bias;
            float slope;
        };
        GroupKey keys[ShadowFrameData::kMaxBiasGroups]{};
        uint32_t group_count = 0;

        for (uint32_t s = 0; s < fd.shadow_slice_count; ++s)
        {
            const float b = slices[s].bias;
            const float sb = slices[s].slope_bias;

            uint32_t found = group_count; // sentinel: not found
            for (uint32_t g = 0; g < group_count; ++g)
            {
                if (keys[g].bias == b && keys[g].slope == sb)
                {
                    found = g;
                    break;
                }
            }

            if (found == group_count)
            {
                // New group
                if (group_count >= ShadowFrameData::kMaxBiasGroups)
                {
                    // Overflow: map to last group (best-effort).
                    found = ShadowFrameData::kMaxBiasGroups - 1;
                }
                else
                {
                    keys[group_count] = {b, sb};
                    fd.bias_groups[group_count].depth_bias = b;
                    fd.bias_groups[group_count].slope_bias = sb;
                    found = group_count;
                    ++group_count;
                }
            }

            fd.slice_to_group_map[s] = found;
        }

        fd.bias_group_count = group_count;

        // Compute per-group union scissor from constituent slice tiles.
        for (uint32_t g = 0; g < group_count; ++g)
        {
            int32_t min_x = std::numeric_limits<int32_t>::max();
            int32_t min_y = std::numeric_limits<int32_t>::max();
            int32_t max_x = 0;
            int32_t max_y = 0;
            bool any = false;

            for (uint32_t s = 0; s < fd.shadow_slice_count; ++s)
            {
                if (fd.slice_to_group_map[s] != g)
                    continue;

                VkRect2D tile_sc = makeSliceTileScissor(slices[s], atlas_resolution);
                const int32_t tx0 = tile_sc.offset.x;
                const int32_t ty0 = tile_sc.offset.y;
                const int32_t tx1 = tx0 + static_cast<int32_t>(tile_sc.extent.width);
                const int32_t ty1 = ty0 + static_cast<int32_t>(tile_sc.extent.height);

                min_x = std::min(min_x, tx0);
                min_y = std::min(min_y, ty0);
                max_x = std::max(max_x, tx1);
                max_y = std::max(max_y, ty1);
                any = true;
            }

            if (any)
            {
                fd.bias_groups[g].scissor.offset = {min_x, min_y};
                fd.bias_groups[g].scissor.extent = {
                    static_cast<uint32_t>(max_x - min_x),
                    static_cast<uint32_t>(max_y - min_y)};
            }
        }

        // Build frustum planes payload for vkCmdUpdateBuffer.
        const size_t planes_bytes = static_cast<size_t>(fd.shadow_slice_count) * 6u * sizeof(Frustum::Plane);
        fd.frustum_planes_payload.resize(planes_bytes);
        auto* plane_dst = reinterpret_cast<Frustum::Plane*>(fd.frustum_planes_payload.data());
        for (uint32_t s = 0; s < fd.shadow_slice_count; ++s)
        {
            Frustum frustum = Frustum::fromViewProj(slices[s].light_vp);
            std::memcpy(plane_dst + static_cast<size_t>(s) * 6u, frustum.planes.data(), 6 * sizeof(Frustum::Plane));
        }

        // Build the metadata tail: group map, exact origin page and exact
        // origin-local/page-size, each one vec4 per slice.
        {
            struct alignas(16) GroupVec4
            {
                float x;
                float y;
                float z;
                float w;
            };
            static_assert(sizeof(GroupVec4) == 16);
            const size_t group_bytes = static_cast<size_t>(fd.shadow_slice_count) * 3u * sizeof(GroupVec4);
            fd.group_map_payload.resize(group_bytes);
            auto* dst = reinterpret_cast<GroupVec4*>(fd.group_map_payload.data());
            for (uint32_t s = 0; s < fd.shadow_slice_count; ++s)
            {
                float as_float;
                uint32_t group_val = fd.slice_to_group_map[s];
                std::memcpy(&as_float, &group_val, sizeof(float));
                dst[s] = {as_float, 0.0f, 0.0f, 0.0f};

                auto* origin_page = reinterpret_cast<std::int32_t*>(&dst[fd.shadow_slice_count + s]);
                std::memcpy(origin_page, slices[s].origin_page, sizeof(slices[s].origin_page));
                dst[fd.shadow_slice_count * 2u + s] = {
                    slices[s].origin_local_page_size[0],
                    slices[s].origin_local_page_size[1],
                    slices[s].origin_local_page_size[2],
                    slices[s].origin_local_page_size[3]};
            }
        }
    }

    // =========================================================================
    //  Render graph passes
    // =========================================================================

    void MeshShadowFeature::addPasses(RGBuilder& builder)
    {
        // Resolve shadow resources from registry
        auto* shadow_res = renderScene().resources().find<ShadowResources>();
        if (!shadow_res || !shadow_res->isInitialized())
            return;

        // Current shadow technique (published by ShadowMapFeature) drives the
        // caster pipeline, the draw's color target and the post passes — fully
        // polymorphic, so adding a technique (VSM/MSM/...) never touches this
        // feature: it only implements the IShadowTechnique caster*/recordPostFrame
        // hooks. Replaces the old activeTechnique()==EVSM branch + dynamic_cast scan.
        IShadowTechnique* tech = shadow_res->currentTechnique();
        if (!tech)
            return;
        ensureCasterPipeline(*tech);
        const uint32_t tech_idx = static_cast<uint32_t>(tech->id());
        if (tech_idx >= caster_pipeline_ready_.size() || !caster_pipeline_ready_[tech_idx])
            return;

        const uint32_t safe_mdc = std::max(shadow_mdc_count_, 1u);
        const uint32_t safe_visible = std::max(shadow_total_visible_capacity_, 1u);

        // Indirect / count / visible buffers are RG-transient — created here each
        // frame, sized from the live shadow MDC count (ForwardMeshFeature pattern).
        // No fixed-capacity feature-owned buffers → no GPU-side OOB on big scenes.
        {
            RGBufferDescription desc{};
            desc.size = static_cast<VkDeviceSize>(safe_mdc) * sizeof(VkDrawIndexedIndirectCommand);
            desc.stride = sizeof(VkDrawIndexedIndirectCommand);
            desc.element_count = safe_mdc;
            desc.usage = ERGBufferUsageBits::STORAGE | ERGBufferUsageBits::INDIRECT;
            desc.usage |= ERGBufferUsageBits::TRANSFER_DST;
            desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
            shadow_indirect_rg_ = builder.createBuffer("MeshShadowIndirect", desc);
        }
        {
            RGBufferDescription desc{};
            desc.size = static_cast<VkDeviceSize>(safe_mdc) * sizeof(uint32_t);
            desc.stride = sizeof(uint32_t);
            desc.element_count = safe_mdc;
            desc.usage = ERGBufferUsageBits::STORAGE | ERGBufferUsageBits::INDIRECT;
            desc.usage |= ERGBufferUsageBits::TRANSFER_DST;
            desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
            shadow_count_rg_ = builder.createBuffer("MeshShadowCount", desc);
        }
        {
            RGBufferDescription desc{};
            desc.size = static_cast<VkDeviceSize>(safe_visible) * sizeof(GpuVisibleInstance);
            desc.stride = sizeof(GpuVisibleInstance);
            desc.element_count = safe_visible;
            desc.usage = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::STORAGE);
            desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
            shadow_visible_rg_ = builder.createBuffer("MeshShadowVisible", desc);
        }

        // Import the stable cull inputs (frustum UBO + instance buffers) so the
        // per-frame transient cull DS can reference them by handle (bindings
        // 0/1/4/5). Sync for the frustum UBO is still the .after(kShadowViewUploadPassName)
        // execution dep, unchanged from the prior immutable-set path.
        RGResourceHandle cull_ubo_rg{};
        {
            RGBufferDescription desc{};
            desc.size = shadow_cull_ssbo_size_;
            desc.stride = shadow_cull_ssbo_size_;
            desc.element_count = 1;
            desc.usage = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::STORAGE);
            desc.memory_usage = ERGMemoryUsage::GPU_ONLY;
            RGImportedBufferInfo imp{};
            imp.buffer_getter = [this](VkBuffer* out, uint32_t cap) -> uint32_t {
                if (out == nullptr || cap == 0)
                    return 0u;
                // Current FIF slot — refreshed each frame by the recorder, same slot
                // the kUploadFrustums write targets (shadow_frame_data_). (P1#24)
                out[0] = shadow_cull_ubo_[mdc_info_slot_];
                return 1u;
            };
            cull_ubo_rg = builder.importBuffer("MeshShadowCullUBO", desc, imp);
        }
        const auto cull_meta_rg = importInstanceStorageBuffer(
            builder,
            "MeshShadowCullMeta",
            instance_res_->fieldStorageImportBytes(sizeof(InstanceCullMeta)),
            sizeof(InstanceCullMeta),
            instance_res_->slotCount(),
            &InstanceResources::cullMetaBuffer
        );
        const auto section_rg = importInstanceStorageBuffer(
            builder,
            "MeshShadowSectionTable",
            static_cast<VkDeviceSize>(instance_res_->capacity()) * sizeof(MeshSectionRecord),
            sizeof(MeshSectionRecord),
            instance_res_->capacity(),
            &InstanceResources::meshSectionBuffer
        );
        const auto property_rg = importInstanceStorageBuffer(
            builder,
            "MeshShadowProperty",
            instance_res_->fieldStorageImportBytes(sizeof(InstanceProperty)),
            sizeof(InstanceProperty),
            instance_res_->slotCount(),
            &InstanceResources::propertyBuffer
        );
        const auto safe_alive_count = std::max(instance_res_->aliveCount(), 1u);
        const auto alive_slots_rg = importInstanceStorageBuffer(
            builder,
            "MeshShadowAliveSlots",
            static_cast<VkDeviceSize>(safe_alive_count) * sizeof(uint32_t),
            sizeof(uint32_t),
            safe_alive_count,
            &InstanceResources::aliveSlotBuffer
        );
        // MDC-info buffer: per-FIF CPU-mapped, imported at the current frame slot.
        // The recorder refreshes this getter each frame, so the rotating slot is
        // picked up without a graph rebuild.
        {
            const VkDeviceSize mdc_info_size =
                static_cast<VkDeviceSize>(shadow_mdc_gpu_data_.size()) * sizeof(uint32_t);
            RGBufferDescription desc{};
            desc.size = std::max(mdc_info_size, VkDeviceSize{4});
            desc.stride = sizeof(uint32_t);
            desc.element_count = std::max(static_cast<uint32_t>(shadow_mdc_gpu_data_.size()), 1u);
            desc.usage = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::STORAGE);
            desc.memory_usage = ERGMemoryUsage::CPU_TO_GPU;
            RGImportedBufferInfo imp{};
            imp.buffer_getter = [this](VkBuffer* out, uint32_t cap) -> uint32_t {
                if (out == nullptr || cap == 0)
                    return 0u;
                out[0] = shadow_mdc_info_buf_[mdc_info_slot_];
                return 1u;
            };
            shadow_mdc_info_rg_ = builder.importBuffer("MeshShadowMdcInfo", desc, imp);
        }

        // The world-partition active mask is no longer a descriptor binding: its GPU
        // address (or 0 when large-world is disabled) rides the cull push-constant and
        // is read via buffer_reference in mesh_cull_unified.comp. A dormant-cell
        // instance still reads 0 and stops casting shadows; the address is supplied
        // per-frame via MeshInstanceExtData (see GpuDrivenMeshFeatureBase).

        // Per-frame transient cull DS (set 0), mirroring ForwardMeshFeature's
        // FwdCullDS. Replaces the persistent immutable shadow_cull_set_ so the
        // RG-transient indirect/count/visible buffers can be bound by handle.
        std::vector<RGDescriptorWrite> cull_bindings = {
            {0, EDescriptorType::STORAGE_BUFFER, cull_ubo_rg},
            {1, EDescriptorType::STORAGE_BUFFER, cull_meta_rg},
            {2, EDescriptorType::STORAGE_BUFFER, shadow_count_rg_},
            {3, EDescriptorType::STORAGE_BUFFER, shadow_indirect_rg_},
            {4, EDescriptorType::STORAGE_BUFFER, section_rg},
            {5, EDescriptorType::STORAGE_BUFFER, property_rg},
            {6, EDescriptorType::STORAGE_BUFFER, shadow_visible_rg_},
            {7, EDescriptorType::STORAGE_BUFFER, shadow_mdc_info_rg_},
            {8, EDescriptorType::STORAGE_BUFFER, alive_slots_rg},
        };
        auto cull_tds = builder.createTransientDS("MeshShadowCullDS", cull_set_layout_, cull_bindings);

        const bool clear_counters_enabled = shadow_clear_pipeline_.valid();
        if (clear_counters_enabled)
        {
            auto shadow_clear_tds = builder.createTransientDS(
                "MeshShadowClearDS",
                shadow_clear_ds_layout_,
                {
                    {0, EDescriptorType::STORAGE_BUFFER, shadow_count_rg_},
                    {1, EDescriptorType::STORAGE_BUFFER, shadow_count_rg_},
                    {2, EDescriptorType::STORAGE_BUFFER, shadow_count_rg_},
                    {3, EDescriptorType::STORAGE_BUFFER, shadow_count_rg_},
                    {4, EDescriptorType::STORAGE_BUFFER, shadow_count_rg_},
                    {5, EDescriptorType::STORAGE_BUFFER, shadow_count_rg_},
                    {6, EDescriptorType::STORAGE_BUFFER, shadow_count_rg_},
                    {7, EDescriptorType::STORAGE_BUFFER, shadow_count_rg_},
                }
            );

            ClearCountersKernelConfig clear_cfg{};
            clear_cfg.buffers[0] = shadow_count_rg_;
            clear_cfg.buffer_count = 1u;

            builder.addPass("MeshShadowClearCounters", ERGPassType::COMPUTE)
                .setComputePipeline(shadow_clear_pipeline_)
                .bindTransientDS(0, shadow_clear_tds)
                .write(shadow_count_rg_, ERGBufferRole::STORAGE)
                .after(kShadowViewUploadPassName)
                .setKernel("ClearCounters", makeKernelConfig(clear_cfg));
        }

        // Shadow cull (compute) — per-MDC append via shadow MDC info buffer
        auto cull_pass = builder.addPass("MeshShadowCull", ERGPassType::COMPUTE)
                             .setComputePipeline(shadow_cull_pipeline_)
                             .bindTransientDS(0, cull_tds)
                             .write(shadow_indirect_rg_, ERGBufferRole::STORAGE)
                             .write(shadow_count_rg_, ERGBufferRole::STORAGE)
                             .write(shadow_visible_rg_, ERGBufferRole::STORAGE)
                             .read(cull_ubo_rg, ERGBufferRole::STORAGE)
                             .read(cull_meta_rg, ERGBufferRole::STORAGE)
                             .read(section_rg, ERGBufferRole::STORAGE)
                             .read(property_rg, ERGBufferRole::STORAGE)
                             .read(alive_slots_rg, ERGBufferRole::STORAGE)
                             .read(shadow_mdc_info_rg_, ERGBufferRole::STORAGE);
        if (clear_counters_enabled)
            cull_pass.after("MeshShadowClearCounters");
        else
        {
            cull_pass.after(kShadowViewUploadPassName);
        }

        MeshCullKernelConfig cull_cfg{
            .frustum_ubo_rg = {},
            .draw_count_rg = shadow_count_rg_,
            .indirect_rg = shadow_indirect_rg_,
            .pass_mask = static_cast<uint32_t>(passMaskForPhase(ECoreRenderPhase::Shadow)),
            .geometry_mask = supportedGeometryMask(),
            .max_slices = static_cast<uint32_t>(max_shadow_slices_),
            .draw_list_count = shadow_mdc_count_,
            .descriptor_layout_version = cfg_.descriptor_layout_version,
            .extension_flags = cfg_.extension_flags.bits(), // GPU push constant: raw word
            .mdc_count = shadow_mdc_count_,
            .view_mdc_count = view_mdc_count_,
        };

        cull_pass.setKernel("ShadowCull", makeKernelConfig(cull_cfg));

        // Shadow compact (replaces finalize) — per-MDC compact pass
        const uint32_t shadow_mdc_for_compact = shadow_mdc_count_;
        builder.addPass("MeshShadowCompact", ERGPassType::COMPUTE)
            .setComputePipeline(compact_pipeline_)
            .bindTransientDS(0, cull_tds)
            .read(shadow_visible_rg_, ERGBufferRole::STORAGE)
            .readWrite(shadow_indirect_rg_, ERGBufferRole::STORAGE)
            .readWrite(shadow_count_rg_, ERGBufferRole::STORAGE)
            .read(shadow_mdc_info_rg_, ERGBufferRole::STORAGE)
            .after("MeshShadowCull")
            .setKernelFn([shadow_mdc_for_compact](const PassRecordContext& pctx) {
                if (pctx.pipeline_layout == VK_NULL_HANDLE)
                    return;
                vkCmdPushConstants(
                    pctx.cmd,
                    pctx.pipeline_layout,
                    VK_SHADER_STAGE_COMPUTE_BIT,
                    0,
                    sizeof(uint32_t),
                    &shadow_mdc_for_compact
                );
                vkCmdDispatch(pctx.cmd, (shadow_mdc_for_compact + 63u) / 64u, 1u, 1u);
            }
            )
            .setKernel(
                "MdcCompact",
                makeKernelConfig(MdcCompactKernelConfig{
                    .draw_count_rg = shadow_count_rg_,
                    .indirect_rg = shadow_indirect_rg_,
                    .mdc_count = shadow_mdc_count_,
                })
            );

        auto shadow_visible_tds = builder.createTransientDS(
            "MeshShadowVisibleDS",
            shadow_visible_set_layout_,
            {
                {0, EDescriptorType::STORAGE_BUFFER, shadow_visible_rg_},
            }
        );

        // Shadow draw (graphics) — per-MDC draws per bias group
        const uint32_t atlas_res = shadow_res->atlasResolution();

        // Single caster pipeline = the current technique's (ensured above).
        // Set 3 = bindless vertex pool is mandatory.
        auto* vpr = renderScene().resources().find<VertexPoolRegistry>();

        // The technique declares whether the caster writes a color target in
        // addition to depth: PCF = depth-only (null), EVSM = the RGBA16F moment
        // atlas. Ordering contract: a requested color target must already be
        // imported into the graph by ShadowMapFeature::addPasses, which runs before
        // this feature (registration order: ShadowMap before MeshShadow). If that
        // order ever breaks, referenceTexture would mint a default forward-ref
        // placeholder and write() would freeze layer_count=1 — but the moment atlas
        // is a 2D array, so the color attachment would cover only layer 0. Same
        // implicit ordering dependency the legacy EVSM draw already had.
        auto shadow_draw = builder.addPass(kMeshShadowDrawPassName, ERGPassType::GRAPHICS);
        if (const char* color_target = tech->casterColorTarget())
            shadow_draw.write(builder.referenceTexture(color_target), lux::render::ETextureRole::COLOR_ATTACHMENT);
        shadow_draw
            .write(builder.referenceTexture(cfg_.shadow_atlas), lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
            .setPipeline(caster_pipelines_[tech_idx]);

        // The caster's set 0 is intentionally feature-private. It contains
        // uCasterShadowSlices and uCasterConfig from ShadowResources' compact
        // descriptor set; it is not the Light engine set. Domain merging only
        // moves Instance/VertexPool into FEATURE set 2 and the private visible
        // set from canonical slot 2 to slot 3. Treating set 0 as Light made the
        // graph compiler skip it because the reflected slot is private, leaving
        // vkCmdDrawIndexedIndirectCount with an unbound set 0.
        shadow_draw.bindResourceDS(
            0,
            shadow_res,
            &ShadowResources::resolveDS,
            EDSBindMode::PER_FIF,
            builder.trackExternalBuffer("ext.ShadowCascade"),
            ERGResourceType::BUFFER
        );

        shadow_draw.useEngineSet(EDescriptorSetSlot::Instance)
            .bindTransientDS(2, shadow_visible_tds)
            .read(shadow_indirect_rg_, ERGBufferRole::INDIRECT)
            .read(shadow_count_rg_, ERGBufferRole::INDIRECT)
            .read(shadow_visible_rg_, ERGBufferRole::STORAGE)
            .after("MeshShadowCompact")
            // Shadow-atlas rendering is logically frame-early and has no data
            // edge to the G-buffer. Pinning it BEFORE the G-buffer draw keeps
            // the GBuffer→Lighting pair adjacent for the local-read merged
            // scope (lighting orders itself .after this pass).
            // Unknown name (forward-only scenes) is ignored by design.
            .before(kDeferredGBufferDrawPassName)
            .setPhaseMask(1ULL << static_cast<uint8_t>(ECoreRenderPhase::Shadow))
            .setManualViewport(true);

        if (vpr != nullptr)
        {
            // 顶点池按逻辑身份声明,收进 FEATURE 域槽(与 Instance 去重到同一
            // 个槽 —— 一次域绑定覆盖两者)。
            shadow_draw.useEngineSet(EDescriptorSetSlot::VertexPool);
            // Order shadow draw after every compute-vertex producer (skinning, ...).
            if (auto* vproducers = renderScene().resources().find<VertexProductionRegistry>())
                for (const auto& prod : vproducers->producers())
                    shadow_draw.read(builder.referenceBuffer(prod.rg_buffer_name), ERGBufferRole::STORAGE);
        }
        // (无 vertex pool 时原先调 forcePassPipeline() 作降级 —— 但该标志位从来没人
        //  读,这个降级实际未生效,注释自己也写着 "will fail to render correctly"。
        //  标志位已作废删除,此处不再有分支;真要处理无 vertex pool 的情形,应当
        //  在特性侧显式换管线,而不是设一个没人读的标志。)

        const auto index_buffers = importSharedIndexBuffers(builder);
        shadow_draw.setKernel(
            "ShadowDraw",
            makeKernelConfig(ShadowDrawKernelConfig{
                .draw_count_rg = shadow_count_rg_,
                .indirect_rg = shadow_indirect_rg_,
                .index_buffers_rg = index_buffers.data(),
                .index_buffer_count = static_cast<std::uint32_t>(index_buffers.size()),
                .geometry_mask = supportedGeometryMask(),
                .atlas_resolution = atlas_res,
                .view_mdc_count = view_mdc_count_,
                .bias_group_count = shadow_frame_data_.bias_group_count,
                .mdc_entries = instance_res_->mdcTable().entries().data(),
                // family_count = 0 skips skinned `+N` offset.
                .family_count = 0u,
            })
        );

        // ── Technique post passes (e.g. EVSM separable blur). PCF is a no-op
        //    (base recordPostFrame). Fully polymorphic — MeshShadowFeature no
        //    longer knows EVSM exists; the EVSM blur logic now lives in
        //    EVSMShadowTechnique::recordPostFrame.
        ShadowFrameContext fctx{};
        fctx.builder = &builder;
        fctx.shadow_res = shadow_res;
        fctx.scene = &renderScene();
        fctx.atlas_resolution = atlas_res;
        tech->recordPostFrame(fctx);
    }

    // =========================================================================
    //  Shadow caster pipeline (lazy, one cached per technique)
    // =========================================================================
    //  Unifies the former depth-only (PCF) and EVSM-moment caster pipelines. The
    //  technique DECLARES its caster shape (vert/frag variants + color write mask
    //  + color target); this assembles the pipeline from those hooks. The 4-set
    //  layout, vertex-pool spec constants and rasterizer state are shared. Adding
    //  a technique = implement the caster* hooks — this function stays untouched.

    void MeshShadowFeature::ensureCasterPipeline(IShadowTechnique& tech)
    {
        const auto idx = static_cast<uint32_t>(tech.id());
        if (idx >= caster_pipelines_.size())
            return;
        if (caster_pipeline_ready_[idx])
            return;

        auto& ctx = renderContext();
        auto* shadow_res = renderScene().resources().find<ShadowResources>();
        VkDescriptorSetLayout shadow_set0 =
            (shadow_res && shadow_res->isInitialized()) ? shadow_res->descriptorSetLayout() : VK_NULL_HANDLE;
        if (shadow_set0 == VK_NULL_HANDLE)
            return;

        auto& shaders = ctx.globalRegistry().must<ShaderResources>();

        // Caster vert+frag come from the technique. PCF = thin shadow_depth.vert +
        // depth-only frag; EVSM = the FAT mesh_shadow.vert (so its moment frag's
        // loc 1/2/3 inputs — vShadowNear/vShadowFar/vDepthPersp — are fed) +
        // shadow_evsm_caster.frag. Resolved from the builtin registry.
        ShaderHandle vh{};
        ShaderHandle fh{};
        const std::array caster_slots{
            ShaderStageSlot{tech.casterVertVariant(), &vh},
            ShaderStageSlot{tech.casterFragVariant(), &fh}};
        if (!resolveShaderStages(shaders, caster_slots))
            return; // 与本函数其余早退口径一致:未就绪即不注册,下次再试

        // 域合并切换:slices(uShadowSlices,原紧凑 set0)连同 transforms/vertex pool
        // 一起落进 FEATURE 域槽 2(数据正确性由 Shadow/Instance/VertexPool 对域集的写
        // 保证);visible(set2,与域槽相撞)经私有重定位表挪到槽 3 —— 管线最终恰好
        // 4 个 set。
        //
        // 紧凑布局老路已退休:它绑的是 Instance/VertexPool 的 per-set 实例,而那些集在
        // 双写拆除后不再有人写,降级过去只会绑到一组空描述符、阴影静默消失。所以任一
        // stage 切换失败即不注册本 technique —— 与 PipelineManager 对未合并引擎资源管线
        // 直接拒绝注册的口径一致。
        const std::array caster_stages{vh, fh};
        auto switched = shaders.preparePipelineStages(caster_stages);
        if (!switched)
            return;

        vh = switched->handle(0);
        fh = switched->handle(1);

        // 域模式下不再手工组装布局:交给反射注册 + 域路由。
        //(此前这里手写一份紧凑布局 —— [0]=shadow slices、[1]=instance、
        // [2]=visible、[3]=bindless vertex pool,槽号与 canonical 不同。它随
        // 紧凑老路一起退休了,见上面 fail-fast 处的说明。)
        auto tmpl = makeOpaqueMeshTemplate();
        tmpl.debug_name = "MeshShadowCaster";
        tmpl.descriptor_set_count = 4u;
        // Set 0 is allocated from ShadowResources' shared three-binding
        // layout (slice SSBO, atlas sampler, config UBO). The caster shaders
        // only reference bindings 0 and 2, so reflection alone would build a
        // two-binding subset layout that is incompatible with the actual set
        // instance (VUID-00358). Declare the feature-owned authoritative
        // layout explicitly; this is exactly the shared-subset case covered
        // by GraphicsPipelineTemplate::explicit_set_layouts.
        tmpl.explicit_set_layouts.emplace_back(0u, shadow_set0);
        tmpl.vertex_shader = switched->module(0);
        tmpl.fragment_shader = switched->module(1);
        tmpl.vertex_bindings.clear();
        tmpl.vertex_attributes.clear();
        // Technique-declared color output: 0 = depth-only (PCF), 0xF = RGBA16F
        // moments written into the moment-atlas color target (EVSM).
        tmpl.color_write_mask = tech.casterColorWriteMask();
        tmpl.blend_enable = VK_FALSE;
        // Conventional back-face culling — records the occluder's light-facing
        // surface in the shadow map, the correct depth for both floating-receiver
        // and receiver-intersects-occluder cases. Self-shadow acne is handled by
        // the slope-scale + constant rasterizer depth bias (dynamic state via
        // `vkCmdSetDepthBias` per bias group) plus the receiver-plane bias in
        // `shadow_pcf.glsl / shadow_evsm.glsl`. Front-face culling — a tempting "free bias" —
        // fails the moment the receiver crosses into the occluder's volume.
        tmpl.cull_mode = VK_CULL_MODE_BACK_BIT;
        // Depth bias enabled (zero constant/slope) for ALL techniques: the shared
        // ShadowDraw kernel issues `vkCmdSetDepthBias` per bias group, so a
        // pipeline that didn't opt into bias would trip VUID-...-08608. EVSM
        // doesn't need bias (its dual-exponential warp absorbs self-shadowing) but
        // must still opt in; 0*slope + 0*constant is a semantic no-op that keeps
        // the dynamic state valid.
        tmpl.depth_bias_enable = VK_TRUE;
        tmpl.depth_bias_constant = 0.0f;
        tmpl.depth_bias_slope = 0.0f;
        tmpl.pipeline_layout = VK_NULL_HANDLE; // 空 → 反射注册 + 域路由

        // Feed the pool layout (full 22-float layout 0) as spec constants. The
        // skinned pool stride is the same, so one spec serves all draws.
        if (auto& vlr = ctx.globalRegistry().must<VertexLayoutRegistry>(); vlr.hasLayout(kDefaultVertexLayoutId))
            appendVertexLayoutSpecs(
                tmpl.specialization_values,
                vlr.fetchLayout(kDefaultVertexLayoutId),
                VK_SHADER_STAGE_VERTEX_BIT,
                kVtxSpecInputBase
            );

        const std::array<const lux::rdesc::ShaderInfo*, 2> infos{&switched->info(0), &switched->info(1)};
        auto pipe = ctx.pipelineManager().registerGraphicsTemplate(tmpl, infos);
        if (!pipe.has_value())
            return;
        caster_pipelines_[idx] = pipe.value();
        caster_pipeline_ready_[idx] = true;
    }

    // =========================================================================
    //  Cull descriptor resources
    // =========================================================================

    void MeshShadowFeature::createCullResources()
    {
        // Reuse the shared 9-binding cull layout used by other mesh features.
        // The cull/compact passes bind a per-frame transient DS built from this
        // layout (see addPasses) — no persistent descriptor set is allocated.
        createCullLayout();

        // Shadow frustum SSBO (updated per-view via vkCmdUpdateBuffer in MeshShadowCull)
        {
            shadow_cull_ssbo_size_ = shadowCullSsboSize(max_shadow_slices_);

            VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            ci.size = shadow_cull_ssbo_size_;
            ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

            for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) // per-FIF (P1#24)
                vmaCreateBuffer(vma_, &ci, &aci, &shadow_cull_ubo_[i], &shadow_cull_ubo_alloc_[i], nullptr);
        }
    }

} // namespace lux::render
