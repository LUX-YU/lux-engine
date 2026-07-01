#include <lux/engine/editor/panels/SceneFeatureSettingPanel.hpp>

#include <lux/engine/meta/Meta.hpp>                          // ReflectionRegistry / RefClass / RefField
#include <lux/engine/render/comm/client/RenderSession.hpp>   // RenderSession::builder()
#include <lux/engine/render/comm/RenderProtocol.hpp>         // opcodes / type_ids / payloads
#include <lux/engine/render/renderer/features/FeatureParamsOperation.hpp>  // FeatureParamsProxy (generic Apply)
#include <lux/engine/ui/ReflectedFieldTable.hpp>             // drawReflectedFieldTable scaffold

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>

namespace lux::editor
{
    SceneFeatureSettingPanel::SceneFeatureSettingPanel(std::string title)
        : Panel(std::move(title), {440.f, 480.f})
    {
        dispatch_.registerBuiltins();
    }

    bool SceneFeatureSettingPanel::drawReflectedStruct(const char* struct_name, void* instance)
    {
        const lux::meta::RefClass* rc =
            lux::meta::ReflectionRegistry::instance().findClass(struct_name);
        if (rc == nullptr)
        {
            ImGui::TextDisabled("reflection unavailable for %s\n(render_meta sidecar not loaded?)",
                                struct_name);
            return false;
        }

        lux::ui::drawReflectedFieldTable(
            struct_name, *rc, instance, 0.40f,
            [this](const lux::meta::RefField& field, void* base)
            { dispatch_.draw(field, base); });   // draw() owns the row + both columns
        return true;
    }

    void SceneFeatureSettingPanel::drawFeatureEditor()
    {
        if (features_.empty())
        {
            ImGui::TextDisabled(query_inflight_ ? "(enumerating features...)"
                                                : "No features in this scene.");
            return;
        }
        if (selected_feature_ < 0 || selected_feature_ >= static_cast<int>(features_.size()))
            selected_feature_ = 0;

        FeatureEntry& e = features_[static_cast<std::size_t>(selected_feature_)];

        ImGui::TextUnformatted(e.name.c_str());
        ImGui::Separator();

        // SpatialCull's user knob (cull_distance) is a SCENE-global setting edited in
        // the "Scene" tab (and mirrored to this feature by EditorScene). Its grid
        // cell_size is an internal perf detail. Redirect here so cull_distance has a
        // SINGLE source of truth — editing it in two places would conflict, and the
        // scene-tab mirror would clobber a feature-tab edit.
        if (e.name == "SpatialCull")
        {
            ImGui::TextWrapped("Cull distance is a scene setting — edit it in the "
                               "\"Scene\" tab. (The cull grid is internal.)");
            return;
        }

        if (e.struct_name.empty() || e.edit_bytes.empty())
        {
            ImGui::TextDisabled("No editable parameters.");
            return;
        }

        // Generic: the reflected field layout writes straight into the feature's
        // edit buffer (seeded with the live values from the enumerate query).
        const bool have_refl = drawReflectedStruct(e.struct_name.c_str(), e.edit_bytes.data());

        ImGui::Spacing();
        // Apply on confirm (not live): only FLAG it — paint() runs frame-CLOSED,
        // so the op is pushed from tickRender() (frame open).
        const bool can_apply = have_refl && session_ != nullptr;
        if (!can_apply) ImGui::BeginDisabled();
        if (ImGui::Button("Apply", ImVec2(120.f, 0.f)))
            apply_requested_ = true;
        if (!can_apply) ImGui::EndDisabled();
    }

    void SceneFeatureSettingPanel::drawRenderGraphDump()
    {
        ImGui::TextUnformatted("Render Graph (debug)");

        // Only FLAG the request — the command is pushed from tickRender() (frame
        // open); recording it here (frame closed) would be wiped by begin().
        if (ImGui::Button("Dump Render Graph") && session_ != nullptr && !dump_inflight_)
            dump_requested_ = true;

        // One-click copy of the whole dump to the clipboard.
        ImGui::SameLine();
        const bool can_copy = !graph_dump_text_.empty();
        if (!can_copy) ImGui::BeginDisabled();
        if (ImGui::Button("Copy"))
            ImGui::SetClipboardText(graph_dump_text_.c_str());
        if (!can_copy) ImGui::EndDisabled();

        if (dump_inflight_ || dump_requested_)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(dumping...)");
        }

