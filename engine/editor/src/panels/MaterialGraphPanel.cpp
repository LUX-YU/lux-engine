#include "panels/MaterialGraphPanel.hpp"
#include <lux/engine/toolchain/asset/material/MaterialGraphCompiler.hpp>
#include <lux/engine/authoring/assets/MaterialGraphDocument.hpp>

#include <lux/engine/authoring/assets/material/Node.hpp>
#include <lux/engine/authoring/assets/material/Nodes.hpp>
#include <lux/engine/toolchain/asset/material/MaterialLowering.hpp>
#include <lux/engine/toolchain/shader/Backend.hpp>
#include <lux/engine/editor/content/RenderShadersPath.hpp>
#include <lux/engine/description/MaterialGraphContract.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include "thumbnail/MaterialPreviewHost.hpp"
#include <lux/engine/function/render/client/resources/material/GraphMaterialData.hpp>
#include <lux/engine/platform/FormatCompat.h>   // lux::format — impl-only (kept out of the header)
#include <lux/engine/editor/AssetRegistry.hpp>
#include <lux/engine/editor/app/EditorEvents.hpp>
#include <lux/engine/events/DomainEvents.hpp>
#include <lux/engine/editor/thumbnail/ThumbnailService.hpp>   // 原地保存后作废旧缩略图
#include <lux/engine/resource/asset/MaterialAsset.hpp>
#include <lux/engine/resource/asset/MaterialSerDeser.hpp>
#include <lux/engine/resource/asset/MaterialInstanceSerDeser.hpp>   // 实例模式 Save
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/ui/AssetDragDrop.hpp>                 // 实例模式贴图槽拖放

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace rdesc = lux::rdesc;
namespace gk    = lux::graphkit;
namespace sgm   = lux::shadergen::material;
namespace sgg   = lux::shadergen::glsl;
using namespace lux::rdesc;

namespace
{
    // Domain tag for opaque pin-type tokens — material categories can never
    // alias another domain's in a shared styling table.
    constexpr std::uint32_t kMatDomainTag = static_cast<std::uint32_t>('M') << 24;

    // Snapshot the graph's declared param values (ParamSlotDecl.dflt) into the
    // flat GPU material blob: slot i -> params[i]. This is the LIVE-editable
    // material state — re-uploading it via modifyGraphMaterial updates the
    // preview with NO shader recompile (params are set-4 uniforms, not baked
    // literals like Constant nodes).
    lux::render::GraphMaterialData buildGraphParams(const MaterialGraph& g)
    {
        lux::render::GraphMaterialData d{};
        uint32_t n = static_cast<uint32_t>(g.param_slots.size());
        if (n > lux::render::GraphMaterialData::kMaxParams)
            n = lux::render::GraphMaterialData::kMaxParams;
        d.param_count = n;
        for (uint32_t i = 0; i < n; ++i)
        {
            d.params[i][0] = g.param_slots[i].dflt[0];
            d.params[i][1] = g.param_slots[i].dflt[1];
            d.params[i][2] = g.param_slots[i].dflt[2];
            d.params[i][3] = g.param_slots[i].dflt[3];
        }
        return d;
    }

    // EMatValueType -> GLSL-ish type name, for pin labels ("uv : vec2") and the
    // type selectors.
    const char* valueTypeName(rdesc::EMatValueType t)
    {
        switch (t)
        {
        case rdesc::EMatValueType::Float: return "float";
        case rdesc::EMatValueType::Vec2:  return "vec2";
        case rdesc::EMatValueType::Vec3:  return "vec3";
        default:                          return "vec4";
        }
    }

    // EMathOp -> human label. Every MathNode otherwise reports the generic name
    // "Math", hiding which operator it is on the canvas — surface it.
    const char* mathOpName(rdesc::EMathOp op)
    {
        using E = rdesc::EMathOp;
        switch (op)
        {
        case E::Mul:       return "Multiply";
        case E::Add:       return "Add";
        case E::Sub:       return "Subtract";
        case E::Div:       return "Divide";
        case E::Dot:       return "Dot";
        case E::Min:       return "Min";
        case E::Max:       return "Max";
        case E::Pow:       return "Pow";
        case E::Step:      return "Step";
        case E::Mod:       return "Mod";
        case E::Cross:     return "Cross";
        case E::Reflect:   return "Reflect";
        case E::Lerp:      return "Lerp";
        case E::Saturate:  return "Saturate";
        case E::OneMinus:  return "OneMinus";
        case E::Abs:       return "Abs";
        case E::Sqrt:      return "Sqrt";
        case E::Floor:     return "Floor";
        case E::Fract:     return "Fract";
        case E::Sin:       return "Sin";
        case E::Cos:       return "Cos";
        case E::Normalize: return "Normalize";
        case E::Length:    return "Length";
        }
        return "Math";
    }

    // Mirrors the lowering's implicit conversions (operandValue): exact, truncate
    // (larger -> smaller vector, e.g. a vec4 texture -> vec3 base_color = .rgb), or
    // scalar splat (float -> vecN). Widening a real vector needs a Construct node.
    bool isConvertibleType(rdesc::EMatValueType src, rdesc::EMatValueType dst)
    {
        if (src == dst) return true;
        const int as = static_cast<int>(src) + 1;
        const int ad = static_cast<int>(dst) + 1;
        if (as > ad) return true;                                       // truncate
        if (src == rdesc::EMatValueType::Float && ad > 1) return true;  // splat
        return false;
    }

    // Compact segmented type selector (F|V2|V3|V4) — a direct pick, no popup, so
    // it works reliably INSIDE the imgui-node-editor canvas (ImGui::Combo's popup
    // misbehaves there). Returns true + writes *out when a different type is picked.
    bool drawTypeSelector(rdesc::EMatValueType cur, rdesc::EMatValueType* out,
                          const char* label = "type:")
    {
        static const char* const kShort[4] = { "F", "V2", "V3", "V4" };
        bool changed = false;
        ImGui::TextUnformatted(label);
        for (int t = 0; t < 4; ++t)
        {
            ImGui::SameLine(0.0f, 2.0f);
            const bool sel = (static_cast<int>(cur) == t);
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.50f, 0.80f, 1.0f));
            if (ImGui::SmallButton(kShort[t]) && !sel)
            {
                *out    = static_cast<rdesc::EMatValueType>(t);
                changed = true;
            }
            if (sel) ImGui::PopStyleColor();
        }
        return changed;
    }

    // A param named like emissive may exceed 1.0 -> the picker uses HDR mode.
    bool nameLooksLikeEmissive(const std::string& n)
    {
        return n.find("emiss") != std::string::npos;
    }

    // Header tint per node kind (the schema's nodeStyle table; the GraphKit
    // chrome forces the band opaque and modulates by the global alpha).
    std::uint32_t headerColorFor(EMatNodeKind k)
    {
        switch (k)
        {
        case EMatNodeKind::Constant:      return IM_COL32( 90,  90,  95, 255);
        case EMatNodeKind::Param:         return IM_COL32( 40, 120, 200, 255);
        case EMatNodeKind::Input:         return IM_COL32( 60, 160, 110, 255);
        case EMatNodeKind::SampleTexture: return IM_COL32(200, 130,  40, 255);
        case EMatNodeKind::Math:          return IM_COL32(150,  70, 170, 255);
        case EMatNodeKind::Swizzle:       return IM_COL32(110,  90, 180, 255);
        case EMatNodeKind::Construct:     return IM_COL32( 90, 110, 190, 255);
        case EMatNodeKind::DecodeNormal:  return IM_COL32( 70, 140, 160, 255);
        case EMatNodeKind::TbnTransform:  return IM_COL32( 60, 150, 150, 255);
        case EMatNodeKind::OutputSurface: return IM_COL32(180,  50,  60, 255);
        default:                          return IM_COL32(100, 100, 100, 255);
        }
    }

    // Pin dot color per value type (same color = compatible, matching the
    // exact-match connect rule).
    std::uint32_t pinColorFor(rdesc::EMatValueType t)
    {
        switch (t)
        {
        case rdesc::EMatValueType::Float: return IM_COL32(160, 220, 100, 255);
        case rdesc::EMatValueType::Vec2:  return IM_COL32(110, 200, 230, 255);
        case rdesc::EMatValueType::Vec3:  return IM_COL32(250, 220,  90, 255);
        default:                          return IM_COL32(230, 130, 200, 255);
        }
    }
}

namespace lux::editor
{
    // =========================================================================
    //  MaterialGraphView — IGraphView over the borrowed rdesc::MaterialGraph
    // =========================================================================

    void MaterialGraphView::forEachNode(
        const std::function<void(gk::GraphNodeRef, std::string_view)>& fn) const
    {
        for (const auto& [id, n] : graph_->nodes())
        {
            if (!n) continue;
            // MathNode::name() is the generic "Math"; append the operator so the
            // canvas header reads e.g. "Math: Multiply" instead of a bare "Math".
            if (const auto* mn = n->as<rdesc::MathNode>())
            {
                const std::string t = std::string("Math: ") + mathOpName(mn->op);
                fn(gk::GraphNodeRef{ id }, t);   // t outlives the synchronous fn call
            }
            else
                fn(gk::GraphNodeRef{ id }, n->name());
        }
    }

    std::uint32_t MaterialGraphView::pinCount(gk::GraphNodeRef node,
                                              gk::EPinSide side) const
    {
        const rdesc::Node* n = graph_->node(node.id);
        if (!n) return 0;
        return static_cast<std::uint32_t>(
            side == gk::EPinSide::INPUT ? n->inputs().size() : n->outputs().size());
    }

