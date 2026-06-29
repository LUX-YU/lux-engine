#include "panels/MaterialGraphPanel.hpp"
#include "import/MaterialGraphBake.hpp"

#include <lux/engine/description/material_graph/Node.hpp>
#include <lux/engine/description/material_graph/Nodes.hpp>
#include <lux/engine/shadergen/material/MaterialLowering.hpp>
#include <lux/engine/shadergen/glsl/Backend.hpp>
#include <lux/engine/editor/content/RenderShadersPath.hpp>
#include <lux/engine/description/MaterialGraphContract.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/editor/thumbnail/PreviewScene.hpp>
#include <lux/engine/render/resources/material/GraphMaterialData.hpp>
#include <lux/engine/editor/app/FormatCompat.h>   // lux::format — impl-only (kept out of the header)
#include <lux/engine/editor/AssetRegistry.hpp>
#include <lux/engine/editor/EditorTextureCache.hpp>
#include <lux/engine/editor/app/EditorEvents.hpp>   // events_->content_changed.emit
#include <lux/engine/asset/MaterialAsset.hpp>
#include <lux/engine/asset/MaterialSerDeser.hpp>
#include <lux/engine/asset/AssetManager.hpp>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
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
    }

    void MaterialGraphPanel::compile()
    {
        glsl_.clear();
        auto lr = sgm::lowerMaterial(graph_);
        if (!lr)
        {
            status_ = lux::format("lower error: {}", lr.error());
            return;
        }
        // ShaderGen #includes the real render SSOT — pass the render shaders dir as
        // the shaderc include search root (cmake-generated RenderShadersPath.hpp).
        const std::vector<std::string> inc = { lux::editor::render_shaders_dir };
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
            status_ = lux::format("emit error: {}", g.error());
            return;
        }
        glsl_ = std::move(*g);
        auto cs = sgg::compileToSpirv(lr->ir, mkParams(rdesc::EMaterialPass::GBuffer), inc);
        if (!cs)
        {
            status_ = lux::format("spirv error: {}", cs.error());
            return;
        }
        status_ = lux::format("OK: {} SPIR-V words / {} nodes",
                              cs->spirv.size(), graph_.nodes().size());

        // Feed the live preview with the FORWARD-pass frag + reflection (the
        // preview renders through the engine's ForwardMesh pass + graph override).
        if (preview_)
        {
            auto fwd = sgg::compileToSpirv(lr->ir, mkParams(rdesc::EMaterialPass::Forward), inc);
            if (fwd)
            {
                const lux::render::GraphMaterialData data = buildGraphMaterial();
                preview_->setGraphContent(fwd->spirv, fwd->info, data);
                std::memcpy(last_tex_bindless_, data.tex_bindless, sizeof(last_tex_bindless_));
            }
            else
            {
                status_ += lux::format("  [preview fwd error: {}]", fwd.error());
            }
        }
    }

    void MaterialGraphPanel::setPreviewScene(PreviewScene* preview)
    {
        preview_ = preview;
        if (!preview_) return;
        preview_element_.setView(preview_->sceneId(), preview_->view().id);
        preview_element_.setOrbitCallback(
            [p = preview_](float dy, float dp, float dz){ p->orbit(dy, dp, dz); });
        preview_element_.setResizeCallback(
            [p = preview_](std::uint32_t w, std::uint32_t h){ p->requestResize(w, h); });
        compile();   // push the current graph to the preview now
    }

    void MaterialGraphPanel::setAssetServices(AssetRegistry* registry, EditorTextureCache* cache,
                                              std::shared_ptr<lux::asset::AssetManager> manager,
                                              EditorEvents* events)
    {
        registry_      = registry;
        texture_cache_ = cache;
        asset_manager_ = std::move(manager);
        events_        = events;
    }

    // Bake the current graph into a MaterialAsset and persist it. The asset
    // stores the runtime artifacts (both passes' SPIR-V + ShaderInfo, param
    // defaults, texture-slot UUIDs) AND the authoring graph — which now
    // carries node positions, so saving also persists the layout.
    bool MaterialGraphPanel::saveAsAsset(const std::string& name, std::string* err)
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

        const std::filesystem::path dest =
            registry_->root() / "Materials" / (name + ".luxasset");

        // Single source of truth: the SAME bake recipe import-time auto-conversion
        // uses (lower -> compile GBuffer+Forward -> params/shading-model/render-state/
        // texture-UUIDs/graph -> create+register+export). Random id (empty seed).
        auto id = bakeGraphMaterial(asset_manager_, graph_, slot_ids, name, dest,
                                    /*seed=*/std::string_view{});
        if (!id)
            return fail(id.error());

        // Announce the new on-disk asset: the host refreshes BOTH the registry
        // (searchable now) AND the browser (visible now). The old direct
        // registry_->refresh() only did the former, so the saved material did
        // not actually appear in the browser until a later rescan.
        if (events_) events_->content_changed.emit({});
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

        if (info->type == lux::asset::EAssetType::MATERIAL)
        {
            const auto* a = asset_manager_->fetchAssetAs<lux::asset::MaterialAsset>(id);
            if (!a || !a->data()) return;
            // The asset OWNS the concrete graph — clone it into an editable working
            // copy (the graph is move-only; don't alias the asset's data).
            g = a->data()->graph.clone();
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
        editor_.bind(&view_, &schema_);   // full-reset rebind (the view lenses graph_)
        last_seen_revision_ = editor_.commands().structureRevision();
        resetEditorForNewGraph();
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
                status_ = lux::format("saved material '{}'", save_name_);
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
    // indices (from slot_texture_ via the editor texture cache). A slot's
    // tex_bindless is 0 (engine default) until its async upload completes.
    lux::render::GraphMaterialData MaterialGraphPanel::buildGraphMaterial()
    {
        lux::render::GraphMaterialData d = buildGraphParams(graph_);
        if (texture_cache_)
        {
            for (const auto& [slot, bind] : slot_texture_)
            {
                if (slot >= lux::render::GraphMaterialData::kMaxTextures) continue;
                d.tex_bindless[slot] = texture_cache_->resolve(bind.uuid);
                d.tex_mask |= (1u << slot);
            }
        }
        return d;
    }

    std::string MaterialGraphPanel::boundTextureName(std::uint32_t slot) const
    {
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
                if (picked < uuids.size())
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
        drawToolbar();
        drawGraphProperties();
        if (show_glsl_ && !glsl_.empty())
        {
            ImGui::InputTextMultiline("##matglsl", &glsl_, ImVec2(-1.0f, 120.0f),
                ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_AllowTabInput);
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
        // dirty callback instead.
        if (editor_.commands().structureRevision() != last_seen_revision_)
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
                    const int  comps = static_cast<int>(slot.type) + 1;
                    const bool ch = (comps >= 4) ? ImGui::ColorPicker4("##pick", slot.dflt, f)
                                                 : ImGui::ColorPicker3("##pick", slot.dflt, f);
                    if (ch && preview_)
                        preview_->updateGraphParams(buildGraphMaterial());
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
        if (preview_ && texture_cache_ && !slot_texture_.empty())
        {
            const lux::render::GraphMaterialData d = buildGraphMaterial();
            if (std::memcmp(d.tex_bindless, last_tex_bindless_, sizeof(last_tex_bindless_)) != 0)
            {
                preview_->updateGraphParams(d);
                std::memcpy(last_tex_bindless_, d.tex_bindless, sizeof(last_tex_bindless_));
            }
        }
    }
}
