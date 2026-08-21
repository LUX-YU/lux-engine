#include <lux/engine/editor/panels/InspectorPanel.hpp>
#include <lux/engine/editor/app/Selection.hpp>
#include <lux/engine/editor/AssetRegistry.hpp>
#include <lux/engine/editor/scene/EditorScene.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>       // vfs->pathOf for tooltips

#include <lux/engine/ui/AssetDragDrop.hpp>
#include <lux/engine/ui/ReflectedFieldTable.hpp>   // drawReflectedFieldTable scaffold
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/serialization/TaggedPropertyArchive.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/cxx/compile_time/type_info.hpp>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace lux::editor
{
    namespace
    {
        // Display label for a component:
        //   1. class-level annotation `display_name=...` if present
        //   2. short class name (segment after the final `::`) otherwise
        std::string displayLabelFor(
            const lux::ecs::ComponentSchemaDescriptor& ce)
        {
            if (ce.ref_class)
            {
                auto annot = ce.ref_class->annotations();
                if (auto dn = annot.get("display_name"))
                    return std::string(*dn);
            }

            std::string_view full = ce.fullName();
            if (auto pos = full.rfind("::"); pos != std::string_view::npos)
                full.remove_prefix(pos + 2);
            return std::string(full);
        }

        // Map the stable `asset_type=` tokens exported by Resource Asset.
        bool assetTypeFromToken(std::string_view tok, lux::asset::EAssetType& out) noexcept
        {
            using lux::asset::EAssetType;
            if      (tok == "texture")           out = EAssetType::TEXTURE;
            else if (tok == "model")             out = EAssetType::MODEL;
            else if (tok == "shader")            out = EAssetType::SHADER;
            else if (tok == "mesh")              out = EAssetType::MESH;
            else if (tok == "font")              out = EAssetType::FONT;
            else if (tok == "sound")             out = EAssetType::SOUND;
            else if (tok == "script")            out = EAssetType::SCRIPT;
            else if (tok == "skeleton")          out = EAssetType::SKELETON;
            else if (tok == "animation_clip")    out = EAssetType::ANIMATION_CLIP;
            else if (tok == "material")          out = EAssetType::MATERIAL;
            else if (tok == "material_instance") out = EAssetType::MATERIAL_INSTANCE;
            else if (tok == "texture_atlas")     out = EAssetType::TEXTURE_ATLAS;
            else if (tok == "flipbook_clip")     out = EAssetType::FLIPBOOK_CLIP;
            else if (tok == "flow_graph")        out = EAssetType::FLOW_GRAPH;
            else return false;
            return true;
        }

        // Pipe-separated tokens are allowed. Missing, empty or unknown tokens
        // reject every asset; there is no field-name or "accept any" fallback.
        int expectedTypesFromAnnotation(const lux::meta::RefField& field,
                                        lux::asset::EAssetType (&out)[4]) noexcept
        {
            auto at = field.annotations().get("asset_type");
            if (!at) return -1;
            if (at->empty()) return 0;
            int n = 0;
            std::string_view rest = *at;
            while (!rest.empty())
            {
                const auto bar = rest.find('|');
                const auto tok = rest.substr(0, bar);
                lux::asset::EAssetType t;
                if (n == 4 || !assetTypeFromToken(tok, t))
                    return 0;
                out[n++] = t;
                if (bar == std::string_view::npos) break;
                rest = rest.substr(bar + 1);
            }
            return n;
        }

        bool fieldAcceptsType(const lux::meta::RefField& field,
                              lux::asset::EAssetType t) noexcept
        {
            lux::asset::EAssetType acc[4];
            const int n = expectedTypesFromAnnotation(field, acc);
            if (n <= 0) return false;
            for (int i = 0; i < n; ++i)
                if (acc[i] == t) return true;
            return false;
        }
    } // namespace

    InspectorPanel::InspectorPanel(
        std::string title,
        const lux::ecs::ComponentTypeCatalog& components)
        : Panel(std::move(title), {360.f, 600.f})
        , components_(components)
    {
        dispatch_.registerBuiltins();

        // The fuzzy-search "pick" popup writes the chosen asset back through the
        // re-resolved component base (avoids a dangling pointer if ECS storage
        // moved between opening the picker and selecting).
        asset_picker_.setHint("search assets...");
        asset_picker_.setOnSelect([this](std::uint32_t idx)
        {
            if (idx >= picker_candidates_.size() || !pick_resolver_)
                return;
            if (void* base = pick_resolver_())
            {
                *reinterpret_cast<lux::asset::asset_id_t*>(
                    static_cast<std::uint8_t*>(base) + pick_offset_) =
                        picker_candidates_[idx];
                // 写契约:popup 在组件表之外落地,component_edited_ 的消费点
                // 已经过了 —— 自带 notify(patch<T> → on_update)。
                if (pick_notify_) pick_notify_();
            }
        });
    }

    // -------------------------------------------------------------------------
    // displayField — asset-reference fields first, then WidgetDispatch, else hex
    // -------------------------------------------------------------------------
    // -------------------------------------------------------------------------
    //  drawAddComponentMenu — capability-gated (editor ADR §7)
    // -------------------------------------------------------------------------
    void InspectorPanel::drawAddComponentMenu(
        lux::ecs::RegistryBase& reg,
        entt::entity e)
    {
        if (!ImGui::BeginPopup("##add_component"))
            return;

        // Which FQNs this scene's active recipes make meaningful. Empty (no
        // active scene / no provider) → offer everything (no constraint).
        std::vector<std::string> avail = available_components_
            ? available_components_()
            : std::vector<std::string>{};
        const bool constrained = !avail.empty();
        const auto isAvailable = [&](std::string_view fqn)
        {
            if (!constrained) return true;
            // Kernel-resident components (Name/Hierarchy) carry no capability and
            // are always addable; the recipe union only gates domain components.
            if (fqn.find("::NameComponent")      != std::string_view::npos ||
                fqn.find("::ParentComponent") != std::string_view::npos ||
                fqn.find("::ScriptComponent")    != std::string_view::npos)   // scripts are universal — any scene kind
                return true;
            for (auto a : avail) if (a == fqn) return true;
            return false;
        };

        for (const auto& ce : components_.all())
        {
            if (!ce.operations.emplace || !ce.operations.has) continue;
            const bool present   = ce.has(reg, e);
            const bool available = isAvailable(ce.fullName());
            const std::string label = displayLabelFor(ce);

            // Present → shown disabled (already on the entity). Not available in
            // this scene → disabled (greyed) so the user SEES it exists but must
            // enable the capability. Available + absent → clickable.
            ImGui::BeginDisabled(present || !available);
            if (ImGui::MenuItem(label.c_str(), present ? "on entity" : nullptr))
                ce.operations.emplace(reg, e);
            ImGui::EndDisabled();
            if (!available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Not available in this scene kind — enable the "
                                  "owning capability first.");
        }
        ImGui::EndPopup();
    }

    void InspectorPanel::displayField(const lux::meta::RefField& field,
                                       void* base)
    {
        if (field.visibility != lux::meta::EVisibility::Public)
            return;

        // An asset_id_t (uuids::uuid) field renders as a named, type-validated
        // picker rather than a hex id (material / mesh / texture references).
        if (drawAssetField(field, base))
            return;

        if (dispatch_.draw(field, base, &component_edited_))
            return;

        // Fallback: show type name + first 8 bytes as hex
        auto* ptr = static_cast<uint8_t*>(base) + field.offset;
        uint64_t raw = 0;
        if (field.type.size > 0 && field.type.size <= 8)
            std::memcpy(&raw, ptr, field.type.size);

        auto annot   = field.annotations();
        auto dn      = annot.get("display_name");
        auto label   = dn.value_or(field.name);
        char lbuf[128];
        const std::size_t nl = std::min(label.size(), std::size_t{127});
        std::memcpy(lbuf, label.data(), nl); lbuf[nl] = '\0';

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(lbuf);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("[%.*s] 0x%llX",
                    static_cast<int>(field.type.name.size()),
                    field.type.name.data(),
                    static_cast<unsigned long long>(raw));
    }

    // -------------------------------------------------------------------------
    // drawAssetField — name + type-validated drag-drop target + search picker
    // -------------------------------------------------------------------------
    bool InspectorPanel::drawAssetField(const lux::meta::RefField& field,
                                        void* base)
    {
        // Only asset_id_t (== uuids::uuid) fields. Match by the compile-time hash
        // (the reflection generator hashes field types with the same
        // lux::cxx::type_hash the widget table uses) OR by the canonical type
        // name — a belt-and-suspenders guard against the alias being spelled
        // differently by the generator, so the feature never silently degrades
        // to the hex fallback.
        static const std::uint64_t kUuidHash =
            lux::cxx::type_hash<lux::asset::asset_id_t>();
        const std::string_view tn = field.type.name;
        const bool is_uuid =
            field.type.hash == kUuidHash ||
            tn == "uuids::uuid" ||
            tn == "lux::asset::asset_id_t" ||
            (tn.size() >= 4 && tn.substr(tn.size() - 4) == "uuid");
        if (!is_uuid)
            return false;
        if (!lux::ecs::serialization::isAssetReferenceField(field))
            return false;

        auto* id_ptr = reinterpret_cast<lux::asset::asset_id_t*>(
            static_cast<std::uint8_t*>(base) + field.offset);

        // ---- label (display_name annotation or field name) ----
        auto annot    = field.annotations();
        auto dn       = annot.get("display_name");
        auto label_sv = dn.value_or(field.name);
        char lbuf[128];
        {
            const std::size_t n = std::min(label_sv.size(), std::size_t{127});
            std::memcpy(lbuf, label_sv.data(), n); lbuf[n] = '\0';
        }

        // ---- resolve the bound asset's display name ----
        std::string shown;
        if (id_ptr->is_nil())
            shown = "(none)";
        else if (registry_)
        {
            if (const AssetMeta* m = registry_->find(*id_ptr)) shown = m->name;
            else                                               shown = "<unresolved>";
        }
        else
            shown = "<id set>";

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(lbuf);
        ImGui::TableSetColumnIndex(1);

        ImGui::PushID(static_cast<int>(field.offset)); // stable per-field in this table

        const float frame_h = ImGui::GetFrameHeight();
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float avail   = ImGui::GetContentRegionAvail().x;
        const float name_w  = std::max(40.0f, avail - 2.0f * (frame_h + spacing));

        // The bound-name button doubles as the type-validated drop target.
        const std::string btn = shown + "##bind";
        ImGui::Button(btn.c_str(), ImVec2(name_w, 0.0f));
        // Hovering reveals the asset's virtual path (the soft address the
        // VFS/scripts resolve). The VFS answers for every mount (/Game,
        // /Engine, future paks); the registry only knows loose /Game files,
        // so it is the fallback; in-memory assets (editor builtins) get an
        // honest label instead of silence.
        if (ImGui::IsItemHovered() && !id_ptr->is_nil())
        {
            std::string tip;
            if (asset_mgr_)
                if (const auto vfs = asset_mgr_->vfs())
                    if (auto p = vfs->pathOf(*id_ptr))
                        tip = std::move(*p);
            if (tip.empty() && registry_)
                if (const AssetMeta* m = registry_->find(*id_ptr))
                {
                    tip = "/Game/" + m->rel_path;
                    if (const auto dot = tip.rfind('.'); dot != std::string::npos)
                        tip.resize(dot);
                }
            if (tip.empty())
                tip = "(in-memory asset — no virtual path)";
            ImGui::SetTooltip("%s", tip.c_str());
        }
        if (ImGui::BeginDragDropTarget())
        {
            // Peek before accepting so a wrong-typed payload shows the no-drop
            // cursor instead of a misleading "accept" highlight.
            const ImGuiPayload* peek = ImGui::GetDragDropPayload();
            const bool decoded = peek && peek->IsDataType(lux::ui::kAssetDragPayloadTag) &&
                                 peek->DataSize ==
                                     static_cast<int>(sizeof(lux::ui::AssetDragPayload));
            lux::ui::AssetDragPayload payload{};
            bool ok = decoded;
            if (decoded)
            {
                std::memcpy(&payload, peek->Data, sizeof(payload));
                ok = fieldAcceptsType(field,
                        static_cast<lux::asset::EAssetType>(payload.asset_type));
            }
            if (ok && ImGui::AcceptDragDropPayload(lux::ui::kAssetDragPayloadTag))
            {
                std::array<std::uint8_t, 16> ub{};
                std::memcpy(ub.data(), payload.uuid_bytes, ub.size());
                *id_ptr = lux::asset::asset_id_t(ub);
                component_edited_ = true;   // 写契约:拖放写入也要走 notify→patch
            }
            // 错型资产维持禁止光标即可,不在拖拽进行中弹 tooltip ——
            // 多视口模式下拖拽中的 tooltip 是独立 OS 窗口(swapchain 创建),
            // 是既往崩溃现场(见 material-model-design.md 症状清单)。
            ImGui::EndDragDropTarget();
        }
        if (!id_ptr->is_nil() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", shown.c_str());

        // Pick (fuzzy search) + clear.
        ImGui::SameLine(0.0f, spacing);
        if (ImGui::Button("...##pick", ImVec2(frame_h, 0.0f)))
            openAssetPicker(field);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Search assets");

        ImGui::SameLine(0.0f, spacing);
        if (ImGui::Button("x##clear", ImVec2(frame_h, 0.0f)))
        {
            *id_ptr = lux::asset::asset_id_t{};
            component_edited_ = true;       // 同上:清空也是一次编辑
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear");

        ImGui::PopID();
        return true;
    }

    // -------------------------------------------------------------------------
    // openAssetPicker — gather type-acceptable assets + open the search popup
    // -------------------------------------------------------------------------
    void InspectorPanel::openAssetPicker(const lux::meta::RefField& field)
    {
        picker_candidates_.clear();
        std::vector<std::string> names;
        if (registry_)
        {
            for (const auto& m : registry_->all())
            {
                if (fieldAcceptsType(field, m.type))
                {
                    picker_candidates_.push_back(m.id);
                    names.push_back(m.name);
                }
            }
        }
        asset_picker_.setItems(names);
        pick_offset_   = field.offset;
        pick_resolver_ = current_comp_resolver_; // re-resolves the base on select
        pick_notify_   = current_comp_notify_;   // fires patch<T> after the write
        asset_picker_.open();
    }

    // -------------------------------------------------------------------------
    // paint — auto-discovers components from ComponentTypeCatalog
    // -------------------------------------------------------------------------
    void InspectorPanel::paintWorldInstance(
        lux::authoring::WorldInstanceId instance_id)
    {
        const auto snapshot = scene_
            ? scene_->worldInstance(instance_id)
            : std::nullopt;
        if (!snapshot)
        {
            ImGui::TextDisabled("Loading Instance Page...");
            return;
        }

        auto instance = *snapshot;
        ImGui::TextUnformatted("World Instance");
        ImGui::TextDisabled(
            "%s / %llu",
            uuids::to_string(instance.id.set.value()).c_str(),
            static_cast<unsigned long long>(instance.id.local_id));
        ImGui::Separator();
        if (scene_ && !scene_->worldInstancePreviewStatus().empty())
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
                "%s",
                scene_->worldInstancePreviewStatus().c_str());
            ImGui::Spacing();
        }

        bool edited = false;
        if (auto* position = std::get_if<lux::math::Position3d>(
                &instance.position))
        {
            double value[3]{position->x, position->y, position->z};
            if (ImGui::InputScalarN(
                    "Position", ImGuiDataType_Double, value, 3))
            {
                *position = {value[0], value[1], value[2]};
                edited = true;
            }
        }
        else if (auto* position = std::get_if<lux::math::Position2d>(
                      &instance.position))
        {
            double value[2]{position->x, position->y};
            if (ImGui::InputScalarN(
                    "Position", ImGuiDataType_Double, value, 2))
            {
                *position = {value[0], value[1]};
                edited = true;
            }
        }
        edited |= ImGui::DragFloat4(
            "Rotation (xyzw)", instance.rotation.data(), 0.005f);
        float scale[3]{
            instance.scale[0], instance.scale[1], instance.scale[2]};
        if (ImGui::DragFloat3("Scale", scale, 0.01f, 0.0001f, 100000.0f))
        {
            instance.scale = {scale[0], scale[1], scale[2]};
            edited = true;
        }

        float color[4]{
            static_cast<float>((instance.rgba8 >> 24u) & 0xffu) / 255.0f,
            static_cast<float>((instance.rgba8 >> 16u) & 0xffu) / 255.0f,
            static_cast<float>((instance.rgba8 >> 8u) & 0xffu) / 255.0f,
            static_cast<float>(instance.rgba8 & 0xffu) / 255.0f};
        if (ImGui::ColorEdit4("Color", color))
        {
            const auto channel = [](float value)
            {
                return static_cast<std::uint32_t>(
                    std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            instance.rgba8 = channel(color[0]) << 24u |
                channel(color[1]) << 16u |
                channel(color[2]) << 8u |
                channel(color[3]);
            edited = true;
        }
        for (std::size_t index = 0u;
             index < instance.custom_values.size(); ++index)
        {
            const auto label = "Custom " + std::to_string(index);
            edited |= ImGui::DragFloat4(
                label.c_str(), instance.custom_values[index].data(), 0.01f);
        }

        const auto drawInstanceAsset = [&]<typename Predicate>(
            const char* label,
            lux::asset::asset_id_t& id,
            bool allow_clear,
            Predicate&& accepts)
        {
            std::string shown = id.is_nil()
                ? std::string{"(none)"}
                : uuids::to_string(id);
            if (registry_)
            {
                if (const auto* metadata = registry_->find(id))
                    shown = metadata->name;
            }
            ImGui::PushID(label);
            ImGui::TextUnformatted(label);
            ImGui::SameLine(110.0f);
            ImGui::Button(shown.c_str(), ImVec2(-30.0f, 0.0f));
            bool changed = false;
            if (ImGui::BeginDragDropTarget())
            {
                const auto* payload = ImGui::AcceptDragDropPayload(
                    lux::ui::kAssetDragPayloadTag);
                if (payload && payload->DataSize ==
                        static_cast<int>(sizeof(lux::ui::AssetDragPayload)))
                {
                    lux::ui::AssetDragPayload asset;
                    std::memcpy(&asset, payload->Data, sizeof(asset));
                    if (accepts(static_cast<lux::asset::EAssetType>(
                            asset.asset_type)))
                    {
                        std::array<std::uint8_t, 16> bytes{};
                        std::memcpy(
                            bytes.data(), asset.uuid_bytes, bytes.size());
                        id = lux::asset::asset_id_t{bytes};
                        changed = true;
                    }
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!allow_clear);
            if (ImGui::SmallButton("x") && allow_clear)
            {
                id = {};
                changed = true;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
            return changed;
        };
        edited |= drawInstanceAsset(
            "Mesh",
            instance.mesh,
            false,
            [](lux::asset::EAssetType type)
            {
                return type == lux::asset::EAssetType::MESH;
            });
        edited |= drawInstanceAsset(
            "Material",
            instance.material_instance,
            true,
            [](lux::asset::EAssetType type)
            {
                return type == lux::asset::EAssetType::MATERIAL ||
                    type == lux::asset::EAssetType::MATERIAL_INSTANCE;
            });
        if (edited && scene_)
            (void)scene_->updateWorldInstance(std::move(instance));

        ImGui::Separator();
        if (ImGui::Button("Duplicate") && scene_)
            (void)scene_->duplicateWorldInstance(instance_id);
        ImGui::SameLine();
        if (ImGui::Button("Delete") && scene_)
            (void)scene_->deleteWorldInstance(instance_id);
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Instances use a closed schema. Convert to Actor before adding "
            "scripts, dynamic physics, or arbitrary components.");
        if (ImGui::Button("Convert Instance to Actor") && scene_)
            (void)scene_->convertWorldInstanceToActor(instance_id);
        if (scene_ && !scene_->worldInstanceEditError().empty())
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                "%s",
                scene_->worldInstanceEditError().c_str());
        }
    }

    void InspectorPanel::paint()
    {
        if (selection_ && selection_->object())
        {
            if (const auto* instance = std::get_if<
                    lux::authoring::WorldInstanceId>(
                        &*selection_->object()))
            {
                paintWorldInstance(*instance);
                return;
            }
        }
        // Immediate-mode: read the SCENE'S selection each frame and validate —
        // null between scenes (C11); a swap can leave a stale id for one frame.
        lux::ecs::RegistryBase* reg =
            selection_ ? selection_->registry() : nullptr;
        const entt::entity e   = selection_ ? selection_->entity() : entt::null;
        if (!reg || e == entt::null || !reg->valid(e))
        {
            ImGui::TextDisabled("No entity selected.");
            ImGui::Spacing();
            ImGui::TextWrapped("Hold Alt to enter pick mode, then left-click a mesh.");
            return;
        }

        ImGui::Text("Entity  #%u", static_cast<uint32_t>(e));
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120.f);
        if (ImGui::Button("Add Component..."))
            ImGui::OpenPopup("##add_component");
        drawAddComponentMenu(*reg, e);
        if (scene_ && selection_->object())
        {
            if (const auto* actor = std::get_if<lux::authoring::WorldActorId>(
                    &*selection_->object()))
            {
                if (ImGui::Button("Convert Actor to Instance"))
                {
                    if (scene_->convertWorldActorToInstance(*actor))
                        return;
                }
                if (!scene_->worldInstanceEditError().empty())
                {
                    ImGui::SameLine();
                    ImGui::TextColored(
                        ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                        "%s",
                        scene_->worldInstanceEditError().c_str());
                }
            }
        }
        ImGui::Separator();

        auto entries = components_.all();
        for (const auto& ce : entries)
        {
            if (!ce.operations.has || !ce.has(*reg, e))
                continue;

            const std::string label = displayLabelFor(ce);
            const bool open = ImGui::CollapsingHeader(label.c_str(),
                                                     ImGuiTreeNodeFlags_DefaultOpen);

            // Right-click the header → Remove Component. Removal
            // happens BEFORE any field draw below, so nothing dereferences the
            // freed component this frame.
            bool removed = false;
            if (ce.operations.remove && ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Remove Component"))
                {
                    ce.operations.remove(*reg, e);
                    removed = true;
                }
                ImGui::EndPopup();
            }
            if (removed || !open) continue;

            const lux::meta::RefClass* rc = ce.ref_class;

            if (rc && !rc->fields.empty())
            {
                void* comp = ce.operations.get
                    ? ce.operations.get(*reg, e)
                    : nullptr;
                if (comp)
                {
                    // Resolver for asset-reference picks on THIS component: the
                    // search popup outlives the field draw, so it re-resolves
                    // the component base (entity + get fn captured by value)
                    // rather than holding a pointer that ECS storage may move.
                    auto comp_get = ce.operations.get;
                    current_comp_resolver_ = [this, comp_get, e]() -> void*
                    {
                        lux::ecs::RegistryBase* r =
                            selection_ ? selection_->registry() : nullptr;
                        if (!r || !comp_get || e == entt::null || !r->valid(e))
                            return nullptr;
                        return comp_get(*r, e);
                    };
                    // 与 resolver 同点重建:picker 落地后要发 notify,同样按
                    // 值捕获 + 落地时重验证(popup 存活期间场景可能换)。
                    auto comp_notify = ce.operations.notify;
                    current_comp_notify_ = [this, comp_notify, e]()
                    {
                        lux::ecs::RegistryBase* r =
                            selection_ ? selection_->registry() : nullptr;
                        if (r && comp_notify && e != entt::null && r->valid(e))
                            comp_notify(*r, e);
                    };

                    component_edited_ = false;
                    lux::ui::drawReflectedFieldTable(
                        label.c_str(), *rc, comp, 0.35f,
                        [this](const lux::meta::RefField& field, void* base)
                        { displayField(field, base); });
                    // Write contract: an Inspector field poke bypasses entt's
                    // signals — fire the component's notify (patch<T> →
                    // on_update) so event-driven consumers observe the edit.
                    if (component_edited_ && ce.operations.notify)
                        ce.operations.notify(*reg, e);
                }
            }
            else
            {
                ImGui::TextDisabled(
                    "  [component present — no reflection data registered]");
            }
        }

        // The asset picker is a popup: draw it once per frame, OUTSIDE the
        // component tables (a popup opened from a "pick" button above).
        asset_picker_.draw();
    }

} // namespace lux::editor