    gk::GraphPinView MaterialGraphView::pin(gk::GraphNodeRef node,
                                            gk::EPinSide side,
                                            std::uint32_t index) const
    {
        gk::GraphPinView out;
        const rdesc::Node* n = graph_->node(node.id);
        if (!n) return out;
        const auto& pins =
            (side == gk::EPinSide::INPUT) ? n->inputs() : n->outputs();
        if (index >= pins.size()) return out;
        const rdesc::DataPin& p = pins[index];

        label_scratch_ = lux::format("{} : {}", p.name, valueTypeName(p.type));
        out.name = label_scratch_;
        out.type = gk::GraphPinType{
            kMatDomainTag | static_cast<std::uint32_t>(p.type),
            side,
            side == gk::EPinSide::INPUT ? std::uint8_t{ 1 } : gk::kFanUnlimited,
            false };
        // Outputs draw always-filled (parity with the old builder — scanning
        // every input of every node per output would be O(N^2) per frame).
        out.has_link = (side == gk::EPinSide::INPUT) ? p.source.valid() : true;
        return out;
    }

    void MaterialGraphView::forEachLink(
        const std::function<void(gk::GraphLinkView)>& fn) const
    {
        for (const auto& [id, n] : graph_->nodes())
        {
            if (!n) continue;
            const auto& ins = n->inputs();
            for (std::uint32_t i = 0; i < ins.size(); ++i)
            {
                if (!ins[i].source.valid()) continue;
                fn(gk::GraphLinkView{
                    gk::GraphPinRef{ gk::GraphNodeRef{ ins[i].source.node },
                                     gk::EPinSide::OUTPUT, ins[i].source.pin },
                    gk::GraphPinRef{ gk::GraphNodeRef{ id },
                                     gk::EPinSide::INPUT, i } });
            }
        }
    }

    std::optional<gk::GraphVec2> MaterialGraphView::nodePos(gk::GraphNodeRef node) const
    {
        const rdesc::Node* n = graph_->node(node.id);
        if (!n || !n->ui_placed) return std::nullopt;
        return gk::GraphVec2{ n->ui_pos[0], n->ui_pos[1] };
    }

    void MaterialGraphView::setNodePos(gk::GraphNodeRef node, gk::GraphVec2 pos)
    {
        rdesc::Node* n = graph_->node(node.id);
        if (!n || !std::isfinite(pos.x) || !std::isfinite(pos.y)) return;
        n->ui_pos[0] = pos.x;
        n->ui_pos[1] = pos.y;
        n->ui_placed = true;
    }

    gk::GraphNodeRef MaterialGraphView::addNode(std::string_view template_id)
    {
        MaterialGraph& g = *graph_;
        node_id id = invalid_node;

        if (template_id == "constant")
        {
            auto n = std::make_unique<ConstantNode>();
            n->setType(rdesc::EMatValueType::Vec3);   // arity via the in-node selector
            n->value[0] = n->value[1] = n->value[2] = n->value[3] = 0.5f;
            id = g.addNode(std::move(n));
        }
        else if (template_id == "param")
        {
            const uint32_t slot = static_cast<uint32_t>(g.param_slots.size());
            g.param_slots.push_back(
                ParamSlotDecl{ lux::format("param{}", slot), rdesc::EMatValueType::Vec3 });
            auto n = std::make_unique<ParamNode>(rdesc::EMatValueType::Vec3);
            n->param_slot = slot;
            id = g.addNode(std::move(n));
        }
        else if (template_id == "input.uv")
        {
            auto n   = std::make_unique<InputNode>();
            n->input = rdesc::EMaterialInput::UV0;
            id = g.addNode(std::move(n));
        }
        else if (template_id == "input.normal")
        {
            auto n   = std::make_unique<InputNode>();
            n->input = rdesc::EMaterialInput::WorldNormal;
            id = g.addNode(std::move(n));
        }
        else if (template_id == "math.mul")
        {
            id = g.addNode(std::make_unique<MathNode>(EMathOp::Mul));
        }
        else if (template_id == "math.add")
        {
            id = g.addNode(std::make_unique<MathNode>(EMathOp::Add));
        }
        else if (template_id == "construct")
        {
            id = g.addNode(std::make_unique<ConstructNode>());
        }
        else if (template_id == "swizzle")
        {
            id = g.addNode(std::make_unique<SwizzleNode>());
        }
        else if (template_id == "decode_normal")
        {
            id = g.addNode(std::make_unique<DecodeNormalNode>());
        }
        else if (template_id == "tbn")
        {
            id = g.addNode(std::make_unique<TbnTransformNode>());
        }
        else if (template_id == "sample_texture")
        {
            const uint32_t slot = static_cast<uint32_t>(g.texture_slots.size());
            g.texture_slots.push_back(TextureSlotDecl{ lux::format("tex{}", slot) });
            auto n          = std::make_unique<SampleTextureNode>();
            n->texture_slot = slot;
            id = g.addNode(std::move(n));
        }

        return gk::GraphNodeRef{ id == invalid_node ? gk::kInvalidNode : id };
    }

    gk::NodeCapture MaterialGraphView::detachNode(gk::GraphNodeRef node)
    {
        auto holder = std::make_shared<std::unique_ptr<rdesc::Node>>(
            graph_->extractNode(node.id));
        if (!*holder) return nullptr;
        return holder;   // shared_ptr<void> type-erases the move-only payload
    }

    bool MaterialGraphView::attachNode(gk::GraphNodeRef original, gk::NodeCapture capture)
    {
        auto holder = std::static_pointer_cast<std::unique_ptr<rdesc::Node>>(capture);
        if (!holder || !*holder) return false;
        return graph_->addNodeWithId(original.id, std::move(*holder)) != invalid_node;
    }

    bool MaterialGraphView::connect(gk::GraphPinRef from, gk::GraphPinRef to)
    {
        rdesc::Node* d = graph_->node(to.node.id);
        if (!d || to.pin >= d->inputs().size()) return false;
        // Port contract (b): never implicitly sever — the command stack
        // pre-disconnects cap-1 pins itself (undoable replace-on-reconnect).
        if (d->inputs()[to.pin].source.valid()) return false;
        return graph_->connect(from.node.id, from.pin, to.node.id, to.pin);
    }

    bool MaterialGraphView::disconnect(gk::GraphPinRef from, gk::GraphPinRef to)
    {
        rdesc::Node* d = graph_->node(to.node.id);
        if (!d || to.pin >= d->inputs().size()) return false;
        const rdesc::PinLink& src = d->inputs()[to.pin].source;
        if (!src.valid() || src.node != from.node.id || src.pin != from.pin)
            return false;   // not THIS link — refuse to clear someone else's
        graph_->disconnect(to.node.id, to.pin);
        return true;
    }

    void MaterialGraphView::reconstructNode(gk::GraphNodeRef /*node*/)
    {
        // Material node kinds have fixed pin COUNTS today (type selectors only
        // retype pins, which the per-frame read absorbs) — nothing to rebuild.
    }

    // =========================================================================
    //  MaterialGraphSchema — connection rules / palette / styling / body editors
    // =========================================================================

    gk::ConnectResult MaterialGraphSchema::canConnect(gk::GraphPinRef from,
                                                      gk::GraphPinRef to,
                                                      const gk::IGraphView&) const
    {
        if (from.node == to.node)
            return gk::ConnectResult::no("can't connect a node to itself");

        const rdesc::Node* s = graph_->node(from.node.id);
        const rdesc::Node* d = graph_->node(to.node.id);
        if (!s || !d
            || from.pin >= s->outputs().size()
            || to.pin >= d->inputs().size())
        {
            return gk::ConnectResult::no("can't link");
        }

        // Connect-time type rule: exact / truncate / splat (the lowering's
        // implicit conversions); widening needs a Construct node.
        const auto st = s->outputs()[from.pin].type;
        const auto dt = d->inputs()[to.pin].type;
        if (!isConvertibleType(st, dt))
        {
            return gk::ConnectResult::no(lux::format(
                "can't widen {} -> {}\ninsert a Construct node",
                valueTypeName(st), valueTypeName(dt)));
        }
        return gk::ConnectResult::yes();
    }

    std::span<const gk::NodeTemplate> MaterialGraphSchema::palette() const
    {
        // Closed-enum domain: a fixed table (string ids consumed by
        // MaterialGraphView::addNode).
        static const std::vector<gk::NodeTemplate> kTable = {
            { "constant",       "Constant",       "Value"   },
            { "param",          "Param",          "Value"   },
            { "input.uv",       "Input: UV",      "Input"   },
            { "input.normal",   "Input: Normal",  "Input"   },
            { "math.mul",       "Math: Multiply", "Math"    },
            { "math.add",       "Math: Add",      "Math"    },
            { "construct",      "Construct",      "Vector"  },
            { "swizzle",        "Swizzle",        "Vector"  },
            { "decode_normal",  "Decode Normal",  "Normal"  },
            { "tbn",            "TBN Transform",  "Normal"  },
            { "sample_texture", "Sample Texture", "Texture" },
        };
        return kTable;
    }

    gk::PinStyleDesc MaterialGraphSchema::pinStyle(const gk::GraphPinType& type) const
    {
        return gk::PinStyleDesc{
            pinColorFor(static_cast<rdesc::EMatValueType>(type.category & 0xFFu)) };
    }

    gk::NodeStyleDesc MaterialGraphSchema::nodeStyle(gk::GraphNodeRef node,
                                                     const gk::IGraphView&) const
    {
        if (const rdesc::Node* n = graph_->node(node.id))
            return gk::NodeStyleDesc{ headerColorFor(n->kind()), true };
        return {};
    }

