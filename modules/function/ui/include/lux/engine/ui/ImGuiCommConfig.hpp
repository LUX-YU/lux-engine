#pragma once
// ============================================================================
//  ImGuiCommConfig — trivially-copyable config for ImGuiFeature creation
//  via the FeatureFactory / RegisterFeatureType comm protocol.
//
//  Also contains: query name constants, OperationIds, and ImGuiProxy sugar.
// ============================================================================

#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/function/render/client/core/RenderTypes.hpp>
#include <lux/engine/function/visibility_ui.h>

namespace lux::render
{
    class RenderFrameSession;
    template <typename T> class RenderRequest;
}

namespace lux::ui
{
    struct ImGuiCommConfig
    {
        lux::render::ETextureFormatHint color_format{lux::render::ETextureFormatHint::BGRA8};
        unsigned char* font_pixels{nullptr};
        int            font_width{0};
        int            font_height{0};
    };
    static_assert(std::is_trivially_copyable_v<ImGuiCommConfig>);

    /// Factory for ImGuiFeature — register via RenderFrameSession::registerFeatureType().
    LUX_FUNCTION_UI_PUBLIC extern const lux::render::FeatureFactory kImGuiFeatureFactory;

    // =========================================================================
    //  Query name constants — use with queryTypeId() / findTypeId()
    // =========================================================================
    namespace imgui_ops
    {
        inline constexpr const char* kSubmitDrawData      = "ImGuiSubmitDrawData";
        // (kAddUIView / kRemoveUIView 已消亡:UI 目标走核心命令面
        //  CreateOffscreenTarget(SAMPLED) + SetLayer。)
    }

    /// ImGui draw-data 快照的附件类型标签。
    ///
    /// 它此前住在 L0 的 `core/protocol/RenderCommTypes.hpp` —— 引擎的协议基础头里
    /// 写着 "ImGui"。而全仓只有两处真实用点(本模块的 UIRenderFrameSession 生产、
    /// UIRenderServer 消费),**render 模块自己一次都不读它**。
    ///
    /// 取值 3 沿用原值(1/2 为引擎保留,见 RenderCommTypes.hpp 的保留区说明),
    /// 所以线上格式逐字节不变。
    inline constexpr lux::render::TypeId kImGuiDrawDataAttachment = 3;

    // =========================================================================
    //  Operation IDs — populated from FeatureTypeRegisteredReply::ops
    // =========================================================================
    struct ImGuiOperationIds
    {
        lux::render::TypeId submit_draw_data{lux::render::kInvalidTypeId};

        static ImGuiOperationIds fromOps(const lux::render::TypeId* ops, uint32_t count) noexcept
        {
            ImGuiOperationIds ids{};
            if (count > 0) ids.submit_draw_data = ops[0];
            return ids;
        }
    };

    // =========================================================================
    //  Client-side proxy — ImGuiProxy
    // =========================================================================
    class LUX_FUNCTION_UI_PUBLIC ImGuiProxy
    {
    public:
        ImGuiProxy(lux::render::RenderFrameSession& session, ImGuiOperationIds ops) noexcept
            : session_(&session), ops_(ops) {}

        /// Submit a snapshotted ImGui draw-data frame.
        void submitDrawData(uint32_t attachment_index);

        // (addUIView / removeUIView 已消亡——见 imgui_ops 注释。)

    private:
        lux::render::RenderFrameSession*  session_;
        ImGuiOperationIds            ops_;
    };

} // namespace lux::ui