        if (graph_dump_text_.empty())
        {
            ImGui::BeginChild("##graphdump", ImVec2(0.f, 0.f), true,
                              ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextDisabled("Click 'Dump Render Graph' to capture the current compiled graph.");
            ImGui::EndChild();
        }
        else
        {
            // Read-only multiline = selectable + Ctrl+A/Ctrl+C copyable, with the
            // graph's long lines scrolling horizontally (no wrap). Fill the rest
            // of the panel. ReadOnly means ImGui never writes back into the buffer,
            // so handing it the string's own storage (data(), size()+1) is safe.
            ImGui::InputTextMultiline(
                "##graphdump",
                graph_dump_text_.data(),
                graph_dump_text_.size() + 1,
                ImGui::GetContentRegionAvail(),
                ImGuiInputTextFlags_ReadOnly);
        }
    }

    void SceneFeatureSettingPanel::parseFeatureQuery(std::uint32_t count, std::size_t bytes)
    {
        // Preserve the current selection across a re-query by feature name.
        const std::string prev_sel =
            (selected_feature_ >= 0 && selected_feature_ < static_cast<int>(features_.size()))
                ? features_[static_cast<std::size_t>(selected_feature_)].name
                : std::string{};

        std::vector<FeatureEntry> next;
        next.reserve(count);

        const auto* p   = reinterpret_cast<const std::uint8_t*>(query_buf_.data());
        const auto* end = p + std::min(bytes, query_buf_.size());
        auto avail = [&](std::size_t n) { return static_cast<std::size_t>(end - p) >= n; };

        for (std::uint32_t i = 0; i < count; ++i)
        {
            if (!avail(8u + 1u + 2u)) break;   // id is now an 8-byte FeatureHandle (五-5)
            FeatureEntry e{};
            std::memcpy(&e.id, p, 8); p += 8;
            e.enabled = (*p++ != 0);
            std::uint16_t nl; std::memcpy(&nl, p, 2); p += 2;
            if (!avail(nl)) break;
            e.name.assign(reinterpret_cast<const char*>(p), nl); p += nl;

            if (!avail(2u)) break;
            std::uint16_t sl; std::memcpy(&sl, p, 2); p += 2;
            if (!avail(sl)) break;
            e.struct_name.assign(reinterpret_cast<const char*>(p), sl); p += sl;

            if (!avail(2u)) break;
            std::uint16_t pl; std::memcpy(&pl, p, 2); p += 2;
            if (!avail(pl)) break;
            e.edit_bytes.assign(p, p + pl); p += pl;

            next.push_back(std::move(e));
        }

        features_ = std::move(next);

        selected_feature_ = 0;
        for (int i = 0; i < static_cast<int>(features_.size()); ++i)
            if (features_[static_cast<std::size_t>(i)].name == prev_sel) { selected_feature_ = i; break; }
    }

    void SceneFeatureSettingPanel::tickRender()
    {
        if (session_ == nullptr)
            return;

        // 0) Enumerate the scene's features (once per request — requested on
        //    setTarget / Refresh). dst_ptr idiom: grow + re-issue if too small.
        if (query_requested_ && !query_inflight_)
        {
            query_buf_.assign(8 * 1024, '\0');
            query_req_       = session_->queryFeatureParams(
                scene_id_, query_buf_.data(), query_buf_.size());
            query_inflight_  = true;
            query_requested_ = false;
        }
        if (query_inflight_ && query_req_.isReady())
        {
            const lux::render::QueryFeatureParamsReply r = query_req_.result();
            if (r.needed > query_buf_.size())
            {
                query_buf_.assign(r.needed, '\0');
                query_req_ = session_->queryFeatureParams(
                    scene_id_, query_buf_.data(), query_buf_.size());
                // stay in flight for the (now large-enough) retry
            }
            else
            {
                parseFeatureQuery(r.count, r.written);
                query_inflight_ = false;
            }
        }

        // 1) Apply edits for the selected feature (INC-B, generic). Push the edited
        //    param blob to the feature's GENERIC setParams op, looked up BY NAME from
        //    the registry — works for ANY param-exposing feature (built-in or plugin),
        //    no per-feature typed payload. No-op if the feature has no params op.
        if (apply_requested_)
        {
            if (registry_ && selected_feature_ >= 0 &&
                selected_feature_ < static_cast<int>(features_.size()))
            {
                const FeatureEntry& e = features_[static_cast<std::size_t>(selected_feature_)];
                if (!e.struct_name.empty() && !e.edit_bytes.empty())
                    lux::render::FeatureParamsProxy(*session_).setParams(
                        scene_id_, e.id,   // full handle (五-5)
                        registry_->paramSetOp(e.name),
                        e.edit_bytes.data(), e.edit_bytes.size());
            }
            apply_requested_ = false;
        }

        // 2) Kick off a graph dump (the render thread writes dump_buf_ via dst_ptr
        //    until the request resolves — don't resize it while in flight).
        if (dump_requested_ && !dump_inflight_)
        {
            dump_buf_.assign(64 * 1024, '\0');   // generous; resized + re-issued if too small
            dump_req_      = session_->dumpRenderGraph(
                scene_id_, dump_buf_.data(), dump_buf_.size());
            dump_inflight_ = true;
            graph_dump_text_.clear();
        }
        dump_requested_ = false;

        // 3) Poll the dump (non-blocking): grow + re-issue if the buffer was small.
        if (dump_inflight_ && dump_req_.isReady())
        {
            const lux::render::RenderGraphDumpReply r = dump_req_.result();
            if (r.needed > dump_buf_.size())
            {
                dump_buf_.assign(r.needed, '\0');
                dump_req_ = session_->dumpRenderGraph(
                    scene_id_, dump_buf_.data(), dump_buf_.size());
                // stay in flight for the (now large-enough) retry
            }
            else
            {
                const std::size_t n = std::min<std::size_t>(r.written, dump_buf_.size());
                graph_dump_text_.assign(dump_buf_.data(), n);
                if (r.status != 0 && graph_dump_text_.empty())
                    graph_dump_text_ = "(dump failed — scene not found)";
                dump_inflight_ = false;
            }
        }
    }

