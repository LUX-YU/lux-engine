#include <lux/engine/editor/panels/InspectorPanel.hpp>
#include <lux/engine/editor/app/StateRegistry.hpp>
#include <lux/engine/editor/app/Selection.hpp>
#include <lux/engine/editor/AssetRegistry.hpp>

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/AssetVfs.hpp>       // vfs->pathOf for tooltips

#include <lux/engine/ui/AssetDragDrop.hpp>
#include <lux/engine/ui/ReflectedFieldTable.hpp>   // drawReflectedFieldTable scaffold
#include <lux/engine/gameplay/ComponentTypeRegistry.hpp>
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
        std::string displayLabelFor(const lux::gameplay::ComponentTypeEntry& ce)
        {
            if (ce.ref_class)
            {
                auto annot = ce.ref_class->annotations();
                if (auto dn = annot.get("display_name"))
                    return std::string(*dn);
            }

            std::string_view full = ce.full_name;
            if (auto pos = full.rfind("::"); pos != std::string_view::npos)
                full.remove_prefix(pos + 2);
            return std::string(full);
        }

        // Case-insensitive substring test (ASCII) — used to infer the asset type
        // a uuid field expects from its conventional name.
        bool nameHas(std::string_view name, std::string_view needle) noexcept
        {
            if (needle.empty() || needle.size() > name.size()) return false;
            for (std::size_t i = 0; i + needle.size() <= name.size(); ++i)
            {
                bool m = true;
                for (std::size_t j = 0; j < needle.size(); ++j)
                {
                    char a = name[i + j];
                    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
                    if (a != needle[j]) { m = false; break; }
                }
                if (m) return true;
            }
            return false;
        }

        // lux has no typed asset handles (every reference is a bare asset_id_t);
        // infer the acceptable asset type(s) for a uuid field from its name.
        // Writes up to 4 types and returns the count; 0 == "accepts any asset".
        int expectedTypes(std::string_view name,
                          lux::asset::EAssetType (&out)[4]) noexcept
        {
            using lux::asset::EAssetType;
            int n = 0;
            if (nameHas(name, "material"))
            {
                out[n++] = EAssetType::MATERIAL;
                out[n++] = EAssetType::MATERIAL_INSTANCE;  // assignable; bridge resolves to its parent's PSO
            }
            else if (nameHas(name, "mesh") || nameHas(name, "model"))
            {
                out[n++] = EAssetType::MODEL;
                out[n++] = EAssetType::MESH;
            }
            else if (nameHas(name, "texture"))
            {
                out[n++] = EAssetType::TEXTURE;
            }
            return n; // 0 → any
        }

        bool fieldAcceptsType(std::string_view name,
                              lux::asset::EAssetType t) noexcept
        {
            lux::asset::EAssetType acc[4];
            const int n = expectedTypes(name, acc);
            if (n == 0) return true; // untyped field accepts any asset
            for (int i = 0; i < n; ++i)
                if (acc[i] == t) return true;
            return false;
        }
    } // namespace

    InspectorPanel::InspectorPanel(std::string title, StateRegistry& states)
        : Panel(std::move(title), {360.f, 600.f}),
          selection_(states.ensure<Selection>())
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
                *reinterpret_cast<lux::asset::asset_id_t*>(
                    static_cast<std::uint8_t*>(base) + pick_offset_) =
                        picker_candidates_[idx];
        });
    }

    // -------------------------------------------------------------------------
    // displayField — asset-reference fields first, then WidgetDispatch, else hex
    // -------------------------------------------------------------------------
    void InspectorPanel::displayField(const lux::meta::RefField& field,
                                       void* base)
    {
        if (field.visibility != lux::meta::EVisibility::Public)
            return;

        // An asset_id_t (uuids::uuid) field renders as a named, type-validated
        // picker rather than a hex id (material / mesh / texture references).
        if (drawAssetField(field, base))
            return;

        if (dispatch_.draw(field, base))
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
            bool ok = peek && peek->IsDataType(lux::ui::kAssetDragPayloadTag) &&
                      peek->DataSize ==
                          static_cast<int>(sizeof(lux::ui::AssetDragPayload));
            lux::ui::AssetDragPayload payload{};
            if (ok)
            {
                std::memcpy(&payload, peek->Data, sizeof(payload));
                ok = fieldAcceptsType(field.name,
                        static_cast<lux::asset::EAssetType>(payload.asset_type));
            }
            if (ok && ImGui::AcceptDragDropPayload(lux::ui::kAssetDragPayloadTag))
            {
                std::array<std::uint8_t, 16> ub{};
                std::memcpy(ub.data(), payload.uuid_bytes, ub.size());
                *id_ptr = lux::asset::asset_id_t(ub);
            }
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
            *id_ptr = lux::asset::asset_id_t{};
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
                if (fieldAcceptsType(field.name, m.type))
                {
                    picker_candidates_.push_back(m.id);
                    names.push_back(m.name);
                }
            }
        }
        asset_picker_.setItems(names);
        pick_offset_   = field.offset;
        pick_resolver_ = current_comp_resolver_; // re-resolves the base on select
        asset_picker_.open();
    }

    // -------------------------------------------------------------------------
    // paint — auto-discovers components from ComponentTypeRegistry
    // -------------------------------------------------------------------------
    void InspectorPanel::paint()
    {
        // Immediate-mode: read the shared selection each frame and validate —
        // a scene swap can leave a stale entity id for a single frame.
        entt::registry*    reg = selection_->registry();
        const entt::entity e   = selection_->entity();
        if (!reg || e == entt::null || !reg->valid(e))
        {
            ImGui::TextDisabled("No entity selected.");
            ImGui::Spacing();
            ImGui::TextWrapped("Hold Alt to enter pick mode, then left-click a mesh.");
            return;
        }

        ImGui::Text("Entity  #%u", static_cast<uint32_t>(e));
        ImGui::Separator();

        auto entries = lux::gameplay::ComponentTypeRegistry::instance().all();
        for (const auto& ce : entries)
        {
            if (!ce.has || !ce.has(*reg, e))
                continue;

            const std::string label = displayLabelFor(ce);
            const bool open = ImGui::CollapsingHeader(label.c_str(),
                                                     ImGuiTreeNodeFlags_DefaultOpen);
            if (!open) continue;

            const lux::meta::RefClass* rc = ce.ref_class;

            if (rc && !rc->fields.empty())
            {
                void* comp = ce.get ? ce.get(*reg, e) : nullptr;
                if (comp)
                {
                    // Resolver for asset-reference picks on THIS component: the
                    // search popup outlives the field draw, so it re-resolves
                    // the component base (entity + get fn captured by value)
                    // rather than holding a pointer that ECS storage may move.
                    auto comp_get = ce.get;
                    current_comp_resolver_ = [this, comp_get, e]() -> void*
                    {
                        entt::registry* r = selection_ ? selection_->registry() : nullptr;
                        if (!r || !comp_get || e == entt::null || !r->valid(e))
                            return nullptr;
                        return comp_get(*r, e);
                    };

                    lux::ui::drawReflectedFieldTable(
                        label.c_str(), *rc, comp, 0.35f,
                        [this](const lux::meta::RefField& field, void* base)
                        { displayField(field, base); });
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
