/**
 * @file PCFeatureIndirectBase.cpp
 * @brief Shared GPU infrastructure for GPU-Driven, LOD, and Splatting features.
 */

#include <lux/engine/render/renderer/features/point_cloud/PCFeatureIndirectBase.hpp>
#include <lux/engine/render/renderer/features/BufferTransferSynchronization.hpp>
#include <lux/engine/render/core/FrustumCuller.hpp>
#include <lux/engine/render/core/ViewFrameData.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraResource.hpp>
#include <lux/engine/render/resources/point_cloud/PointCloudGlobalBuffer.hpp>
#include <lux/engine/render/resources/point_cloud/GpuOctreeNodeBuffer.hpp>
#include <lux/engine/render/resources/PointCloudResources.hpp>
#include <lux/engine/render/gpu/transfer/TransferContributor.hpp>   // makeTransferContributor
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>

#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>

#include <vk_mem_alloc.h>

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <string>

namespace lux::render
{

// ============================================================================
//  Constructor / Destructor
// ============================================================================

PCFeatureIndirectBase::PCFeatureIndirectBase(
    ShaderHandle          compute_shader_id,
    uint32_t              max_nodes,
    RenderFeature::Config feature_cfg,
    std::string_view      pass_label)
    : IPointCloudFeature(feature_cfg)
    , pass_label_(pass_label)
    , compute_shader_id_(compute_shader_id)
    , max_nodes_(max_nodes)
{
}

lux::render::Expected<void> PCFeatureIndirectBase::initAndAttachTo(RenderScene& /*scene*/){
    auto& ctx = renderContext();
    vk_device_ = ctx.device();
    allocator_ = ctx.vmaAllocator();
    auto& shaders = ctx.globalRegistry().must<ShaderResources>();
    auto resolved_cs = resolveShaderStage(shaders, compute_shader_id_,
                                         EBuiltinShader::PC_CULLING_COMP);
    if (!resolved_cs)
        return lux::cxx::unexpected(resolved_cs.error());
    compute_shader_id_ = *resolved_cs;
    const ShaderObject* cs = shaders.get(compute_shader_id_);
    if (cs == nullptr)
        return renderFailure<err::shader::HandleStale>();

    // ---- 1. Custom 5-binding descriptor set layout (compute stage) ----
    createDescriptorLayout(vk_device_);

    // ---- 2. Compute pipeline layout (1 set, no push constants needed here) ----
    const std::array<VkDescriptorSetLayout, 1> set_layouts{cull_layout_};
    const PipelineLayoutDesc cull_layout_desc{
        .set_layouts    = set_layouts,
        .push_constants = std::span<const VkPushConstantRange>{},
        .debug_name     = "PointCloudCullLayout"};
    auto compute_layout = ctx.pipelineLayoutService().getOrCreate(cull_layout_desc);
    if (!compute_layout)
        return lux::cxx::unexpected(compute_layout.error());

    // PipelineManager takes ownership of compute_layout.
    compute_handle_ = ctx.pipelineManager().registerComputePipeline(
        cs->module, *compute_layout);

    // ---- 3. Frames in flight ----
    fif_ = ctx.framesInFlight();

    // ---- 4. Shared GPU resources (lazy-init PointCloudResources) ----
    //
    // 这里原本是「先 find,没有就 emplace 再 find 再 init,设完三个 setter 之后
    // 若仍未初始化就用**完全相同的参数**再 init 一次」。两次 init 里第一次成功则
    // 第二次被跳过、第一次失败则第二次必然同样失败 —— 结构上二选一都是空转,
    // 而中间三个 setter 已经作用在可能未初始化的对象上了。收敛成一次
    // ensure<T>(init_args):构造 + init + 只在成功时发布(与 PCFeatureSimple 同形)。
    // 4M 点位的 VMA 缓冲是真会分配不出来的量级 —— 失败时注册表里什么都没有,
    // 而不是留下一个已发布的空资源让 attach 照常成功、到绘制期才暴露。
    auto pc_r = renderScene().sceneRegistry().ensure<PointCloudResources>(
        ctx.vmaAllocator(), kMaxGlobalPoints, max_nodes_);
    if (!pc_r)
        return lux::cxx::unexpected<RenderError>(pc_r.error());
    auto* pc_res = *pc_r;
    pc_res->setDeferredQueue(&ctx.deferredDestroyQueue());
    pc_res->setRetireScheduler(&ctx.retireScheduler());
    pc_res->setRetireOwnerToken(renderScene().retireOwnerToken());
    // Register PointCloudResources as a transfer contributor once (idempotent:
    // multiple PC features in one scene share it; only the first attach adds it).
    // The scene core used to do this lazily in recordUploads — the OWNER does it
    // now, keeping the core scene domain-free.
    if (!pc_res->usesTransferScheduler()) {
        renderScene().transferScheduler().contributors().add(
            makeTransferContributor(pc_res, /*priority=*/1));
        pc_res->setUseTransferScheduler(true);
    }
    global_buf_ = &pc_res->globalBuffer();
    node_buf_   = &pc_res->nodeBuffer();
    return {};
    }

PCFeatureIndirectBase::~PCFeatureIndirectBase()
{
    // cull_layout_ is managed by DescriptorService, no manual cleanup needed.
    // cull_layout_ / cull_layout_id_ are destroyed with the descriptor service.
    vk_device_ = VK_NULL_HANDLE;
}

// ============================================================================
//  RenderFeature per-frame callbacks
// ============================================================================

void PCFeatureIndirectBase::onFrameBegin(const FeatureFrameContext& ctx)
{
    current_frame_ = ctx.frame_index;
}


// ============================================================================
//  addComputePass — called from subclass addPasses()
// ============================================================================
void PCFeatureIndirectBase::addComputePass(
    RGBuilder&        builder,
    RGResourceHandle& out_inst_rg,
    RGResourceHandle& out_indirect_rg)
{
    const VkDeviceSize indirect_size =
        static_cast<VkDeviceSize>(16u + max_nodes_ * sizeof(IndirectDrawCommand));
    const VkDeviceSize instance_size =
        static_cast<VkDeviceSize>(16u + max_nodes_ * sizeof(PointCloudInstance));

    // ---- RG-managed compute SSBOs ----
    {
        RGBufferDescription inst_desc{};
        inst_desc.size          = instance_size;
        inst_desc.stride        = sizeof(PointCloudInstance);
        inst_desc.element_count = max_nodes_;
        inst_desc.usage         = ERGBufferUsageBits::STORAGE | ERGBufferUsageBits::TRANSFER_DST;
        inst_desc.memory_usage  = ERGMemoryUsage::GPU_ONLY;
        out_inst_rg = builder.createBuffer("PC" + pass_label_ + "Instances", inst_desc);
    }
    {
        RGBufferDescription indirect_desc{};
        indirect_desc.size          = indirect_size;
        indirect_desc.stride        = sizeof(IndirectDrawCommand);
        indirect_desc.element_count = max_nodes_;
        indirect_desc.usage         = ERGBufferUsageBits::STORAGE | ERGBufferUsageBits::INDIRECT;
        indirect_desc.usage        |= ERGBufferUsageBits::TRANSFER_DST;
        indirect_desc.memory_usage  = ERGMemoryUsage::GPU_ONLY;
        out_indirect_rg = builder.createBuffer("PC" + pass_label_ + "Indirect", indirect_desc);
    }

    // ---- RG-managed UBOs (written via vkCmdUpdateBuffer per-view) ----
    RGResourceHandle params_rg{};
    {
        RGBufferDescription desc{};
        desc.size          = sizeof(CullingParams);
        desc.stride        = sizeof(CullingParams);
        desc.element_count = 1;
        desc.usage         = ERGBufferUsageBits::UNIFORM | ERGBufferUsageBits::TRANSFER_DST;
        desc.memory_usage  = ERGMemoryUsage::GPU_ONLY;
        params_rg = builder.createBuffer("PC" + pass_label_ + "CullParams", desc);
    }

    RGResourceHandle frustum_rg{};
    {
        RGBufferDescription desc{};
        desc.size          = sizeof(FrustumPlanes);
        desc.stride        = sizeof(FrustumPlanes);
        desc.element_count = 1;
        desc.usage         = ERGBufferUsageBits::UNIFORM | ERGBufferUsageBits::TRANSFER_DST;
        desc.memory_usage  = ERGMemoryUsage::GPU_ONLY;
        frustum_rg = builder.createBuffer("PC" + pass_label_ + "FrustumPlanes", desc);
    }

    // ---- Import node buffer (shared external resource) ----
    RGResourceHandle node_rg{};
    {
        RGBufferDescription desc{};
        desc.size          = node_buf_
            ? (GpuOctreeNodeBuffer::kHeaderSize + node_buf_->maxNodes() * sizeof(GpuOctreeNode))
            : 1;
        desc.stride        = 1;
        desc.element_count = 1;
        desc.usage         = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::STORAGE);
        desc.memory_usage  = ERGMemoryUsage::GPU_ONLY;
        RGImportedBufferInfo imp{};
        imp.buffer_getter = [this](VkBuffer* out_buffers, uint32_t capacity) -> uint32_t {
            if (out_buffers == nullptr || capacity == 0)
                return 0u;
            out_buffers[0] = node_buf_ ? node_buf_->buffer() : VK_NULL_HANDLE;
            return 1u;
        };
        node_rg = builder.importBuffer("PC" + pass_label_ + "Nodes", desc, imp);
    }

