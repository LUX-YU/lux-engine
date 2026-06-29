#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <limits>

#include <lux/engine/render/pipeline/RenderPassKey.hpp>
#include <lux/engine/render/graph/RGPassTypes.hpp>
#include <lux/engine/render/graph/RGDepAnalyzer.hpp>

namespace lux::render
{
    // Render pass information for each graphics pass
    struct RGPassInRenderPass {
        uint32_t pass_index; // Logical pass index
        uint32_t subpass_index;// Subpass number in physical render pass (can be all 0 for now)
    };

    struct RGRenderPassGroup {
        RenderPassKey key;                       ///< Format + samples
        std::vector<RGPassInRenderPass> passes;  ///< Passes within the group

        /// Pre-computed by RenderGraphCompiler::computeGroupLoadOps().
        /// Index of the first color / depth attachment resource written by this group.
        /// Used at record time to determine loadOp without re-scanning passes.
        uint32_t color_attachment_res_idx = std::numeric_limits<uint32_t>::max();
        uint32_t depth_attachment_res_idx = std::numeric_limits<uint32_t>::max();

        /// Pre-computed loadOps for static (non-conditional) render graphs.
        /// First group to write a resource → CLEAR; subsequent groups → LOAD.
        /// For conditional graphs the recorder recomputes this at runtime.
        VkAttachmentLoadOp color_load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkAttachmentLoadOp depth_load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
    };

    struct RGRenderPassLayoutInfo {
        std::vector<RGRenderPassGroup> groups;

        // Convenient query: pass_index -> (group_index, subpass_index)
        std::vector<uint32_t> pass_to_group;  // size = pass_count, INVALID or group_index
        std::vector<uint32_t> pass_to_subpass;

        bool        valid{true};
        std::string error_message;
    };

    // Only responsible for: deducing RenderPassKey for each graphics pass based on Graph + dependency info
    class LUX_FUNCTION_PUBLIC RenderPassPlanner
    {
    public:
        static RGRenderPassLayoutInfo plan(const RGGraphDescription& graph, const RGDependencyInfo& deps);
    };
} // namespace lux::render
