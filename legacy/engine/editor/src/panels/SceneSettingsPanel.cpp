#include <lux/engine/editor/panels/SceneSettingsPanel.hpp>
#include <lux/engine/editor/scene/EditorScene.hpp>
#include <lux/engine/editor/app/Selection.hpp>

#include <lux/engine/meta/Meta.hpp>                          // ReflectionRegistry / RefClass / RefField
#include <lux/engine/function/render/client/RenderFrameSession.hpp>   // RenderFrameSession::builder()
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>         // opcodes / type_ids / payloads
#include <lux/engine/function/render/client/protocol/FeatureParamsOperation.hpp>  // FeatureParamsProxy (generic Apply)
#include <lux/engine/ui/ReflectedFieldTable.hpp>             // drawReflectedFieldTable scaffold

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <lux/cxx/core/Format.hpp>
#include <string>

namespace lux::editor
{
    SceneSettingsPanel::SceneSettingsPanel(std::string title)
        : Panel(std::move(title), {440.f, 480.f})
    {
        dispatch_.registerBuiltins();
    }

    bool SceneSettingsPanel::drawReflectedStruct(const char* struct_name, void* instance)
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

    void SceneSettingsPanel::drawFeatureEditor()
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
        const bool can_apply = have_refl && frame_ != nullptr;
        if (!can_apply) ImGui::BeginDisabled();
        if (ImGui::Button("Apply", ImVec2(120.f, 0.f)))
            apply_requested_ = true;
        if (!can_apply) ImGui::EndDisabled();
    }

    void SceneSettingsPanel::drawRenderGraphDump()
    {
        ImGui::TextUnformatted("Render Graph (debug)");

        // Only FLAG the request — the command is pushed from tickRender() (frame
        // open); recording it here (frame closed) would be wiped by begin().
        if (ImGui::Button("Dump Render Graph") && control_ != nullptr && !dump_inflight_)
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

    void SceneSettingsPanel::parseFeatureQuery(std::uint32_t count, std::size_t bytes)
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
            if (!avail(8u + 1u + 2u)) break;   // id is now an 8-byte FeatureHandle
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

    void SceneSettingsPanel::tickRender()
    {
        // Scene-domain target: between scenes the panel is
        // re-targeted with a null scene id / registry — idle until a live one.
        if (frame_ == nullptr || control_ == nullptr ||
            scene_id_.isNull() || registry_ == nullptr)
            return;

        // 0) Enumerate the scene's features (once per request — requested on
        //    setTarget / Refresh). dst_ptr idiom: grow + re-issue if too small.
        if (query_requested_ && !query_inflight_)
        {
            query_buf_.assign(8 * 1024, '\0');
            query_req_       = control_->queryFeatureParams(
                scene_id_, query_buf_.data(), query_buf_.size());
            query_inflight_  = true;
            query_requested_ = false;
        }
        if (query_inflight_ && query_req_.isReady())
        {
            const lux::render::QueryFeatureParamsReply r = query_req_.tryResult()->get();
            if (r.needed > query_buf_.size())
            {
                query_buf_.assign(r.needed, '\0');
                query_req_ = control_->queryFeatureParams(
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
                    lux::render::FeatureParamsProxy(*frame_).setParams(
                        scene_id_, e.id,   // full handle
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
            dump_req_      = control_->dumpRenderGraph(
                scene_id_, dump_buf_.data(), dump_buf_.size());
            dump_inflight_ = true;
            graph_dump_text_.clear();
        }
        dump_requested_ = false;

        // 3) Poll the dump (non-blocking): grow + re-issue if the buffer was small.
        if (dump_inflight_ && dump_req_.isReady())
        {
            const lux::render::RenderGraphDumpReply r = dump_req_.tryResult()->get();
            if (r.needed > dump_buf_.size())
            {
                dump_buf_.assign(r.needed, '\0');
                dump_req_ = control_->dumpRenderGraph(
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

    void SceneSettingsPanel::paint()
    {
        if (frame_ == nullptr || control_ == nullptr)
        {
            ImGui::TextDisabled("Renderer not ready.");
            return;
        }

        if (ImGui::BeginTabBar("##scene_settings_tabs"))
        {
            // Render-view settings; spatial demand is authored by scene contributions.
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
            if (ImGui::BeginTabItem("Terrain"))
            {
                drawTerrainTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

    void SceneSettingsPanel::drawSceneTab()
    {
        ImGui::TextDisabled("Render View settings");
        ImGui::Spacing();

        void* settings = scene_settings_accessor_ ? scene_settings_accessor_() : nullptr;
        if (settings == nullptr)
        {
            ImGui::TextDisabled("No active scene.");
            return;
        }
        // Edited live in place and mirrored to render SpatialCull.
        if (!drawReflectedStruct("lux::ecs::SceneSettingsComponent", settings))
            ImGui::TextDisabled("(reflection sidecar not loaded)");

        ImGui::Spacing();
        ImGui::Separator();
        drawVisualStateEditor();
    }

    void SceneSettingsPanel::drawVisualStateEditor()
    {
        ImGui::TextUnformatted("Scene Sky / Light / Fog");
        if (!editor_scene_ || !editor_scene_->isLive())
        {
            ImGui::TextDisabled("No live scene is open.");
            return;
        }

        auto& registry = editor_scene_->world().registry();
        entt::entity sky_entity = entt::null;
        entt::entity light_entity = entt::null;
        entt::entity fog_entity = entt::null;
        bool ambiguous = false;
        const auto find_singleton = [&]<class Component>(
            entt::entity& result)
        {
            for (const auto entity : registry.view<Component>())
            {
                if (result != entt::null)
                {
                    ambiguous = true;
                    return;
                }
                result = entity;
            }
        };
        find_singleton.template operator()<lux::ecs::SkyboxComponent>(
            sky_entity);
        find_singleton.template operator()<
            lux::ecs::DirectionalLightComponent>(light_entity);
        find_singleton.template operator()<lux::ecs::HeightFogComponent>(
            fog_entity);
        if (ambiguous || sky_entity == entt::null ||
            light_entity == entt::null || fog_entity == entt::null)
        {
            ImGui::TextDisabled(
                ambiguous
                    ? "Skybox, directional light and height fog must be unique."
                    : "Add one Skybox, Directional Light and Height Fog entity."
            );
            return;
        }

        if (!visual_state_loaded_)
        {
            skybox_edit_ = registry.get<lux::ecs::SkyboxComponent>(
                sky_entity);
            directional_light_edit_ = registry.get<
                lux::ecs::DirectionalLightComponent>(light_entity);
            height_fog_edit_ = registry.get<lux::ecs::HeightFogComponent>(
                fog_entity);
            const std::string sky = uuids::to_string(
                skybox_edit_.equirect_texture_id);
            const auto sky_count = std::min(sky.size(), sky_texture_id_text_.size() - 1u);
            std::ranges::copy_n(sky.begin(), sky_count, sky_texture_id_text_.begin());
            sky_texture_id_text_[sky_count] = '\0';
            visual_state_loaded_ = true;
        }

        ImGui::PushID("scene_visual_state");
        ImGui::InputText(
            "Sky Texture UUID",
            sky_texture_id_text_.data(),
            sky_texture_id_text_.size());
        ImGui::DragFloat(
            "Sky Rotation (radians)",
            &skybox_edit_.rotation_radians,
            0.01f);
        ImGui::DragFloat(
            "Sky Intensity",
            &skybox_edit_.intensity,
            0.02f,
            0.0f,
            100000.0f);

        if (ImGui::TreeNodeEx("Sun", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::DragFloat3(
                "Direction",
                directional_light_edit_.direction.data(),
                0.01f,
                -1.0f,
                1.0f);
            ImGui::ColorEdit3(
                "Color",
                directional_light_edit_.color.data(),
                ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
            ImGui::DragFloat(
                "Intensity",
                &directional_light_edit_.intensity,
                0.05f,
                0.0f,
                100000.0f);
            ImGui::Checkbox(
                "Shadows", &directional_light_edit_.cast_shadow);

            int cascades = static_cast<int>(
                directional_light_edit_.cascade_count);
            if (ImGui::SliderInt("Cascade Count", &cascades, 1, 8))
            {
                directional_light_edit_.cascade_count =
                    static_cast<std::uint32_t>(cascades);
            }
            int resolution = static_cast<int>(
                directional_light_edit_.shadow_map_size);
            if (ImGui::InputInt("Shadow Resolution", &resolution, 256, 1024))
            {
                resolution = std::clamp(resolution, 256, 8192);
                directional_light_edit_.shadow_map_size =
                    static_cast<std::uint32_t>(resolution);
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("Fog", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Checkbox("Enabled", &height_fog_edit_.enabled);
            ImGui::ColorEdit3(
                "Color",
                height_fog_edit_.color.data(),
                ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
            ImGui::DragFloat(
                "Density",
                &height_fog_edit_.density,
                0.00001f,
                0.0f,
                1000.0f,
                "%.6f");
            ImGui::DragFloat(
                "Start Distance",
                &height_fog_edit_.start_distance,
                1.0f,
                0.0f,
                1000000.0f);
            ImGui::DragFloat(
                "Reference Height",
                &height_fog_edit_.reference_height,
                1.0f);
            ImGui::DragFloat(
                "Height Falloff",
                &height_fog_edit_.height_falloff,
                0.0001f,
                0.0f,
                1000.0f,
                "%.5f");
            ImGui::SliderFloat(
                "Maximum Opacity",
                &height_fog_edit_.maximum_opacity,
                0.0f,
                1.0f);
            ImGui::TreePop();
        }

        const auto parsed_sky = uuids::uuid::from_string(
            sky_texture_id_text_.data());
        const bool valid_sky = parsed_sky.has_value();
        if (valid_sky)
            skybox_edit_.equirect_texture_id = *parsed_sky;
        const auto shadow_size = directional_light_edit_.shadow_map_size;
        const bool valid = valid_sky &&
            std::isfinite(skybox_edit_.rotation_radians) &&
            std::isfinite(skybox_edit_.intensity) &&
            skybox_edit_.intensity >= 0.0f &&
            directional_light_edit_.direction.allFinite() &&
            directional_light_edit_.direction.squaredNorm() > 1.0e-8f &&
            directional_light_edit_.color.allFinite() &&
            std::isfinite(directional_light_edit_.intensity) &&
            directional_light_edit_.intensity >= 0.0f &&
            directional_light_edit_.cascade_count >= 1u &&
            directional_light_edit_.cascade_count <= 8u &&
            shadow_size >= 256u && shadow_size <= 8192u &&
            (shadow_size & (shadow_size - 1u)) == 0u &&
            height_fog_edit_.color.allFinite() &&
            std::isfinite(height_fog_edit_.density) &&
            height_fog_edit_.density >= 0.0f &&
            std::isfinite(height_fog_edit_.start_distance) &&
            height_fog_edit_.start_distance >= 0.0f &&
            std::isfinite(height_fog_edit_.reference_height) &&
            std::isfinite(height_fog_edit_.height_falloff) &&
            height_fog_edit_.height_falloff >= 0.0f &&
            std::isfinite(height_fog_edit_.maximum_opacity) &&
            height_fog_edit_.maximum_opacity >= 0.0f &&
            height_fog_edit_.maximum_opacity <= 1.0f;
        if (!valid)
        {
            ImGui::TextColored(
                ImVec4{1.0f, 0.35f, 0.25f, 1.0f},
                "Invalid: UUID, non-zero light direction, finite bounded values, 1-8 cascades and power-of-two shadow resolution are required.");
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Apply ECS Visual Components"))
        {
            registry.patch<lux::ecs::SkyboxComponent>(
                sky_entity,
                [this](auto& component)
                {
                    component = skybox_edit_;
                });
            registry.patch<lux::ecs::DirectionalLightComponent>(
                light_entity,
                [this](auto& component)
                {
                    component = directional_light_edit_;
                });
            registry.patch<lux::ecs::HeightFogComponent>(
                fog_entity,
                [this](auto& component)
                {
                    component = height_fog_edit_;
                });
        }
        if (!valid)
            ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Revert ECS Visual Components"))
            visual_state_loaded_ = false;
        ImGui::PopID();
    }

    void SceneSettingsPanel::drawFeaturesTab()
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
                        lux::format("{}{}", e.name, e.enabled ? "" : " (off)");
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

    void SceneSettingsPanel::drawTerrainTab()
    {
        if (!editor_scene_ || !editor_scene_->isLive())
        {
            ImGui::TextDisabled("No active Authoring World.");
            return;
        }
        if (const auto& selected = editor_scene_->selection().object())
        {
            if (const auto* terrain = std::get_if<TerrainSelection>(
                    &*selected);
                terrain && (!last_terrain_selection_ ||
                    *last_terrain_selection_ != *terrain))
            {
                const auto text = uuids::to_string(terrain->terrain);
                const auto text_count = std::min(text.size(), terrain_id_text_.size() - 1u);
                std::ranges::copy_n(text.begin(), text_count, terrain_id_text_.begin());
                terrain_id_text_[text_count] = '\0';
                if (const auto* cell = std::get_if<
                        lux::authoring::PlanarCellCoord>(
                            &terrain->page.coordinate))
                {
                    terrain_cell_a_ = cell->a;
                    terrain_cell_b_ = cell->b;
                }
                last_terrain_selection_ = *terrain;
            }
        }

        ImGui::TextDisabled(
            "LXTP pages are loaded asynchronously; edits commit atomically.");
        ImGui::InputText(
            "Terrain Set UUID",
            terrain_id_text_.data(),
            terrain_id_text_.size());
        ImGui::InputScalar(
            "Cell X",
            ImGuiDataType_S64,
            &terrain_cell_a_);
        ImGui::InputScalar(
            "Cell Z",
            ImGuiDataType_S64,
            &terrain_cell_b_);
        ImGui::SliderInt(
            "Loaded radius (Cells)",
            &terrain_neighbourhood_,
            0,
            2);

        const auto terrain_uuid = uuids::uuid::from_string(
            terrain_id_text_.data());
        const auto terrain_space = editor_scene_->defaultWorldTerrainSpace();
        const auto valid_identity = terrain_uuid &&
            !terrain_uuid->is_nil() && terrain_space;
        const auto build_cells = [&]()
        {
            std::vector<lux::authoring::WorldCellKey> cells;
            const auto radius = static_cast<std::int64_t>(
                terrain_neighbourhood_);
            for (auto z = -radius; z <= radius; ++z)
            for (auto x = -radius; x <= radius; ++x)
            {
                if ((x < 0 && terrain_cell_a_ <
                        std::numeric_limits<std::int64_t>::min() - x) ||
                    (x > 0 && terrain_cell_a_ >
                        std::numeric_limits<std::int64_t>::max() - x) ||
                    (z < 0 && terrain_cell_b_ <
                        std::numeric_limits<std::int64_t>::min() - z) ||
                    (z > 0 && terrain_cell_b_ >
                        std::numeric_limits<std::int64_t>::max() - z))
                {
                    cells.clear();
                    return cells;
                }
                cells.push_back({
                    lux::authoring::EPartitionTopology::PLANAR_XZ,
                    lux::authoring::PlanarCellCoord{
                        terrain_cell_a_ + x,
                        terrain_cell_b_ + z}});
            }
            return cells;
        };
        auto cells = build_cells();
        if (!valid_identity || cells.empty())
            ImGui::BeginDisabled();
        if (ImGui::Button("Load edit region"))
        {
            (void)editor_scene_->requestWorldTerrainRegion(
                lux::authoring::TerrainSetId{*terrain_uuid},
                *terrain_space,
                cells);
        }
        if (!valid_identity || cells.empty())
            ImGui::EndDisabled();

        ImGui::SeparatorText("Brush");
        static constexpr std::array modes{
            "Raise / Lower",
            "Smooth",
            "Flatten",
            "Weight Paint",
            "Hole Paint"};
        auto mode = static_cast<int>(terrain_brush_.mode);
        if (ImGui::Combo(
                "Mode", &mode, modes.data(), static_cast<int>(modes.size())))
        {
            terrain_brush_.mode = static_cast<
                lux::authoring::EWorldTerrainBrushMode>(mode);
        }
        ImGui::DragFloat(
            "Center local X", &terrain_local_x_, 0.25f, 0.0f, 128.0f);
        ImGui::DragFloat(
            "Center local Z", &terrain_local_z_, 0.25f, 0.0f, 128.0f);
        ImGui::DragFloat(
            "Radius", &terrain_brush_.radius, 0.1f, 0.01f, 4096.0f);
        ImGui::SliderFloat(
            "Falloff", &terrain_brush_.falloff, 0.0f, 1.0f);
        ImGui::DragFloat(
            "Strength", &terrain_brush_.strength, 0.01f, -128.0f, 128.0f);
        if (terrain_brush_.mode ==
            lux::authoring::EWorldTerrainBrushMode::FLATTEN)
        {
            ImGui::DragFloat(
                "Target height",
                &terrain_brush_.flatten_height,
                0.1f);
        }
        if (terrain_brush_.mode ==
            lux::authoring::EWorldTerrainBrushMode::WEIGHT_PAINT)
        {
            int layer = terrain_brush_.weight_layer;
            int value = terrain_brush_.weight_value;
            ImGui::SliderInt("Weight layer", &layer, 0, 7);
            ImGui::SliderInt("Weight value", &value, 0, 255);
            terrain_brush_.weight_layer = static_cast<std::uint8_t>(layer);
            terrain_brush_.weight_value = static_cast<std::uint8_t>(value);
        }
        if (terrain_brush_.mode ==
            lux::authoring::EWorldTerrainBrushMode::HOLE_PAINT)
        {
            ImGui::Checkbox("Create hole", &terrain_brush_.hole_value);
        }

        const lux::authoring::WorldCellKey center_cell{
            lux::authoring::EPartitionTopology::PLANAR_XZ,
            lux::authoring::PlanarCellCoord{
                terrain_cell_a_, terrain_cell_b_}};
        const auto center = valid_identity
            ? editor_scene_->makeWorldTerrainPosition(
                  *terrain_space,
                  center_cell,
                  terrain_local_x_,
                  terrain_local_z_)
            : std::nullopt;
        if (!valid_identity || cells.empty() || !center)
            ImGui::BeginDisabled();
        if (ImGui::Button("Apply stroke"))
        {
            (void)editor_scene_->applyWorldTerrainBrush(
                lux::authoring::TerrainSetId{*terrain_uuid},
                cells,
                *center,
                terrain_brush_);
        }
        if (!valid_identity || cells.empty() || !center)
            ImGui::EndDisabled();

        const auto stats = editor_scene_->worldTerrainEditStats();
        ImGui::SameLine();
        ImGui::BeginDisabled(stats.undo_depth == 0u);
        if (ImGui::Button("Undo"))
            (void)editor_scene_->undoWorldTerrainEdit();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(stats.redo_depth == 0u);
        if (ImGui::Button("Redo"))
            (void)editor_scene_->redoWorldTerrainEdit();
        ImGui::EndDisabled();

        ImGui::SeparatorText("16-bit Heightmap (RAW little-endian)");
        ImGui::InputText(
            "RAW16 path",
            terrain_raw16_path_.data(),
            terrain_raw16_path_.size());
        const auto can_heightmap = valid_identity && !cells.empty() &&
            terrain_raw16_path_[0] != '\0' &&
            !stats.heightmap_io_pending;
        ImGui::BeginDisabled(!can_heightmap);
        if (ImGui::Button("Import RAW16"))
        {
            (void)editor_scene_->requestImportWorldTerrainHeightmap16(
                lux::authoring::TerrainSetId{*terrain_uuid},
                cells,
                std::filesystem::path{terrain_raw16_path_.data()});
        }
        ImGui::SameLine();
        if (ImGui::Button("Export RAW16"))
        {
            (void)editor_scene_->requestExportWorldTerrainHeightmap16(
                lux::authoring::TerrainSetId{*terrain_uuid},
                cells,
                std::filesystem::path{terrain_raw16_path_.data()});
        }
        ImGui::EndDisabled();
        if (stats.heightmap_io_pending)
            ImGui::TextDisabled("(heightmap IO pending...)");

        ImGui::SeparatorText("Status");
        ImGui::Text(
            "Pages: %zu loaded, %zu dirty",
            stats.loaded_pages,
            stats.dirty_pages);
        ImGui::Text(
            "History: %zu undo, %zu redo%s",
            stats.undo_depth,
            stats.redo_depth,
            stats.load_pending ? " (loading...)" : "");
        if (!stats.last_error.empty())
        {
            ImGui::PushStyleColor(
                ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.25f, 1.0f));
            ImGui::TextWrapped("%s", stats.last_error.c_str());
            ImGui::PopStyleColor();
        }
    }

} // namespace lux::editor
