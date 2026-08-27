#pragma once
#include <lux/engine/function/render/client/core/Errors.hpp> // RenderError

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <limits>

#include <lux/engine/render/gpu/pipeline/RenderPassKey.hpp>
#include <lux/engine/render/graph/RGPassTypes.hpp>
#include <lux/engine/function/render/graph/DependencyAnalyzer.hpp>

namespace lux::render
{
    // Render pass information for each graphics pass
    struct RGPassInRenderPass
    {
        uint32_t pass_index;    // Logical pass index
        uint32_t subpass_index; // Subpass number in physical render pass (can be all 0 for now)

        // ── local-read merged-group remaps (line-B design P2) ────────────
        // Filled ONLY for passes inside a local_read group; empty vectors =
        // identity mapping (plain groups keep today's behaviour untouched).
        /// scope color slot → fragment output location of THIS pass
        /// (VK_ATTACHMENT_UNUSED = this pass does not write that slot).
        std::vector<uint32_t> color_locations;
        /// scope color slot → input attachment index declared by this pass's
        /// inputRead() (VK_ATTACHMENT_UNUSED = this pass does not read it).
        std::vector<uint32_t> input_indices;
        /// Input attachment index for reading the scope depth attachment
        /// (VK_ATTACHMENT_UNUSED = no depth input read).
        uint32_t depth_input_index = VK_ATTACHMENT_UNUSED;

        // ── per-pass loadOps(computeAttachmentOps 填充,serial 路径消费) ──
        /// 组按格式 key 合并,成员可写各自不同的资源;"组内首 pass 用组
        /// op、后续一律 LOAD" 会让某资源真正的首写者 LOAD 到未定义内容。
        /// 因此 loadOp 按 (pass, 资源) 首写判定,顺序与本 pass 的
        /// COLOR_ATTACHMENT 声明序一致(即 fillAttachmentInfos 的映射序)。
        /// local_read 合并组不用此字段(走 union_color_load_ops)。
        std::vector<VkAttachmentLoadOp> pass_color_load_ops;
        VkAttachmentLoadOp pass_depth_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;

        /// storeOp 同序对齐:TRANSIENT 资源且本 pass 之后(执行序)再无
        /// 任何访问者 → DONT_CARE(tiler 上免除 tile→显存写回)。
        /// imported/external 一律 STORE(终态可能被图外消费)。
        std::vector<VkAttachmentStoreOp> pass_color_store_ops;
        VkAttachmentStoreOp pass_depth_store_op = VK_ATTACHMENT_STORE_OP_STORE;
    };

    struct RGRenderPassGroup
    {
        RenderPassKey key;                      ///< Format + samples
        std::vector<RGPassInRenderPass> passes; ///< Passes within the group

        /// 组级 loadOp(computeAttachmentOps 填充)。fast path 消费(整组只在
        /// 首 pass Begin 一次);serial 路径按逐 pass 表(pass_*_load_ops)。
        /// 首写者 CLEAR、后继 LOAD 的判定按 (pass, 资源) 粒度完成后取首 pass。
        VkAttachmentLoadOp color_load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkAttachmentLoadOp depth_load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;

        // ── local-read merged group (line-B design P2) ───────────────────
        /// True when a consumer pass reading this group's attachments as
        /// input attachments (RGPassBuilder::inputRead) was merged in. The
        /// whole group records inside ONE dynamic-rendering scope whose
        /// attachment list is the UNION below (producer MRT + consumer
        /// outputs); per-pass location/input-index remaps ride
        /// RGPassInRenderPass. Requires KHR_dynamic_rendering_local_read —
        /// the FEATURE gates the inputRead declaration on DeviceCaps, the
        /// planner only reacts to declarations.
        bool local_read = false;
        /// Scope color attachment resource indices in slot order (matches
        /// key.color_formats). Only filled for local_read groups — plain
        /// groups keep deriving attachments from their first pass's refs.
        std::vector<uint32_t> union_color_res;
        /// Scope depth attachment resource index (UINT32_MAX = none).
        uint32_t union_depth_res = std::numeric_limits<uint32_t>::max();
        /// Per-union-slot loadOps (computeAttachmentOps). The single
        /// color_load_op tracks only ONE resource per group — a merged scope
        /// clears/loads EACH union attachment by its own first-writer status,
        /// and must mark ALL of them as written for later groups (a
        /// downstream writer like Skybox must LOAD the lighting result, not
        /// re-clear it). Only filled for local_read groups.
        std::vector<VkAttachmentLoadOp> union_color_load_ops;
        /// Per-union-slot storeOps:TRANSIENT 且作用域结束后无访问者 →
        /// DONT_CARE。G-buffer 三色的最后读者就是 scope 内的 lighting,
        /// 免写回正是 local_read 在 tiler 上的主要收益。深度被 scope 后
        /// 的 HzbBuild 读 → STORE。Only filled for local_read groups.
        std::vector<VkAttachmentStoreOp> union_color_store_ops;
        VkAttachmentStoreOp union_depth_store_op = VK_ATTACHMENT_STORE_OP_STORE;
        /// input-attachment 读的并集槽位掩码 + 深度输入标记(编译期由
        /// computeAttachmentOps 聚合一次;emit 与 serial 两个消费点直接读,
        /// 勿各自再扫 passes)。Only filled for local_read groups.
        uint8_t lr_input_mask = 0;
        bool lr_depth_input = false;
    };

    struct RGRenderPassLayoutInfo
    {
        std::vector<RGRenderPassGroup> groups;

        // Convenient query: pass_index -> (group_index, subpass_index)
        std::vector<uint32_t> pass_to_group; // size = pass_count, INVALID or group_index
        std::vector<uint32_t> pass_to_subpass;

        bool valid{true};
        RenderError error;
    };

    // Only responsible for: deducing RenderPassKey for each graphics pass based on Graph + dependency info
    class LUX_FUNCTION_PUBLIC RenderPassPlanner
    {
    public:
        /// @param max_color_attachments Ceiling for a merged group's color
        ///        attachment union, from DeviceCaps::max_color_attachments.
        ///        0 = unknown device -> falls back to the key's array capacity.
        ///        Never pass the array capacity as if it were a device limit:
        ///        Vulkan only guarantees 4 and a wider group fails at
        ///        vkCmdBeginRendering.
        static RGRenderPassLayoutInfo
        plan(const RGGraphDescription& graph, const RGDependencyInfo& deps, uint32_t max_color_attachments = 0);
    };
} // namespace lux::render