    void MaterialGraphSchema::drawNodeBody(gk::GraphNodeRef node, gk::IGraphView&,
                                           gk::DeferredPopupQueue& popups)
    {
        rdesc::Node* raw = graph_->node(node.id);
        if (!raw) return;

        // ── R3 实例模式:拓扑与烘焙量属于根图,节点体转 override 编辑面 ──
        if (instance_mode_)
        {
            // Constant / Swizzle / 类型:只读展示(烘焙进 SPIR-V,实例不可改)。
            if (auto* c = raw->as<ConstantNode>())
            {
                const int comps = static_cast<int>(c->value_type) + 1;
                std::string t = "= ";
                for (int k = 0; k < comps; ++k)
                    t += lux::format("{}{:.3g}", k ? ", " : "", c->value[k]);
                ImGui::TextDisabled("%s", t.c_str());
            }
            if (auto* p = raw->as<ParamNode>())
            {
                const std::uint32_t lane = p->param_slot;
                if (inst_hooks_.param_overridden && inst_hooks_.param_effective)
                {
                    bool ov = inst_hooks_.param_overridden(lane);
                    if (ImGui::Checkbox("##ov", &ov) && inst_hooks_.param_toggle)
                        inst_hooks_.param_toggle(lane, ov);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("override(勾上从父有效值起步)");
                    ImGui::SameLine();
                    float v[4]{};
                    inst_hooks_.param_effective(lane, v);
                    const int comps = static_cast<int>(p->type) + 1;
                    ImGui::PushItemWidth(58.0f * static_cast<float>(comps));
                    ImGui::BeginDisabled(!ov);
                    bool edited = false;
                    switch (comps)
                    {
                    case 1:  edited = ImGui::DragFloat ("##pval", v, 0.01f); break;
                    case 2:  edited = ImGui::DragFloat2("##pval", v, 0.01f); break;
                    case 3:  edited = ImGui::DragFloat3("##pval", v, 0.01f); break;
                    default: edited = ImGui::DragFloat4("##pval", v, 0.01f); break;
                    }
                    ImGui::EndDisabled();
                    ImGui::PopItemWidth();
                    if (edited && ov && inst_hooks_.param_set)
                        inst_hooks_.param_set(lane, v);
                    if (comps >= 3)
                    {
                        ImGui::SameLine();
                        const ImVec4 sw(v[0], v[1], v[2], 1.0f);
                        if (ImGui::ColorButton("##pswatch", sw,
                                ImGuiColorEditFlags_NoTooltip |
                                ImGuiColorEditFlags_NoDragDrop))
                        {
                            if (!ov && inst_hooks_.param_toggle)
                                inst_hooks_.param_toggle(lane, true);   // 点色板即开 override
                            popups.push(gk::DeferredPopupRequest{ node, "color_picker", 0 });
                        }
                    }
                }
            }
            if (auto* st = raw->as<SampleTextureNode>())
            {
                const std::uint32_t slot = st->texture_slot;
                bool ov = inst_hooks_.tex_overridden && inst_hooks_.tex_overridden(slot);
                if (ImGui::Checkbox("##tov", &ov) && inst_hooks_.tex_toggle)
                    inst_hooks_.tex_toggle(slot, ov);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("override(勾上从父绑定起步)");
                ImGui::SameLine();
                const std::string bound =
                    bound_texture_name_ ? bound_texture_name_(slot) : std::string{};
                ImGui::TextUnformatted(bound.empty() ? "(none)" : bound.c_str());
                ImGui::SameLine();
                ImGui::BeginDisabled(!ov);
                if (ImGui::SmallButton("pick##tex"))
                    popups.push(gk::DeferredPopupRequest{
                        node, "texture_picker", slot });
                ImGui::SameLine();
                if (ImGui::SmallButton("x##texclr") && inst_hooks_.tex_clear)
                    inst_hooks_.tex_clear(slot);   // override + nil = 显式清槽
                ImGui::EndDisabled();
            }
            if (auto* sw = raw->as<SwizzleNode>())
            {
                static const char* const kComp[4] = { "x", "y", "z", "w" };
                const int outN = static_cast<int>(sw->out_type) + 1;
                std::string picks;
                for (int k = 0; k < outN; ++k)
                    picks += kComp[sw->components[k] & 3];
                ImGui::TextDisabled("pick: %s", picks.c_str());
            }
            return;   // 实例模式不落任何 structure/param 直写
        }

        bool structure_dirty = false;   // baked-shader edits -> recompile

        // Constant body editor (baked literal -> recompile on edit).
        if (auto* c = raw->as<ConstantNode>())
        {
            rdesc::EMatValueType nt;
            if (drawTypeSelector(c->value_type, &nt)) { c->setType(nt); structure_dirty = true; }
            const int comps = static_cast<int>(c->value_type) + 1;
            ImGui::PushItemWidth(58.0f * static_cast<float>(comps));
            switch (comps)
            {
            case 1:  ImGui::DragFloat ("##cval", c->value, 0.01f); break;
            case 2:  ImGui::DragFloat2("##cval", c->value, 0.01f); break;
            case 3:  ImGui::DragFloat3("##cval", c->value, 0.01f); break;
            default: ImGui::DragFloat4("##cval", c->value, 0.01f); break;
            }
            ImGui::PopItemWidth();
            if (ImGui::IsItemDeactivatedAfterEdit()) structure_dirty = true;
        }

        // Param body editor (set-4 uniform -> live preview update while
        // dragging; vec3/vec4 params get a swatch -> deferred color picker).
        if (auto* p = raw->as<ParamNode>())
        {
            if (p->param_slot < graph_->param_slots.size())
            {
                ParamSlotDecl& slot = graph_->param_slots[p->param_slot];
                rdesc::EMatValueType nt;
                if (drawTypeSelector(p->type, &nt))
                {
                    p->setType(nt); slot.type = p->type; structure_dirty = true;
                }
                const int comps  = static_cast<int>(slot.type) + 1;
                bool      edited = false;
                ImGui::PushItemWidth(58.0f * static_cast<float>(comps));
                switch (comps)
                {
                case 1:  edited = ImGui::DragFloat ("##pval", slot.dflt, 0.01f); break;
                case 2:  edited = ImGui::DragFloat2("##pval", slot.dflt, 0.01f); break;
                case 3:  edited = ImGui::DragFloat3("##pval", slot.dflt, 0.01f); break;
                default: edited = ImGui::DragFloat4("##pval", slot.dflt, 0.01f); break;
                }
                ImGui::PopItemWidth();
                // A color-picker popup can't receive input inside the canvas —
                // raise a deferred request; the panel draws the floating picker.
                if (comps >= 3)
                {
                    ImGui::SameLine();
                    const ImVec4 sw(slot.dflt[0], slot.dflt[1], slot.dflt[2], 1.0f);
                    if (ImGui::ColorButton("##pswatch", sw,
                            ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop))
                        popups.push(gk::DeferredPopupRequest{ node, "color_picker", 0 });
                }
                if (edited && on_params_dirty_)
                    on_params_dirty_();
            }
        }

        // SampleTexture body: bound-name + the deferred fuzzy texture picker.
        if (auto* st = raw->as<SampleTextureNode>())
        {
            const std::string bound =
                bound_texture_name_ ? bound_texture_name_(st->texture_slot) : std::string{};
            ImGui::TextUnformatted("tex:");
            ImGui::SameLine();
            ImGui::TextUnformatted(bound.empty() ? "(none)" : bound.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("pick##tex"))
                popups.push(gk::DeferredPopupRequest{
                    node, "texture_picker", st->texture_slot });
        }

        // Swizzle body: in/out arity + per-output source-component cyclers.
        if (auto* sw = raw->as<SwizzleNode>())
        {
            rdesc::EMatValueType nt;
            ImGui::PushID("swin");
            if (drawTypeSelector(sw->source_type, &nt, "in :"))
            {
                sw->setTypes(nt, sw->out_type);
                structure_dirty = true;
            }
            ImGui::PopID();
            ImGui::PushID("swout");
            if (drawTypeSelector(sw->out_type, &nt, "out:"))
            {
                sw->setTypes(sw->source_type, nt);
                structure_dirty = true;
            }
            ImGui::PopID();

            const int srcN = static_cast<int>(sw->source_type) + 1;
            const int outN = static_cast<int>(sw->out_type) + 1;
            static const char* const kComp[4] = { "x", "y", "z", "w" };
            ImGui::TextUnformatted("pick:");
            for (int k = 0; k < outN; ++k)
            {
                ImGui::SameLine(0.0f, 2.0f);
                ImGui::PushID(k + 1000);
                const int cur = static_cast<int>(sw->components[k]) % srcN;
                if (ImGui::SmallButton(kComp[cur & 3]))
                {
                    sw->components[k] = static_cast<std::uint8_t>((cur + 1) % srcN);
                    structure_dirty = true;
                }
                ImGui::PopID();
            }
        }

        if (structure_dirty && on_structure_dirty_)
            on_structure_dirty_();
    }

    // =========================================================================
    //  MaterialGraphPanel — the host
    // =========================================================================