    // ---- Transient DS (per-view, per-frame) ----
    auto cull_tds = builder.createTransientDS("PC" + pass_label_ + "CullDS", cull_layout_, {
        {0, EDescriptorType::STORAGE_BUFFER,  node_rg},
        {1, EDescriptorType::STORAGE_BUFFER,  out_inst_rg},
        {2, EDescriptorType::STORAGE_BUFFER,  out_indirect_rg},
        {3, EDescriptorType::UNIFORM_BUFFER,  params_rg},
        {4, EDescriptorType::UNIFORM_BUFFER,  frustum_rg},
    });

    // ---- COMPUTE cull pass ----
    const std::string cull_pass_name = "PC" + pass_label_ + "Cull";

    builder.addPass(cull_pass_name, ERGPassType::COMPUTE)
        .setComputePipeline(compute_handle_)
        .bindTransientDS(0, cull_tds)
        .write(out_inst_rg,      ERGBufferRole::STORAGE)
        .write(out_indirect_rg,  ERGBufferRole::STORAGE)
        .write(params_rg,        ERGBufferRole::CONSTANT)
        .write(frustum_rg,       ERGBufferRole::CONSTANT)
        .read(node_rg,           ERGBufferRole::STORAGE)
        .setKernelFn([this, params_rg, frustum_rg, out_inst_rg, out_indirect_rg]
                     (const PassRecordContext& ctx)
        {
            if (!node_buf_ || node_buf_->nodeCount() == 0) return;

            const uint32_t view_handle = ctx.view ? ctx.view->handle.index : 0;
            auto* cam = resolveViewCameraOnce(cam_cache_, renderScene().sceneRegistry());

            // Upload CullingParams UBO
            {
                CullingParams cp{};
                cp.view_proj_matrix[0]  = 1.0f;
                cp.view_proj_matrix[5]  = 1.0f;
                cp.view_proj_matrix[10] = 1.0f;
                cp.view_proj_matrix[15] = 1.0f;
                cp.camera_direction[2]  = -1.0f;
                cp.max_instances         = max_nodes_;
                cp.lod_bias              = 1.0f;
                cp.pixel_error_threshold = 0.5f;
                cp.max_points_per_frame  = 10'000'000u;

                if (ctx.view)
                {
                    const ViewFrameData* cam_fd = cam ? cam->find(view_handle) : nullptr;
                    ViewFrameData vfd = cam_fd ? *cam_fd : ViewFrameData{};
                    const auto& frame = vfd;
                    std::memcpy(
                        cp.view_proj_matrix,
                        frame.camera_view.view_proj.data(),
                        sizeof(cp.view_proj_matrix));

                    cp.camera_position[0] = frame.camera_transform.position.x();
                    cp.camera_position[1] = frame.camera_transform.position.y();
                    cp.camera_position[2] = frame.camera_transform.position.z();

                    // inv_view is a 4x4 camera->world matrix.
                    // Its 3rd column (rows 0..2, col 2) is camera -forward axis.
                    Eigen::Vector3f cam_forward =
                        -frame.camera_view.inv_view.block<3, 1>(0, 2);
                    const float n2 = cam_forward.squaredNorm();
                    if (n2 > 1e-8f)
                        cam_forward /= std::sqrt(n2);
                    else
                        cam_forward = Eigen::Vector3f(0.0f, 0.0f, -1.0f);

                    cp.camera_direction[0] = cam_forward.x();
                    cp.camera_direction[1] = cam_forward.y();
                    cp.camera_direction[2] = cam_forward.z();
                }

                VkBuffer buf = ctx.resolveBufferHandle(params_rg);
                synchronizeBeforeBufferTransferWrites(
                    ctx.cmd,
                    std::array{buf}
                );
                vkCmdUpdateBuffer(ctx.cmd, buf, 0, sizeof(cp), &cp);
            }

            // Upload FrustumPlanes UBO
            {
                const ViewFrameData* cam_fd = cam ? cam->find(view_handle) : nullptr;
                if (cam_fd)
                {
                    const Frustum* frustum = &cam_fd->frustum;
                    FrustumPlanes fp{};
                    static_assert(sizeof(Frustum::Plane) == 4 * sizeof(float));
                    std::memcpy(fp.planes, frustum->planes.data(), sizeof(fp.planes));
                    VkBuffer buf = ctx.resolveBufferHandle(frustum_rg);
                    synchronizeBeforeBufferTransferWrites(
                        ctx.cmd,
                        std::array{buf}
                    );
                    vkCmdUpdateBuffer(ctx.cmd, buf, 0, sizeof(fp), &fp);
                }
            }

            // Barrier: transfer → compute
            {
                VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                barrier.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT
                                      | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                                      | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.memoryBarrierCount = 1;
                dep.pMemoryBarriers    = &barrier;
                vkCmdPipelineBarrier2(ctx.cmd, &dep);
            }

            // Reset atomic counters in instance buffer and indirect buffer
            {
                VkBuffer inst_buf = ctx.resolveBufferHandle(out_inst_rg);
                VkBuffer indr_buf = ctx.resolveBufferHandle(out_indirect_rg);
                synchronizeBeforeBufferTransferWrites(
                    ctx.cmd,
                    std::array{inst_buf, indr_buf}
                );
                vkCmdFillBuffer(ctx.cmd, inst_buf, 0, sizeof(uint32_t), 0u);
                vkCmdFillBuffer(ctx.cmd, indr_buf, 0, sizeof(uint32_t), 0u);

                VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                barrier.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT
                                      | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.memoryBarrierCount = 1;
                dep.pMemoryBarriers    = &barrier;
                vkCmdPipelineBarrier2(ctx.cmd, &dep);
            }

            const uint32_t groups = (node_buf_->nodeHighWaterMark() + 63u) / 64u;
            vkCmdDispatch(ctx.cmd, groups, 1, 1);
        })
        .setKernel("PointCloudCull");
}