    void SceneFeatureSettingPanel::paint()
    {
        if (session_ == nullptr)
        {
            ImGui::TextDisabled("Renderer not ready.");
            return;
        }

        if (ImGui::BeginTabBar("##scene_settings_tabs"))
        {
            // Scene-global view + streaming settings (the UE WorldSettings model):
            // cull_distance (render) + streaming ranges (gameplay), saved with the scene.
            if (ImGui::BeginTabItem("Scene"))
            {
                drawSceneTab();
                ImGui::EndTabItem();
            }
            // Per-feature render settings, auto-enumerated from the scene.
            if (ImGui::BeginTabItem("Features"))
            {
                drawFeaturesTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

    void SceneFeatureSettingPanel::drawSceneTab()
    {
        ImGui::TextDisabled("Scene-global view + streaming (saved with the scene)");
        ImGui::Spacing();

        void* settings = scene_settings_accessor_ ? scene_settings_accessor_() : nullptr;
        if (settings == nullptr)
        {
            ImGui::TextDisabled("No active scene.");
            return;
        }
        // Edited live in place: EditorScene dirty-applies on change (streaming →
        // WorldStreamingSystem; cull_distance mirrored to the render SpatialCull).
        if (!drawReflectedStruct("lux::gameplay::d3::SceneSettingsComponent", settings))
            ImGui::TextDisabled("(reflection sidecar not loaded)");

        ImGui::Spacing();
        ImGui::TextDisabled("Cull Distance = render draw range; streaming = entity");
        ImGui::TextDisabled("load/unload. Independent; keep cull >= unload.");
    }

    void SceneFeatureSettingPanel::drawFeaturesTab()
    {
        // Refresh re-enumerates the scene's features (e.g. after add/remove).
        if (ImGui::SmallButton("Refresh"))
            query_requested_ = true;
        if (query_inflight_)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(enumerating...)");
        }

        // Upper region: feature list (left) + selected feature's editor (right).
        const float region_h = ImGui::GetContentRegionAvail().y * 0.6f;
        ImGui::BeginChild("##features_region", ImVec2(0.f, region_h), false);
        {
            ImGui::BeginChild("##feature_list", ImVec2(150.f, 0.f), true);
            if (features_.empty())
            {
                ImGui::TextDisabled("(none)");
            }
            else
            {
                for (int i = 0; i < static_cast<int>(features_.size()); ++i)
                {
                    const FeatureEntry& e = features_[static_cast<std::size_t>(i)];
                    const std::string label =
                        std::format("{}{}", e.name, e.enabled ? "" : " (off)");
                    ImGui::PushID(i);
                    if (ImGui::Selectable(label.c_str(), selected_feature_ == i))
                        selected_feature_ = i;
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();

            ImGui::SameLine();

            ImGui::BeginChild("##feature_editor", ImVec2(0.f, 0.f), false);
            drawFeatureEditor();
            ImGui::EndChild();
        }
        ImGui::EndChild();

        ImGui::Separator();

        // Lower region: render-graph dump (debug).
        drawRenderGraphDump();
    }

} // namespace lux::editor