    MaterialGraphPanel::MaterialGraphPanel(std::string title)
        : Panel(std::move(title), { 900.f, 600.f })
    {
        // Body-editor callbacks: structural edits rebake; param drags stream
        // live; the texture name comes from the panel's slot bindings.
        schema_.setOnStructureDirty([this] { compile(); });
        schema_.setOnParamsDirty(
            [this]
            {
                if (preview_) preview_->updateGraphParams(buildGraphMaterial());
            });
        schema_.setBoundTextureName(
            [this](std::uint32_t slot) { return boundTextureName(slot); });

        // R3 实例模式的节点体编辑钩子:全部写 inst_edit_ 的 override lane +
        // 快路径推预览。语义与旧参数表逐条对齐(勾上从父有效值起步/清 override
        // 回父值/override+nil = 显式清槽)。
        schema_.setInstanceEditHooks({
            .param_overridden =
                [this](std::uint32_t l)
                { return (inst_edit_.param_override_mask & (1u << l)) != 0u; },
            .param_effective =
                [this](std::uint32_t l, float* out)
                {
                    const bool ov = (inst_edit_.param_override_mask & (1u << l)) != 0u;
                    std::memcpy(out, ov ? inst_edit_.params[l] : inst_parent_params_[l],
                                sizeof(float) * 4);
                },
            .param_toggle =
                [this](std::uint32_t l, bool on)
                {
                    if (on)
                    {
                        inst_edit_.param_override_mask |= (1u << l);
                        // 起步值 = 父有效值(勾上那一刻画面不跳)。
                        std::memcpy(inst_edit_.params[l], inst_parent_params_[l],
                                    sizeof(float) * 4);
                    }
                    else
                        inst_edit_.param_override_mask &= ~(1u << l);
                    instanceParamsChanged();
                },
            .param_set =
                [this](std::uint32_t l, const float* v)
                {
                    std::memcpy(inst_edit_.params[l], v, sizeof(float) * 4);
                    inst_edit_.param_override_mask |= (1u << l);
                    instanceParamsChanged();
                },
            .tex_overridden =
                [this](std::uint32_t s)
                { return (inst_edit_.tex_override_mask & (1u << s)) != 0u; },
            .tex_toggle =
                [this](std::uint32_t s, bool on)
                {
                    if (on)
                    {
                        inst_edit_.tex_override_mask |= (1u << s);
                        inst_edit_.texture_slot_ids[s] = inst_parent_tex_[s]; // 起步 = 父值
                    }
                    else
                        inst_edit_.tex_override_mask &= ~(1u << s);
                    instanceParamsChanged();
                },
            .tex_clear =
                [this](std::uint32_t s)
                {
                    inst_edit_.texture_slot_ids[s] = {};   // override + nil = 显式清槽
                    instanceParamsChanged();
                },
        });

        buildDefaultGraph();
        compile();
        std::cerr << "[MaterialGraphPanel] created; default graph compile -> " << status_ << "\n";
    }

    MaterialGraphPanel::~MaterialGraphPanel() = default;

    // A simple, valid starter graph: a PBR surface whose base color / metallic /
    // roughness are PARAMS (set-4 uniforms), so dragging their sliders updates
    // the preview LIVE via modifyGraphMaterial — no shader recompile. Normal
    // comes from the interpolated world normal. (Params also make the forward
    // frag consume vMatIndex@4, silencing the validation warning a pure-
    // Constant graph would trigger.)
    void MaterialGraphPanel::buildDefaultGraph()
    {
        graph_.param_slots.push_back(
            ParamSlotDecl{ "base_color", rdesc::EMatValueType::Vec3,  { 0.85f, 0.25f, 0.2f, 0.f } });
        graph_.param_slots.push_back(
            ParamSlotDecl{ "metallic",   rdesc::EMatValueType::Float, { 0.0f, 0.f, 0.f, 0.f } });
        graph_.param_slots.push_back(
            ParamSlotDecl{ "roughness",  rdesc::EMatValueType::Float, { 0.4f, 0.f, 0.f, 0.f } });

        auto baseN         = std::make_unique<ParamNode>(rdesc::EMatValueType::Vec3);
        baseN->param_slot  = 0;
        const node_id base = graph_.addNode(std::move(baseN));

        auto metalN         = std::make_unique<ParamNode>(rdesc::EMatValueType::Float);
        metalN->param_slot  = 1;
        const node_id metal = graph_.addNode(std::move(metalN));

        auto roughN         = std::make_unique<ParamNode>(rdesc::EMatValueType::Float);
        roughN->param_slot  = 2;
        const node_id rough = graph_.addNode(std::move(roughN));

        auto nrmN         = std::make_unique<InputNode>();
        nrmN->input       = rdesc::EMaterialInput::WorldNormal;
        const node_id nrm = graph_.addNode(std::move(nrmN));

        const node_id o = graph_.addNode(std::make_unique<OutputSurfaceNode>());
        graph_.connect(base,  0, o, static_cast<uint32_t>(rdesc::EMaterialAttribute::BaseColor));
        graph_.connect(metal, 0, o, static_cast<uint32_t>(rdesc::EMaterialAttribute::Metallic));
        graph_.connect(rough, 0, o, static_cast<uint32_t>(rdesc::EMaterialAttribute::Roughness));
        graph_.connect(nrm,   0, o, static_cast<uint32_t>(rdesc::EMaterialAttribute::NormalTS));

        // Sensible starter arrangement (positions persist with the graph).
        view_.setNodePos(gk::GraphNodeRef{ base },  gk::GraphVec2{  40.0f,  40.0f });
        view_.setNodePos(gk::GraphNodeRef{ metal }, gk::GraphVec2{  40.0f, 200.0f });
        view_.setNodePos(gk::GraphNodeRef{ rough }, gk::GraphVec2{  40.0f, 330.0f });
        view_.setNodePos(gk::GraphNodeRef{ nrm },   gk::GraphVec2{  40.0f, 460.0f });
        view_.setNodePos(gk::GraphNodeRef{ o },     gk::GraphVec2{ 380.0f, 160.0f });

        editor_.bind(&view_, &schema_);   // full reset (bimap, undo, selection)
        last_seen_revision_ = editor_.commands().structureRevision();

        // 默认图不对应任何盘上资产 —— 原地 Save 禁用,直到 openAsset /
        // saveAsAsset 重新钉上目标。默认图必然是图模式。
        open_asset_id_ = {};
        open_asset_path_.clear();
        mode_ = EEditMode::GRAPH;
        editor_.setTopologyLocked(false);   // R3:图模式解除拓扑锁
        schema_.setInstanceMode(false);
    }

    // 池上编译核(批H1):纯函数 —— 只读 job 快照与静态 include 目录常量,
    // 不摸面板/账本/registry/渲染录制(§8 总纪律)。原 compile() 正文整体
    // 迁入,双 pass 一次编齐(GBuffer 供显示与换刀,Forward 供预览 PSO)。
    std::shared_ptr<MaterialCompileOutcome>
    compileMaterialJob(const MaterialCompileJob& job)
    {
        auto out = std::make_shared<MaterialCompileOutcome>();
        auto lr  = sgm::lowerMaterial(job.graph);
        if (!lr)
        {
            out->status = lux::format("lower error: {}", lr.error());
            return out;
        }
        // ShaderGen #includes the real render SSOT — pass the render shaders dir as
        // the shaderc include search root (cmake-generated RenderShadersPath.hpp).
        const std::vector<std::string> inc = { lux::editor::render_shaders_emitted_dir,
                                               lux::editor::render_shaders_dir };
        const auto mkParams = [&](rdesc::EMaterialPass pass)
        {
            sgg::EmitParams p;
            p.pass          = pass;
            p.shading_model = lr->shading_model;
            p.alpha_mode    = lr->alpha_mode;
            p.alpha_cutoff  = lr->alpha_cutoff;
            return p;
        };
        auto g = sgg::emitGlsl(lr->ir, mkParams(rdesc::EMaterialPass::GBuffer));
        if (!g)
        {
            out->status = lux::format("emit error: {}", g.error());
            return out;
        }
        out->glsl = std::move(*g);
        auto cs = sgg::compileToSpirv(lr->ir, mkParams(rdesc::EMaterialPass::GBuffer), inc);
        if (!cs)
        {
            out->status = lux::format("spirv error: {}", cs.error());
            return out;
        }
        out->ok     = true;
        out->status = lux::format("OK: {} SPIR-V words / {} nodes",
                                  cs->spirv.size(), job.graph.nodes().size());

        // Preview receives cooked runtime data. The editable graph remains in
        // the job/authoring document and is never smuggled into MaterialData.
        auto fwd = sgg::compileToSpirv(lr->ir, mkParams(rdesc::EMaterialPass::Forward), inc);
        if (fwd)
        {
            auto payload = std::make_unique<lux::asset::MaterialData>();
            lux::toolchain::flattenMaterialRuntimeValues(job.graph, *payload);
            payload->gbuffer_spirv    = cs->spirv;
            payload->gbuffer_info     = cs->info;
            payload->forward_spirv    = fwd->spirv;
            payload->forward_info     = fwd->info;
            payload->texture_slot_ids = job.texture_slot_ids;
            out->payload = std::move(payload);
        }
        else
        {
            out->status += lux::format("  [preview fwd error: {}]", fwd.error());
        }
        return out;
    }

    void MaterialGraphPanel::compile()
    {
        MaterialCompileJob job;
        job.graph = graph_.clone();
        for (const auto& [slot, bind] : slot_texture_)
            if (slot < lux::asset::MaterialData::kMaxTextures)
                job.texture_slot_ids[slot] = bind.uuid;

        if (compile_dispatch_)
        {
            const std::uint64_t id = ++next_compile_id_;
            pending_compile_id_    = id;
            status_ = "compiling…";
            if (!compile_dispatch_(
                    id,
                    std::make_shared<const MaterialCompileJob>(std::move(job))))
            {
                pending_compile_id_ = 0;
                status_ = "material compile queue is stopping or full";
            }
            return;
        }
        // 无总线(ctor 默认图/测试):同步回落 —— 行为与批H1 之前一致。
        auto out = compileMaterialJob(job);
        applyCompileOutcome(*out);
    }

    void MaterialGraphPanel::onCompiled(
        std::uint64_t request_id,
        std::shared_ptr<MaterialCompileOutcome> outcome)
    {
        if (request_id != pending_compile_id_ || !outcome)
            return;   // 过期结果(已有更新的在途编译):丢弃
        pending_compile_id_ = 0;
        applyCompileOutcome(*outcome);
    }

    void MaterialGraphPanel::applyCompileOutcome(MaterialCompileOutcome& o)
    {
        glsl_   = std::move(o.glsl);
        status_ = std::move(o.status);

        // Feed the live preview:宿主把载荷注册成临时资产,球实体经标准
        // resolver 路径(ensureGraphMaterial,逐材质 forward PSO)换上。
        if (o.payload && preview_)
        {
            preview_->setGraphContent(std::move(o.payload));

            const lux::render::GraphMaterialData data = buildGraphMaterial();
            std::memcpy(last_tex_bindless_, data.tex_bindless, sizeof(last_tex_bindless_));
            // 参数通道也要在这里打开:updateGraphParams 是唯一置 params_valid
            // 的地方,不推的话「换刀就绪后补发参数」永远不触发 —— openAsset
            // 的首刀就停在图默认值(实测症状:打开材质只见灰球,拖任意参数
            // 滑条才第一次渲染正常)。data 上面刚算好,零额外成本。
            preview_->updateGraphParams(data);
        }
    }