// ============================================================================
//  Private — resource creation helpers
// ============================================================================

void PCFeatureIndirectBase::createDescriptorLayout(VkDevice device)
{
    (void)device;
    auto& ds = renderContext().descriptorService();

    // 5 bindings, all COMPUTE stage:
    //  0: NodeBuffer     SSBO (read-only)
    //  1: InstanceBuffer SSBO (write)
    //  2: IndirectBuffer SSBO (write)
    //  3: CullingParams  UBO
    //  4: FrustumPlanes  UBO
    VkDescriptorSetLayoutBinding b[5]{};
    for (uint32_t i = 0; i < 5; ++i)
    {
        b[i].binding         = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

    cull_layout_id_ = ds.registerLayout({
        .bindings = std::span<const VkDescriptorSetLayoutBinding>{b, 5},
        .flags = 0,
        .debug_name = "PointCloudCullSet"
    });
    cull_layout_ = ds.layout(cull_layout_id_);
}

// ============================================================================
//  addIndirectDrawPass — shared GRAPHICS indirect-draw pass for all three
//  indirect point-cloud modes. Only the push constant differs per mode, which
//  the caller supplies as a closure (H11 de-dup).
// ============================================================================
void PCFeatureIndirectBase::addIndirectDrawPass(
    RGBuilder&                                    builder,
    std::string_view                              pass_name,
    std::string_view                              color_target,
    std::string_view                              depth_target,
    RGResourceHandle                              indirect_rg,
    std::function<void(const PassRecordContext&)> push_constant_fn)
{
    builder.addPass(pass_name, ERGPassType::GRAPHICS)
        .write(builder.referenceTexture(color_target), lux::common::ETextureRole::COLOR_ATTACHMENT)
        .write(builder.referenceTexture(depth_target), lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
        .setPipeline(draw_handle_)
        .bindSceneDS()
        .read(indirect_rg, ERGBufferRole::INDIRECT)
        .setPhaseMask(phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::PointCloud)))
        .setKernelFn([this, indirect_rg, push_constant_fn = std::move(push_constant_fn)]
                     (const PassRecordContext& ctx)
        {
            if (!global_buf_ || global_buf_->buffer() == VK_NULL_HANDLE) return;
            if (!node_buf_ || node_buf_->nodeCount() == 0)
                return;

            // Mode-specific push constant (offset 8, VK_SHADER_STAGE_VERTEX_BIT).
            push_constant_fn(ctx);

            VkBuffer     vbuf   = global_buf_->buffer();
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vbuf, &offset);

            VkBuffer indirect_vk = ctx.resolveBufferHandle(indirect_rg);

            // Indirect buffer layout:
            //   [uint draw_count | 3 x uint pad]        bytes [0, 15]
            //   [VkDrawIndirectCommand x draw_count]    bytes [16, ...]
            vkCmdDrawIndirectCount(
                ctx.cmd,
                indirect_vk,                                   // buffer (commands)
                static_cast<VkDeviceSize>(16),                 // offset (skip 16-byte header)
                indirect_vk,                                   // countBuffer (draw_count at 0)
                static_cast<VkDeviceSize>(0),                  // countBufferOffset
                max_nodes_,                                    // maxDrawCount
                static_cast<uint32_t>(sizeof(IndirectDrawCommand)));
        })
        .setKernel("PointCloudDraw");
}

} // namespace lux::render

