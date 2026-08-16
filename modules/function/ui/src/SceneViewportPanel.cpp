#include <lux/engine/ui/SceneViewportPanel.hpp>

#include <cstring>

namespace lux::ui
{

SceneViewportPanel::SceneViewportPanel(std::string title,
                                         std::array<float, 2> suggest_size)
    : Panel(std::move(title), suggest_size)
{}

SceneViewportPanel::~SceneViewportPanel() = default;

void SceneViewportPanel::setTextureID(ImTextureID id) noexcept
{
    texture_id_ = id;
}

ImTextureID SceneViewportPanel::textureID() const noexcept
{
    return texture_id_;
}

void SceneViewportPanel::paint()
{
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x <= 0 || avail.y <= 0)
    {
        content_size_ = {0, 0};
        return;
    }

    ImVec2 display_size = avail;
    if (aspect_ratio_ > 0.f)
    {
        float avail_aspect = avail.x / avail.y;
        if (avail_aspect > aspect_ratio_)
            display_size.x = avail.y * aspect_ratio_;
        else
            display_size.y = avail.x / aspect_ratio_;
    }

    content_size_ = display_size;

    // Notify on size change (truncate to integer pixels for stable comparison)
    const ImVec2 fb_scale = ImGui::GetIO().DisplayFramebufferScale;
    uint32_t w  = static_cast<uint32_t>(display_size.x * fb_scale.x);
    uint32_t h  = static_cast<uint32_t>(display_size.y * fb_scale.y);
    uint32_t pw = static_cast<uint32_t>(prev_content_size_.x * fb_scale.x);
    uint32_t ph = static_cast<uint32_t>(prev_content_size_.y * fb_scale.y);
    if ((w != pw || h != ph) && w > 0 && h > 0)
    {
        prev_content_size_ = display_size;
        if (on_resized) on_resized({w, h});
    }

    // Capture the image's screen position BEFORE rendering it, so we can
    // convert global mouse coordinates back to content-rect-relative
    // (top-left = 0,0) below.
    const ImVec2 image_screen_pos = ImGui::GetCursorScreenPos();

    if (texture_id_)
    {
        ImGui::Image(texture_id_, display_size);
    }
    else
    {
        ImGui::TextDisabled("No texture bound");
    }

    // ── Asset drop target ────────────────────────────────────────────────
    //
    // Catches AssetBrowser drag-source releases over the rendered image.
    // We use the same "last item" anchor as IsItemHovered above so the
    // drop area matches the visible image exactly. ImGui memcpys the
    // payload bytes into its scratch on the source side, so reading via
    // `Data` here is safe across frames within the same drop event.
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetDragPayloadTag))
        {
            if (p->DataSize == sizeof(AssetDragPayload))
            {
                AssetDragPayload payload{};
                std::memcpy(&payload, p->Data, sizeof(payload));
                const ImVec2 mp = ImGui::GetMousePos();
                if (on_asset_dropped)
                    on_asset_dropped({
                        payload,
                        mp.x - image_screen_pos.x,
                        mp.y - image_screen_pos.y,
                        display_size.x, display_size.y});
            }
        }
        ImGui::EndDragDropTarget();
    }

    // ── M2: pointer state for picking + UE-style camera ──────────────────
    //
    // IsItemHovered / IsItemFocused refer to the last submitted item — which
    // is the ImGui::Image (or the TextDisabled fallback). The hover check is
    // intentionally permissive (no AllowWhenBlockedByActiveItem) so that an
    // active drag inside another panel (e.g. AssetBrowser drag-drop) does NOT
    // also count as hovering the viewport.
    pointer_.hovered      = ImGui::IsItemHovered();
    pointer_.focused      = ImGui::IsItemFocused();
    pointer_.content_size = display_size;
    if (pointer_.hovered)
    {
        const ImVec2 mp = ImGui::GetMousePos();
        pointer_.pos_in_content =
            ImVec2(mp.x - image_screen_pos.x, mp.y - image_screen_pos.y);
    }
    else
    {
        pointer_.pos_in_content = ImVec2(0, 0);
    }

    // ── Pick on click ────────────────────────────────────────────────────
    //
    // A pick must be a clean LMB *click* with NO camera interaction. The camera
    // shares LMB (orbit), RMB (fly) and MMB (pan), so several gestures must be
    // excluded — otherwise nudging the view also re-selects in the Hierarchy:
    //
    //   1. RMB / MMB held → fly / pan in progress: never pick.
    //   2. `owns_mouse_` → the host flagged a camera-owning drag (RMB fly): the
    //      cursor is captured, so ImGui's drag delta reads ~0 and a stale LMB
    //      release would look like a click. Suppress during, AND for a few
    //      frames after it ends (the flag has a one-frame lag; the captured
    //      cursor's frozen delta also lingers) — `pick_cooldown_`.
    //   3. `lmb_drag_latched_` → this LMB hold passed the drag threshold at any
    //      point → it's an orbit, not a click. Latched (not just checked at
    //      release) so a drag that wanders back near its start still counts.
    const ImGuiIO& io = ImGui::GetIO();

    if (prev_owns_mouse_ && !owns_mouse_)
        pick_cooldown_ = 3;                 // camera ownership just ended → absorb the tail
    prev_owns_mouse_ = owns_mouse_;
    if (pick_cooldown_ > 0)
        --pick_cooldown_;

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        lmb_drag_latched_ = false;          // fresh press → assume a click until it drags
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        const ImVec2 d   = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f);
        const float  thr = io.MouseDragThreshold;
        if (d.x * d.x + d.y * d.y >= thr * thr)
            lmb_drag_latched_ = true;        // moved past the click radius → it's a drag
    }

    if (pointer_.hovered &&
        !owns_mouse_ &&
        pick_cooldown_ == 0 &&
        !lmb_drag_latched_ &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Right) &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Middle) &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        if (on_picked)
            on_picked({pointer_.pos_in_content.x, pointer_.pos_in_content.y,
                       display_size.x, display_size.y});
    }

    // ── Context menu on RMB tap ──────────────────────────────────────────
    //
    // Same disambiguation as the pick above, for the RIGHT button: a clean
    // tap (no drag latch, no camera ownership, cooldown drained) opens the
    // popup at the tap position. In a camera scheme where RMB-hold flies,
    // owns_mouse_ engages on press and wins — see the header note.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        rmb_drag_latched_ = false;          // fresh press → assume a tap until it drags
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
    {
        const ImVec2 d   = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right, 0.f);
        const float  thr = io.MouseDragThreshold;
        if (d.x * d.x + d.y * d.y >= thr * thr)
            rmb_drag_latched_ = true;        // moved past the click radius → camera drag
    }

    if (context_menu_hook_ &&
        pointer_.hovered &&
        !owns_mouse_ &&
        pick_cooldown_ == 0 &&
        !rmb_drag_latched_ &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Middle) &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Right))
    {
        ctx_click_ = pointer_;               // freeze the tap position for the menu
        ImGui::OpenPopup("##scene_viewport_ctx");
    }

    if (ImGui::BeginPopup("##scene_viewport_ctx"))
    {
        if (context_menu_hook_)
            context_menu_hook_(ctx_click_);
        ImGui::EndPopup();
    }
}

} // namespace lux::ui
