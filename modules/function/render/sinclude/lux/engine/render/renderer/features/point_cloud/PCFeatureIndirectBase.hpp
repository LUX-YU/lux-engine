#pragma once
/**
 * @file PCFeatureIndirectBase.hpp
 * @brief Shared GPU infrastructure for all compute-cull + indirect-draw point
 *        cloud features (GPUDriven, LOD, Splatting).
 *
 * This base class encapsulates:
 *   - Custom 5-binding descriptor set layout for pointcloud_culling.comp
 *   - GPU buffers: CullingParams UBO, FrustumPlanes UBO, Instance SSBO,
 *     Indirect draw SSBO
 *   - COMPUTE pass registration (reset atomics + dispatch culling shader)
 *   - Per-frame camera / frustum UBO writes
 *
 * Subclasses provide:
 *   - mode()  / name()  / priority()  (pure virtuals via IPointCloudFeature)
 *   - draw_handle_ filled in the subclass constructor
 *   - addPasses()  — calls addComputePass() then appends their own draw pass
 *
 * @see PCFeatureGPUDriven  (Mode 2)
 * @see PCFeatureLOD        (Mode 3)
 * @see PCFeatureSplatting  (Mode 4)
 */

#include <lux/engine/render/renderer/features/point_cloud/IPointCloudFeature.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PointCloudGpuData.hpp>
#include <lux/engine/render/renderer/RenderObjectExtraction.hpp>
#include <lux/engine/render/renderer/FrustumCuller.hpp>
#include <lux/engine/render/core/ShaderObject.hpp>
#include <lux/engine/render/core/RenderTypes.hpp>
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/render/resources/descriptor/DescriptorService.hpp>
#include <lux/engine/render/graph/RGPassTypes.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/scene/ViewStateTable.hpp>
#include <lux/engine/function/visibility.h>

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T*;
struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;

namespace lux::render
{
    class PointCloudGlobalBuffer;
    class GpuOctreeNodeBuffer;

    // =========================================================================
    //  PCFeatureIndirectBase
    // =========================================================================

    /**
     * @brief Abstract base for GPU-driven indirect-draw point cloud features.
     *
     * Subclass constructors must:
     *   1. Call one of the protected base-init helpers to allocate GPU resources.
     *   2. Create the graphics pipeline (draw_handle_) for their specific mode.
 *   3. Implement mode() / name() / priority() / addPasses().
 */
    class LUX_FUNCTION_PUBLIC PCFeatureIndirectBase : public IPointCloudFeature
    {
    public:
        static constexpr phase_mask_t kExtractPhaseMask =
            phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::PointCloud));

        ~PCFeatureIndirectBase() override;

        // --- RenderFeature per-frame ---
        void onFrameBegin(const FeatureFrameContext& ctx) override;
        void extractRenderObjects(
            const RenderObjectExtractContext& ctx,
            std::vector<DrawPacket>& out_packets);

    protected:
        // -------------------------------------------------------------------
        //  Protected constructor — called by subclasses
        // -------------------------------------------------------------------

        /**
         * @param ctx          Render context.
         * @param compute_sh   pointcloud_culling.comp SPIR-V + reflection info.
         * @param max_nodes    Maximum number of nodes the compute shader may emit
         *                     per frame (sizes Instance + Indirect SSBOs).
         * @param feature_cfg  Forwarded to RenderFeature (name etc.).
         * @param pass_label   Short string appended to RG pass names to avoid
         *                     name collisions if two features overlap briefly
         *                     (e.g. "GPUDriven", "LOD", "Splat").
         */
        explicit PCFeatureIndirectBase(
            ShaderHandle         compute_shader_id,
            uint32_t             max_nodes,
            RenderFeature::Config feature_cfg,
            std::string_view     pass_label);

        lux::render::Expected<void> initAndAttachTo(RenderScene& scene) override;

        // -------------------------------------------------------------------
        //  addComputePass — call from subclass addPasses()
        //
        //  Imports the Instance and Indirect SSBOs into the RG and registers
        //  the COMPUTE cull pass.  Returns handles for the draw pass to use:
        //    .read(out_indirect_rg, ERGBufferRole::INDIRECT)
        // -------------------------------------------------------------------
        void addComputePass(RGBuilder& builder, RGResourceHandle& out_inst_rg, RGResourceHandle& out_indirect_rg);

        // -------------------------------------------------------------------
        //  addIndirectDrawPass — call from subclass addPasses() after
        //  addComputePass(). Registers the shared GRAPHICS indirect-draw pass
        //  (color/depth targets, scene DS, vertex-buffer bind, drawIndirectCount);
        //  the @p push_constant_fn closure records only the mode-specific push
        //  constant (offset 8, VERTEX stage). Shared by GPUDriven/LOD/Splatting.
        // -------------------------------------------------------------------
        void addIndirectDrawPass(
            RGBuilder&                                    builder,
            std::string_view                              pass_name,
            std::string_view                              color_target,
            std::string_view                              depth_target,
            RGResourceHandle                              indirect_rg,
            std::function<void(const PassRecordContext&)> push_constant_fn);

        // -------------------------------------------------------------------
        //  Protected members — accessible in subclass addPasses() recorders
        // -------------------------------------------------------------------

        std::string                 pass_label_;
        ShaderHandle                    compute_shader_id_{};

        VkDevice                    vk_device_{VK_NULL_HANDLE};
        VmaAllocator                allocator_{};
        uint32_t                    max_nodes_{};
        uint32_t                    current_frame_{};

        ComputePipelineHandle       compute_handle_{};
        GraphicsPipelineHandle      draw_handle_{};   ///< Filled by subclass constructor

        // Compute descriptor layout (transient DS created per-frame in addComputePass)
        DescriptorLayoutId          cull_layout_id_{kInvalidDescriptorLayoutId};
        VkDescriptorSetLayout       cull_layout_{VK_NULL_HANDLE};

        uint32_t                    fif_{1}; ///< actual frames-in-flight (set in initAndAttachTo)

        // Non-owning — points into PointCloudResources (outlives this feature).
        PointCloudGlobalBuffer*     global_buf_{nullptr};
        GpuOctreeNodeBuffer*        node_buf_{nullptr};

    private:
        void createDescriptorLayout(VkDevice device);
    };

} // namespace lux::render