    void MaterialGraphPanel::setPreviewHost(MaterialPreviewHost* preview)
    {
        preview_ = preview;
        if (!preview_) return;
        preview_element_.setTarget(preview_->target());
        preview_element_.setOrbitCallback(
            [p = preview_](float dy, float dp, float dz){ p->orbit(dy, dp, dz); });
        preview_element_.setResizeCallback(
            [p = preview_](std::uint32_t w, std::uint32_t h){ p->requestResize(w, h); });
        compile();   // push the current graph to the preview now
    }

    void MaterialGraphPanel::setAssetServices(AssetRegistry* registry,
                                              std::shared_ptr<lux::asset::AssetManager> manager,
                                              lux::events::DomainEvents* events,
                                              ThumbnailService* thumbnails)
    {
        registry_      = registry;
        asset_manager_ = std::move(manager);
        events_        = events;
        thumbnails_    = thumbnails;
    }

    // Bake the current graph into a MaterialAsset and persist it. The asset
    // stores the runtime artifacts (both passes' SPIR-V + ShaderInfo, param
    // defaults, texture-slot UUIDs) AND the authoring graph — which now
    // carries node positions, so saving also persists the layout.
    bool MaterialGraphPanel::saveAsAsset(const std::string& name, std::string* err,
                                         const std::filesystem::path& folder)
    {
        const auto fail = [&](std::string m) { if (err) *err = std::move(m); return false; };
        if (!asset_manager_ || !registry_)        return fail("asset services not wired");
        if (registry_->root().empty())            return fail("no project open");
        if (name.empty())                         return fail("name required");

        // Graph texture slot i -> texture asset UUID (slot index == graph slot).
        std::vector<lux::asset::asset_id_t> slot_ids;
        for (const auto& [slot, bind] : slot_texture_)
            if (slot < lux::asset::MaterialData::kMaxTextures)
            {
                if (slot >= slot_ids.size()) slot_ids.resize(slot + 1);
                slot_ids[slot] = bind.uuid;
            }

        // An explicit folder (browser right-click "New Material")
        // wins; otherwise the legacy <content>/Materials/ home.
        const std::filesystem::path dir =
            folder.empty() ? (registry_->root() / "Materials") : folder;
        std::error_code mk_ec;
        std::filesystem::create_directories(dir, mk_ec);
        const std::filesystem::path dest = dir / (name + ".luxasset");

        // Single source of truth: the SAME bake recipe import-time auto-conversion
        // uses (lower -> compile GBuffer+Forward -> params/shading-model/render-state/
        // texture-UUIDs/graph -> create+register+export). Random id (empty seed).
        auto id = lux::toolchain::bakeGraphMaterial(
            asset_manager_,
            graph_,
            slot_ids,
            name,
            dest,
            /*seed=*/std::string_view{});
        if (!id)
            return fail(id.error());

        // Save As 之后当前图就「属于」新资产 —— 原地 Save 从此可用。
        open_asset_id_   = *id;
        open_asset_path_ = dest;

        // Tell the user WHERE it landed — as the engine VIRTUAL path (the /Game
        // mount over the content root), not a raw OS path.
        std::error_code rel_ec;
        std::filesystem::path shown =
            std::filesystem::relative(dest, registry_->root(), rel_ec);
        if (rel_ec || shown.empty()) shown = dest.filename();
        save_status_ = "saved -> /Game/" + shown.replace_extension().generic_string();

        // Announce the new on-disk asset: the host refreshes BOTH the registry
        // (searchable now) AND the browser (visible now). The old direct
        // registry_->refresh() only did the former, so the saved material did
        // not actually appear in the browser until a later rescan.
        if (events_)
        {
            events_->publish(EditorAssetChanged{
                *id,
                EEditorAssetChange::ADDED,
                asset_manager_->contentRevision(*id),
                dest
            });
        }
        return true;
    }

    // 原地保存:同 id 覆写内存 payload + 盘上文件。与 saveAsAsset 的差异只有
    // 「资产从哪来」:那边 bake 出一个新资产,这边把编译产物装回打开的那个。
    // LuaConsole 的脚本保存是同款先例(记 id+path → setData/setPayload →
    // exportAsLuxAsset)。GPU 侧已上传的旧副本不会自动重编 —— 场景里的实体
    // 要到缓存条目失效/重建才见新样(热更新列为后续项,批丙去重表一起考虑)。
    bool MaterialGraphPanel::saveInPlace(std::string* err)
    {
        const auto fail = [&](std::string m)
        { if (err) *err = std::move(m); return false; };
        if (!asset_manager_)             return fail("asset services not wired");
        if (open_asset_id_.is_nil())     return fail("no asset open — use Save As");
        if (open_asset_path_.empty())    return fail("asset path unknown — use Save As");

        std::vector<lux::asset::asset_id_t> slot_ids;
        for (const auto& [slot, bind] : slot_texture_)
            if (slot < lux::asset::MaterialData::kMaxTextures)
            {
                if (slot >= slot_ids.size()) slot_ids.resize(slot + 1);
                slot_ids[slot] = bind.uuid;
            }

        auto payload = lux::toolchain::compileGraphToPayload(graph_, slot_ids);
        if (!payload)
            return fail(payload.error());

        // shell 可能是 data-less(驱逐后):get-or-load 一次再装新数据。
        (void)asset_manager_->ensureAsset(open_asset_id_);
        auto* a = asset_manager_->fetchAssetAs<lux::asset::MaterialAsset>(open_asset_id_);
        if (!a)
            return fail("open asset no longer registered — use Save As");
        a->setData(std::make_unique<lux::asset::MaterialData>(std::move(*payload)));
        lux::authoring::attachMaterialGraph(*a, graph_);

        lux::asset::MaterialSerDeser ser(asset_manager_);
        if (const auto e = ser.exportAsLuxAsset(open_asset_id_, open_asset_path_);
            e != lux::asset::EAssetError::SUCCESS)
            return fail(lux::format("export failed (err={})", static_cast<int>(e)));

        // 旧缩略图作废(帧关也安全,见 ThumbnailService::invalidate)+ 广播
        // 内容变化(registry/browser 重扫 —— New Script 落盘同款出口)。
        if (thumbnails_) thumbnails_->invalidate(open_asset_id_);
        asset_manager_->notifyContentChanged(open_asset_id_);
        if (events_)
        {
            events_->publish(EditorAssetChanged{
                open_asset_id_,
                EEditorAssetChange::CONTENT_UPDATED,
                asset_manager_->contentRevision(open_asset_id_),
                open_asset_path_
            });
        }

        std::error_code rel_ec;
        std::filesystem::path shown = registry_
            ? std::filesystem::relative(open_asset_path_, registry_->root(), rel_ec)
            : open_asset_path_.filename();
        if (rel_ec || shown.empty()) shown = open_asset_path_.filename();
        save_status_ = "saved -> /Game/" + shown.replace_extension().generic_string();
        return true;
    }

    bool MaterialGraphPanel::createNewMaterialAssetAt(const std::filesystem::path& folder)
    {
        if (!asset_manager_ || !registry_ || folder.empty())
            return false;

        // Fresh default graph (the ctor's starting point), replacing the
        // current working graph — standard "New" semantics.
        buildDefaultGraph();
        editor_.bind(&view_, &schema_);
        last_seen_revision_ = editor_.commands().structureRevision();
        resetEditorForNewGraph();
        slot_texture_.clear();

        // Free name: NewMaterial, NewMaterial_1, … (never clobber a file).
        std::string name;
        for (int i = 0; ; ++i)
        {
            name = i == 0 ? "NewMaterial" : ("NewMaterial_" + std::to_string(i));
            std::error_code ec;
            if (!std::filesystem::exists(folder / (name + ".luxasset"), ec))
                break;
        }
        std::string err;
        if (!saveAsAsset(name, &err, folder))
        {
            save_status_ = "new material failed: " + err;
            return false;
        }
        return true;
    }

    void MaterialGraphPanel::resetEditorForNewGraph()
    {
        color_pick_node_ = lux::rdesc::invalid_node;
        std::memset(last_tex_bindless_, 0, sizeof(last_tex_bindless_));
        compile();                                         // re-lower + push to preview
    }

    std::string MaterialGraphPanel::textureDisplayName(const lux::asset::asset_id_t& id) const
    {
        if (registry_)
            if (const auto* m = registry_->find(id))
                return m->name;
        return "texture";
    }

