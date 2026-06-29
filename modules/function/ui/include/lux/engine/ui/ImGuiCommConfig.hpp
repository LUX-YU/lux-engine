#pragma once
// ============================================================================
//  ImGuiCommConfig — trivially-copyable config for ImGuiFeature creation
//  via the FeatureFactory / RegisterFeatureType comm protocol.
//
//  Also contains: query name constants, OperationIds, and ImGuiProxy sugar.
// ============================================================================

#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/core/RenderTypes.hpp>
#include <lux/engine/function/visibility_ui.h>

namespace lux::render
{
    class RenderSession;
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

    /// Factory for ImGuiFeature — register via RenderSession::registerFeatureType().
    LUX_FUNCTION_UI_PUBLIC extern const lux::render::FeatureFactory kImGuiFeatureFactory;

    // =========================================================================
    //  Query name constants — use with queryTypeId() / findTypeId()
    // =========================================================================
    namespace imgui_ops
    {
        inline constexpr const char* kSubmitDrawData      = "ImGuiSubmitDrawData";
        inline constexpr const char* kAddUIView           = "ImGuiAddUIView";
        inline constexpr const char* kRemoveUIView        = "ImGuiRemoveUIView";
    }

    // =========================================================================
    //  Operation IDs — populated from FeatureTypeRegisteredReply::ops
    // =========================================================================
    struct ImGuiOperationIds
    {
        lux::render::TypeId submit_draw_data{lux::render::kInvalidTypeId};
        lux::render::TypeId add_ui_view{lux::render::kInvalidTypeId};
        lux::render::TypeId remove_ui_view{lux::render::kInvalidTypeId};

        static ImGuiOperationIds fromOps(const lux::render::TypeId* ops, uint32_t count) noexcept
        {
            ImGuiOperationIds ids{};
            if (count > 0) ids.submit_draw_data    = ops[0];
            if (count > 1) ids.add_ui_view         = ops[1];
            if (count > 2) ids.remove_ui_view      = ops[2];
            return ids;
        }
    };

    // =========================================================================
    //  Client-side proxy — ImGuiProxy
    // =========================================================================
    class LUX_FUNCTION_UI_PUBLIC ImGuiProxy
    {
    public:
        ImGuiProxy(lux::render::RenderSession& session, ImGuiOperationIds ops) noexcept
            : session_(&session), ops_(ops) {}

        /// Submit a snapshotted ImGui draw-data frame.
        void submitDrawData(uint32_t attachment_index);

        /// Create a UI offscreen view for scene viewport rendering.
        lux::render::RenderRequest<lux::render::ViewCreatedReply> addUIView(
            lux::render::RenderSceneId scene_id,
            lux::common::Size2D extent,
            const char* name,
            const float* initial_view_matrix = nullptr,
            const float* initial_proj_matrix = nullptr,
            const float* initial_camera_position = nullptr);

        /// Remove a UI offscreen view.
        void removeUIView(lux::render::RenderSceneId scene_id, lux::render::ViewHandle view);

    private:
        lux::render::RenderSession*  session_;
        ImGuiOperationIds            ops_;
    };

} // namespace lux::ui