    // UE-style "double-click any material -> material editor": a MATERIAL is
    // reconstructed from its persisted graph. v2 graphs restore their node
    // layout; v1 graphs open unplaced (the editor grid-lays them out) and
    // upgrade on the next save.
    void MaterialGraphPanel::openAsset(const lux::asset::asset_id_t& id)
    {
        if (!asset_manager_ || id.is_nil()) return;
        const auto* info = asset_manager_->queryInfo(id);
        if (!info) return;

        MaterialGraph g;
        std::unordered_map<std::uint32_t, TexBinding> slots;

        if (info->type == lux::asset::EAssetType::MATERIAL_INSTANCE)
        {
            openInstanceAsset(id);   // 实例模式(见头文件 openAsset 文档)
            return;
        }
        if (info->type == lux::asset::EAssetType::MATERIAL)
        {
            // W2c 驱逐真正执行之后,「shell 在、数据不在」是常态(列表视图从不
            // 请求缩略图,材质 payload 可能从没被拉起过)。同步 get-or-load 一次;
            // 仍然失败才放弃 —— 且要**说出来**,静默 no-op 的面板像是坏了。
            (void)asset_manager_->ensureAsset(id);
            const auto* a = asset_manager_->fetchAssetAs<lux::asset::MaterialAsset>(id);
            if (!a || !a->data())
            {
                status_ = "open failed: material payload could not be loaded";
                return;
            }
            std::string graph_error;
            if (!lux::authoring::readMaterialGraph(*a, g, &graph_error))
            {
                status_ = "open failed: " + graph_error;
                return;
            }
            for (std::uint32_t s = 0; s < lux::asset::MaterialData::kMaxTextures; ++s)
            {
                const auto tid = a->data()->texture_slot_ids[s];
                if (!tid.is_nil())
                    slots[s] = TexBinding{ tid, textureDisplayName(tid) };
            }
        }
        else
        {
            return;   // only graph MATERIALs reopen (W5: the sole material kind)
        }

        graph_        = std::move(g);
        slot_texture_ = std::move(slots);
        mode_         = EEditMode::GRAPH;
        editor_.bind(&view_, &schema_);   // full-reset rebind (the view lenses graph_)
        last_seen_revision_ = editor_.commands().structureRevision();
        editor_.setTopologyLocked(false);   // R3:图模式解除拓扑锁
        schema_.setInstanceMode(false);
        resetEditorForNewGraph();

        // 记住「现在打开的是谁」—— Save(原地覆写)的前提。路径从注册表取
        // (失败留空,Save 按钮据此禁用,Save As 照常可用)。
        open_asset_id_ = id;
        open_asset_path_.clear();
        if (registry_)
            if (const auto* m = registry_->find(id); m && !registry_->root().empty())
                open_asset_path_ = registry_->root() / m->rel_path;
    }

    // ════════════════════════════════════════════════════════════════════
    //  实例模式(openAsset 对 MATERIAL_INSTANCE 的分派;头文件 openAsset 文档)
    // ════════════════════════════════════════════════════════════════════

    void MaterialGraphPanel::openInstanceAsset(const lux::asset::asset_id_t& id)
    {
        (void)asset_manager_->ensureAsset(id);   // shell 可能 data-less(W2c 驱逐后是常态)
        const auto* a = asset_manager_->fetchAssetAs<lux::asset::MaterialInstanceAsset>(id);
        if (!a || !a->data())
        {
            status_ = "open failed: instance payload could not be loaded";
            return;
        }

        inst_edit_ = *a->data();                  // 编辑副本;Save 写回
        std::string err;
        if (!resolveInstanceChain(&err))
        {
            status_ = "open failed: " + err;
            return;
        }

        mode_          = EEditMode::INSTANCE;
        open_asset_id_ = id;
        open_asset_path_.clear();
        if (registry_)
            if (const auto* m = registry_->find(id); m && !registry_->root().empty())
                open_asset_path_ = registry_->root() / m->rel_path;
        status_ = lux::format("instance: {} param lane(s), {} texture slot(s)",
                              inst_decls_.size(), inst_tex_decls_.size());

        // R3:与 Material 同一套画布 —— 根图 clone 进画布(带持久化布局),
        // 拓扑锁定,Param/SampleTexture 节点体转 override 编辑面(schema 的
        // 实例钩子)。slot_texture_ 是图模式的绑定表,这里不用(有效贴图经
        // inst_parent_tex_ ⊕ override 解析)。
        if (const auto* root =
                asset_manager_->fetchAssetAs<lux::asset::MaterialAsset>(inst_root_id_);
            root && root->data())
        {
            std::string graph_error;
            if (!lux::authoring::readMaterialGraph(*root, graph_, &graph_error))
            {
                status_ = "open failed: " + graph_error;
                return;
            }
        }
        slot_texture_.clear();
        editor_.bind(&view_, &schema_);
        last_seen_revision_ = editor_.commands().structureRevision();
        editor_.setTopologyLocked(true);
        schema_.setInstanceMode(true);
        editor_.navigateToContent();

        pushInstancePreview();
    }

    // 沿 parent 链解析到根。「父的有效值」= 根图默认 ⊕ (根,直接父] 各级
    // override,应用顺序根→叶(近端赢)。每级 ensureAsset:链上任何一级被驱逐
    // 都是 data-less shell。
    bool MaterialGraphPanel::resolveInstanceChain(std::string* err)
    {
        const auto fail = [&](std::string m) { if (err) *err = std::move(m); return false; };
        if (!asset_manager_) return fail("asset services not wired");
        constexpr int kMaxChainDepth = 8;   // 实例链深度守卫(驻留编排环检同族)

        std::vector<const lux::asset::MaterialInstanceData*> chain;
        lux::asset::asset_id_t cur = inst_edit_.parent_material_id;
        for (int depth = 0; ; ++depth)
        {
            if (cur.is_nil())            return fail("instance has no parent");
            if (depth > kMaxChainDepth)  return fail("parent chain too deep or cyclic");

            const auto* info = asset_manager_->queryInfo(cur);
            if (!info)                   return fail("a parent in the chain is not registered");
            if (info->type == lux::asset::EAssetType::MATERIAL)
            {
                inst_root_id_ = cur;
                break;
            }
            if (info->type != lux::asset::EAssetType::MATERIAL_INSTANCE)
                return fail("a parent in the chain is not a material");

            (void)asset_manager_->ensureAsset(cur);
            const auto* pa =
                asset_manager_->fetchAssetAs<lux::asset::MaterialInstanceAsset>(cur);
            if (!pa || !pa->data())      return fail("a parent instance failed to load");
            chain.push_back(pa->data());
            cur = pa->data()->parent_material_id;
        }

        (void)asset_manager_->ensureAsset(inst_root_id_);
        const auto* root = asset_manager_->fetchAssetAs<lux::asset::MaterialAsset>(inst_root_id_);
        if (!root || !root->data())      return fail("root material failed to load");
        const lux::asset::MaterialData& rp = *root->data();
        lux::rdesc::MaterialGraph root_graph;
        std::string graph_error;
        if (!lux::authoring::readMaterialGraph(*root, root_graph, &graph_error))
            return fail(graph_error);

        inst_decls_     = root_graph.param_slots;
        inst_tex_decls_ = root_graph.texture_slots;

        // 基线 = 根默认值,再从根向叶逐级应用 override(chain 收集序是叶→根)。
        for (std::uint32_t i = 0; i < lux::asset::MaterialInstanceData::kMaxParams; ++i)
            for (std::uint32_t j = 0; j < 4; ++j)
                inst_parent_params_[i][j] = i < inst_decls_.size() ? inst_decls_[i].dflt[j] : 0.0f;
        for (std::uint32_t s = 0; s < lux::asset::MaterialInstanceData::kMaxTextures; ++s)
            inst_parent_tex_[s] = rp.texture_slot_ids[s];
        inst_parent_alpha_ = rp.alpha_mode;
        inst_parent_dbl_   = rp.double_sided;

        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            const auto& lvl = **it;
            for (std::uint32_t i = 0; i < lux::asset::MaterialInstanceData::kMaxParams; ++i)
                if (lvl.param_override_mask & (1u << i))
                    for (std::uint32_t j = 0; j < 4; ++j)
                        inst_parent_params_[i][j] = lvl.params[i][j];
            for (std::uint32_t s = 0; s < lux::asset::MaterialInstanceData::kMaxTextures; ++s)
                if (lvl.tex_override_mask & (1u << s))
                    inst_parent_tex_[s] = lvl.texture_slot_ids[s];
            if (lvl.render_state_override)
            {
                inst_parent_alpha_ = lvl.alpha_mode;
                inst_parent_dbl_   = lvl.double_sided;
            }
        }
        return true;
    }

    lux::render::GraphMaterialData MaterialGraphPanel::buildInstanceEffective()
    {
        lux::render::GraphMaterialData gd{};
        gd.param_count = static_cast<std::uint32_t>(
            std::min<std::size_t>(inst_decls_.size(),
                                  lux::render::GraphMaterialData::kMaxParams));
        for (std::uint32_t i = 0; i < lux::render::GraphMaterialData::kMaxParams; ++i)
        {
            const bool ov = (inst_edit_.param_override_mask & (1u << i)) != 0u;
            for (std::uint32_t j = 0; j < 4; ++j)
                gd.params[i][j] = ov ? inst_edit_.params[i][j] : inst_parent_params_[i][j];
        }
        if (preview_)
        {
            for (std::uint32_t s = 0; s < lux::render::GraphMaterialData::kMaxTextures; ++s)
            {
                const auto& tid = (inst_edit_.tex_override_mask & (1u << s))
                                      ? inst_edit_.texture_slot_ids[s]
                                      : inst_parent_tex_[s];
                if (tid.is_nil()) continue;
                gd.tex_bindless[s] = preview_->resolveTextureIndex(tid);
                gd.tex_mask |= (1u << s);
            }
        }
        return gd;
    }

    void MaterialGraphPanel::pushInstancePreview()
    {
        if (!preview_ || !asset_manager_ || inst_root_id_.is_nil()) return;
        const auto* root = asset_manager_->fetchAssetAs<lux::asset::MaterialAsset>(inst_root_id_);
        if (!root || !root->data()) return;
        const lux::asset::MaterialData& rp = *root->data();

        // Compose a cooked preview payload. Instances never compile or retain an
        // authoring graph; they override the root's flat runtime lanes.
        auto payload = std::make_unique<lux::asset::MaterialData>();
        payload->parameter_defaults = rp.parameter_defaults;
        payload->parameter_count    = rp.parameter_count;
        payload->alpha_mode         = rp.alpha_mode;
        payload->double_sided       = rp.double_sided;
        payload->gbuffer_spirv = rp.gbuffer_spirv;
        payload->gbuffer_info  = rp.gbuffer_info;
        payload->forward_spirv = rp.forward_spirv;
        payload->forward_info  = rp.forward_info;
        for (std::uint32_t i = 0;
             i < payload->parameter_count
             && i < lux::asset::MaterialInstanceData::kMaxParams; ++i)
        {
            const bool ov = (inst_edit_.param_override_mask & (1u << i)) != 0u;
            for (std::uint32_t j = 0; j < 4; ++j)
                payload->parameter_defaults[i][j] =
                    ov ? inst_edit_.params[i][j] : inst_parent_params_[i][j];
        }
        for (std::uint32_t s = 0; s < lux::asset::MaterialInstanceData::kMaxTextures; ++s)
            payload->texture_slot_ids[s] = (inst_edit_.tex_override_mask & (1u << s))
                                               ? inst_edit_.texture_slot_ids[s]
                                               : inst_parent_tex_[s];
        payload->alpha_mode =
            inst_edit_.render_state_override ? inst_edit_.alpha_mode : inst_parent_alpha_;
        payload->double_sided =
            inst_edit_.render_state_override ? inst_edit_.double_sided : inst_parent_dbl_;

        preview_->setGraphContent(std::move(payload));
        const auto gd = buildInstanceEffective();
        std::memcpy(last_tex_bindless_, gd.tex_bindless, sizeof(last_tex_bindless_));
        preview_->updateGraphParams(gd);   // 换刀就绪后补发参数(params_valid 通道)
    }

    bool MaterialGraphPanel::saveInstanceInPlace(std::string* err)
    {
        const auto fail = [&](std::string m) { if (err) *err = std::move(m); return false; };
        if (!asset_manager_)          return fail("asset services not wired");
        if (open_asset_id_.is_nil())  return fail("no instance open");
        if (open_asset_path_.empty()) return fail("asset path unknown");

        (void)asset_manager_->ensureAsset(open_asset_id_);
        auto* a = asset_manager_->fetchAssetAs<lux::asset::MaterialInstanceAsset>(open_asset_id_);
        if (!a) return fail("instance no longer registered");
        a->setData(std::make_unique<lux::asset::MaterialInstanceData>(inst_edit_));

        lux::asset::MaterialInstanceSerDeser ser(asset_manager_);
        if (const auto e = ser.exportAsLuxAsset(open_asset_id_, open_asset_path_);
            e != lux::asset::EAssetError::SUCCESS)
            return fail(lux::format("export failed (err={})", static_cast<int>(e)));

        if (thumbnails_) thumbnails_->invalidate(open_asset_id_);   // 随时可调(帧关延迟归还)
        asset_manager_->notifyContentChanged(open_asset_id_);
        if (events_)
        {
            events_->publish(EditorAssetChanged{
                open_asset_id_,
                EEditorAssetChange::CONTENT_UPDATED,
                asset_manager_->contentRevision(open_asset_id_),
                open_asset_path_
            });
        }
        return true;
    }

    // R3:实例模式属性条 —— 参数/贴图的编辑面搬进了**共用画布**的节点体
    // (schema 实例钩子,拓扑锁定);这里只剩会话动作 + render-state override
    // (图级属性,没有对应节点)。
    void MaterialGraphPanel::drawInstanceProperties()
    {
        static constexpr const char* kAlphaModeNames[] = { "Opaque", "Mask", "Blend" };

        // ── 工具栏:Save / Revert / Close(回图模式) / parent 链信息 ──
        const bool ctrl_s = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
                            && ImGui::GetIO().KeyCtrl
                            && ImGui::IsKeyPressed(ImGuiKey_S, /*repeat=*/false);
        if (ImGui::Button("Save") || ctrl_s)
        {
            std::string err;
            status_ = saveInstanceInPlace(&err) ? "saved" : ("save failed: " + err);
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert"))
            openInstanceAsset(open_asset_id_);   // 重读数据 + 重推预览
        ImGui::SameLine();
        if (ImGui::Button("Close"))
        {
            // 回图模式的空白起点(不丢盘上数据 —— 实例编辑副本仅内存)。
            buildDefaultGraph();
            editor_.bind(&view_, &schema_);
            last_seen_revision_ = editor_.commands().structureRevision();
            resetEditorForNewGraph();
            slot_texture_.clear();
            return;
        }
        ImGui::SameLine();
        {
            std::string parent_name = "?";
            if (registry_)
                if (const auto* m = registry_->find(inst_edit_.parent_material_id))
                    parent_name = m->name;
            ImGui::TextDisabled("parent: %s", parent_name.c_str());
        }
        if (!status_.empty()) { ImGui::SameLine(); ImGui::TextUnformatted(status_.c_str()); }

        bool content_dirty = false;   // 换刀(render-state 影响 PSO)

        if (ImGui::CollapsingHeader("Render State", ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool ov = inst_edit_.render_state_override != 0u;
            if (ImGui::Checkbox("Override##rs", &ov))
            {
                inst_edit_.render_state_override = ov ? 1u : 0u;
                if (ov)
                {
                    inst_edit_.alpha_mode   = inst_parent_alpha_;
                    inst_edit_.double_sided = inst_parent_dbl_;
                }
                content_dirty = true;   // PSO 级变化:换刀
            }
            ImGui::BeginDisabled(!ov);
            int am = static_cast<int>(ov ? inst_edit_.alpha_mode : inst_parent_alpha_);
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::Combo("Alpha Mode", &am, kAlphaModeNames, 3) && ov)
            {
                inst_edit_.alpha_mode = static_cast<std::uint32_t>(am);
                content_dirty = true;
            }
            bool ds = ov ? inst_edit_.double_sided : inst_parent_dbl_;
            if (ImGui::Checkbox("Double Sided", &ds) && ov)
            {
                inst_edit_.double_sided = ds;
                content_dirty = true;
            }
            ImGui::EndDisabled();
        }

        // 参数/贴图 override 的快路径推送在 schema 钩子里(instanceParamsChanged);
        // 贴图 bindless 翻转的补推在 paint() 共用尾段;预览在共用预览窗。
        if (content_dirty)
            pushInstancePreview();
    }

    void MaterialGraphPanel::instanceParamsChanged()
    {
        if (!preview_) return;
        const auto gd = buildInstanceEffective();
        std::memcpy(last_tex_bindless_, gd.tex_bindless, sizeof(last_tex_bindless_));
        preview_->updateGraphParams(gd);
    }

    // Modal "name + Save" popup. Drawn OUTSIDE the node-editor canvas (in paint()).
    void MaterialGraphPanel::drawSavePopup()
    {
        constexpr const char* kId = "Save Graph Material##matgraph";
        if (save_popup_open_)
        {
            ImGui::OpenPopup(kId);
            save_popup_open_ = false;
        }
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;

        ImGui::TextUnformatted("Asset name:");
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##savename", &save_name_,
                                             ImGuiInputTextFlags_EnterReturnsTrue);
        if (!save_status_.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", save_status_.c_str());

        // Destination preview — the user always sees WHERE this will land, as
        // the engine virtual path (the /Game mount over the content root).
        ImGui::TextDisabled("-> /Game/Materials/%s", save_name_.c_str());

        const bool do_save = ImGui::Button("Save", ImVec2(120.0f, 0.0f)) || enter;
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
        {
            save_status_.clear();
            ImGui::CloseCurrentPopup();
        }
        if (do_save)
        {
            std::string e;
            if (saveAsAsset(save_name_, &e))
            {
                status_ = lux::format("saved material -> /Game/Materials/{}", save_name_);
                save_status_.clear();
                ImGui::CloseCurrentPopup();
            }
            else
            {
                save_status_ = e;
            }
        }
        ImGui::EndPopup();
    }

    // Live material data = params (from param_slots) + resolved texture bindless
    // indices (slot_texture_ 经预览宿主查进程域共享缓存 —— EditorTextureCache
    // 并轨后同一张贴图不再有 UI 专属的第二份显存副本)。A slot's tex_bindless
    // is 0 (engine default) until its async upload completes.
    lux::render::GraphMaterialData MaterialGraphPanel::buildGraphMaterial()
    {
        lux::render::GraphMaterialData d = buildGraphParams(graph_);
        if (preview_)
        {
            for (const auto& [slot, bind] : slot_texture_)
            {
                if (slot >= lux::render::GraphMaterialData::kMaxTextures) continue;
                d.tex_bindless[slot] = preview_->resolveTextureIndex(bind.uuid);
                d.tex_mask |= (1u << slot);
            }
        }
        return d;
    }

    std::string MaterialGraphPanel::boundTextureName(std::uint32_t slot) const
    {
        // R3 实例模式:名字按**有效**绑定解析(parent ⊕ override,近端赢)。
        if (mode_ == EEditMode::INSTANCE)
        {
            if (slot >= lux::asset::MaterialInstanceData::kMaxTextures)
                return {};
            const bool ov = (inst_edit_.tex_override_mask & (1u << slot)) != 0u;
            const auto& tid = ov ? inst_edit_.texture_slot_ids[slot]
                                 : inst_parent_tex_[slot];
            if (tid.is_nil())
                return {};
            if (registry_)
                if (const auto* m = registry_->find(tid))
                    return m->name;
            return "(texture)";
        }
        const auto it = slot_texture_.find(slot);
        return (it != slot_texture_.end()) ? it->second.name : std::string{};
    }

    // Open the fuzzy-search popup over ALL project textures for a SampleTexture
    // slot. The popup is drawn OUTSIDE the node canvas (in paint()). On pick,
    // record the slot->texture binding; the per-frame push streams it live.
    void MaterialGraphPanel::openTexturePicker(std::uint32_t slot)
    {
        if (!registry_) return;
        std::vector<std::string>            names;
        std::vector<lux::asset::asset_id_t> uuids;
        for (std::uint32_t idx : registry_->ofType(lux::asset::EAssetType::TEXTURE))
        {
            const AssetMeta& meta = registry_->all()[idx];
            names.push_back(meta.name);
            uuids.push_back(meta.id);
        }
        texture_popup_.setItems(names);
        texture_popup_.setOnSelect(
            [this, slot, uuids, names](std::uint32_t picked)
            {
                if (picked >= uuids.size())
                    return;
                if (mode_ == EEditMode::INSTANCE)
                {
                    // R3:实例模式选图 = 写贴图 override(picker 只在
                    // override 勾上时可达,置位是双保险)。
                    inst_edit_.texture_slot_ids[slot] = uuids[picked];
                    inst_edit_.tex_override_mask |= (1u << slot);
                    instanceParamsChanged();
                    return;
                }
                slot_texture_[slot] = TexBinding{ uuids[picked], names[picked] };
            });
        texture_popup_.open();
    }

    void MaterialGraphPanel::drawToolbar()
    {
        auto& commands = editor_.commands();

        if (ImGui::Button("Compile"))
            compile();
        ImGui::SameLine();
        ImGui::BeginDisabled(!commands.canUndo());
        if (ImGui::Button("Undo"))
            editor_.undo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!commands.canRedo());
        if (ImGui::Button("Redo"))
            editor_.redo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Frame"))
            editor_.navigateToContent();
        ImGui::SameLine();
        ImGui::Checkbox("GLSL", &show_glsl_);
        ImGui::SameLine();
        if (ImGui::Button(show_preview_ ? "Hide Preview" : "Show Preview"))
            show_preview_ = !show_preview_;

        // "Save" — in-place overwrite of the OPEN asset (id + path recorded by
        // openAsset / saveAsAsset); disabled on the default graph. Ctrl+S while
        // the panel is focused does the same.
        ImGui::SameLine();
        const bool can_save_in_place =
            asset_manager_ && !open_asset_id_.is_nil() && !open_asset_path_.empty();
        const bool ctrl_s = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
                            && ImGui::GetIO().KeyCtrl
                            && ImGui::IsKeyPressed(ImGuiKey_S, /*repeat=*/false);
        ImGui::BeginDisabled(!can_save_in_place);
        if (ImGui::Button("Save") || (can_save_in_place && ctrl_s))
        {
            std::string err;
            if (!saveInPlace(&err))
                save_status_ = "save failed: " + err;
            status_ = save_status_;
        }
        ImGui::EndDisabled();
        if (!can_save_in_place && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("No asset open — use Save as Asset first.");

        // "Save as Asset" — only when a project is open (we write under its
        // content root) and the manager is wired.
        ImGui::SameLine();
        const bool can_save = asset_manager_ && registry_ && !registry_->root().empty();
        ImGui::BeginDisabled(!can_save);
        if (ImGui::Button("Save as Asset"))
        {
            if (save_name_.empty()) save_name_ = "GraphMaterial";
            save_status_.clear();
            save_popup_open_ = true;
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::TextUnformatted(status_.c_str());
    }

    void MaterialGraphPanel::drawGraphProperties()
    {
        auto& rs = graph_.render_state;
        bool recompile = false;

        static const char* const kAlpha[] = { "Opaque", "Mask", "Blend" };
        int am = static_cast<int>(rs.alpha_mode);
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::Combo("alpha mode", &am, kAlpha, 3))
        {
            rs.alpha_mode = static_cast<rdesc::EAlphaMode>(am);
            recompile = true;   // Mask bakes a `discard` -> the SPIR-V changes
        }
        if (rs.alpha_mode == rdesc::EAlphaMode::Mask)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::SliderFloat("cutoff", &rs.alpha_cutoff, 0.0f, 1.0f);
            if (ImGui::IsItemDeactivatedAfterEdit())
                recompile = true;   // the cutoff is a baked literal in the discard
        }
        ImGui::SameLine();
        ImGui::Checkbox("double sided", &rs.double_sided);  // pure PSO state — no re-bake
        if (rs.alpha_mode == rdesc::EAlphaMode::Blend)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(Blend: rendered opaque — transparent pass TBD)");
        }

        if (recompile)
            compile();
    }

    void MaterialGraphPanel::paint()
    {
        // R3:两种模式共用同一套画布 —— 实例模式画根图(拓扑锁定,Param/
        // SampleTexture 节点体转 override 编辑面),图模式行为不变。
        if (mode_ == EEditMode::INSTANCE)
        {
            drawInstanceProperties();
        }
        else
        {
            drawToolbar();
            drawGraphProperties();
            if (show_glsl_ && !glsl_.empty())
            {
                ImGui::InputTextMultiline("##matglsl", &glsl_, ImVec2(-1.0f, 120.0f),
                    ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_AllowTabInput);
            }
        }

        // ── the shared canvas (chrome, right-click fuzzy palette, undoable
        //    edits, position sync) ─────────────────────────────────────────
        editor_.paint("MatGraphCanvas");

        // Drain the deferred popup queue — popup-shaped UI raised from inside
        // the canvas is drawn HERE, in panel space (the canvas's transformed
        // coordinate space can't receive popup input).
        for (const auto& req : editor_.takeDeferredPopups())
        {
            if (req.kind == "texture_picker")
                openTexturePicker(static_cast<std::uint32_t>(req.arg));
            else if (req.kind == "color_picker")
                color_pick_node_ = static_cast<rdesc::node_id>(req.node.id);
        }

        // Bake-on-edit: any STRUCTURAL change (add/remove/connect/disconnect,
        // including undo/redo replays) rebakes the shader + preview. Node
        // moves never trigger this; body edits recompile via the schema's
        // dirty callback instead.(实例模式拓扑锁定,revision 不会动 ——
        // 实例永不编译,SPIR-V 恒取自根。)
        if (mode_ == EEditMode::GRAPH &&
            editor_.commands().structureRevision() != last_seen_revision_)
        {
            last_seen_revision_ = editor_.commands().structureRevision();
            compile();
        }

        // Live preview lives in its OWN top-level window so it can be freely
        // resized / docked / floated, decoupled from this panel's layout.
        if (preview_ && show_preview_)
        {
            ImGui::SetNextWindowSize(ImVec2(420.0f, 460.0f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Material Preview", &show_preview_))
            {
                ImGui::TextUnformatted("drag = orbit, wheel = zoom");
                preview_element_.draw();
            }
            ImGui::End();
        }

        // Floating color picker for the clicked color Param — panel space.
        // Edits stream to the preview live via modifyGraphMaterial.
        if (color_pick_node_ != lux::rdesc::invalid_node)
        {
            bool  open  = true;
            auto* cnode = graph_.node(color_pick_node_);
            auto* pn    = cnode ? cnode->as<lux::rdesc::ParamNode>() : nullptr;
            if (pn && pn->param_slot < graph_.param_slots.size())
            {
                lux::rdesc::ParamSlotDecl& slot = graph_.param_slots[pn->param_slot];
                ImGui::SetNextWindowSize(ImVec2(280.0f, 0.0f), ImGuiCond_Appearing);
                if (ImGui::Begin("Color Picker##matgraph", &open))
                {
                    ImGui::TextUnformatted(slot.name.c_str());
                    ImGuiColorEditFlags f = ImGuiColorEditFlags_Float;
                    if (nameLooksLikeEmissive(slot.name)) f |= ImGuiColorEditFlags_HDR;
                    const int comps = static_cast<int>(slot.type) + 1;
                    if (mode_ == EEditMode::INSTANCE)
                    {
                        // R3:实例模式编辑的是 override lane,不写根图默认值。
                        const std::uint32_t lane = pn->param_slot;
                        float v[4];
                        const bool ov =
                            (inst_edit_.param_override_mask & (1u << lane)) != 0u;
                        std::memcpy(v, ov ? inst_edit_.params[lane]
                                          : inst_parent_params_[lane], sizeof(v));
                        const bool ch = (comps >= 4)
                            ? ImGui::ColorPicker4("##pick", v, f)
                            : ImGui::ColorPicker3("##pick", v, f);
                        if (ch)
                        {
                            std::memcpy(inst_edit_.params[lane], v, sizeof(v));
                            inst_edit_.param_override_mask |= (1u << lane);
                            instanceParamsChanged();
                        }
                    }
                    else
                    {
                        const bool ch = (comps >= 4)
                            ? ImGui::ColorPicker4("##pick", slot.dflt, f)
                            : ImGui::ColorPicker3("##pick", slot.dflt, f);
                        if (ch && preview_)
                            preview_->updateGraphParams(buildGraphMaterial());
                    }
                }
                ImGui::End();
            }
            else
            {
                open = false;
            }
            if (!open)
                color_pick_node_ = lux::rdesc::invalid_node;
        }

        // The texture picker popup — panel space (anchored at the mouse).
        texture_popup_.draw();

        // The "Save as Asset" modal — likewise outside the canvas.
        drawSavePopup();

        // Re-push the material when a pending texture upload completes (its bindless
        // index flips 0 -> real). Cheap: a cache lookup per bound slot + a memcmp.
        // 实例模式同款检查走有效贴图集(parent ⊕ override)。
        if (preview_)
        {
            if (mode_ == EEditMode::INSTANCE)
            {
                const lux::render::GraphMaterialData d = buildInstanceEffective();
                if (std::memcmp(d.tex_bindless, last_tex_bindless_,
                                sizeof(last_tex_bindless_)) != 0)
                {
                    preview_->updateGraphParams(d);
                    std::memcpy(last_tex_bindless_, d.tex_bindless,
                                sizeof(last_tex_bindless_));
                }
            }
            else if (!slot_texture_.empty())
            {
                const lux::render::GraphMaterialData d = buildGraphMaterial();
                if (std::memcmp(d.tex_bindless, last_tex_bindless_,
                                sizeof(last_tex_bindless_)) != 0)
                {
                    preview_->updateGraphParams(d);
                    std::memcpy(last_tex_bindless_, d.tex_bindless,
                                sizeof(last_tex_bindless_));
                }
            }
        }
    }
}
