#include <lux/engine/asset/MaterialGraphCodec.hpp>

#include <lux/engine/asset/detail/ByteIO.hpp>
#include <lux/engine/description/material_graph/Nodes.hpp>

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lux::asset::detail
{
    namespace
    {
        namespace rd = ::lux::rdesc;

        // ── encode: per-kind node payload ────────────────────────────────────
        void writePayload(ByteWriter& w, const rd::Node* n)
        {
            switch (n->kind())
            {
            case rd::EMatNodeKind::Constant:
            {
                auto* c = static_cast<const rd::ConstantNode*>(n);
                for (float v : c->value) w.f32(v);
                w.u8(static_cast<std::uint8_t>(c->value_type));
                break;
            }
            case rd::EMatNodeKind::Input:
                w.u8(static_cast<std::uint8_t>(static_cast<const rd::InputNode*>(n)->input));
                break;
            case rd::EMatNodeKind::SampleTexture:
                w.u32(static_cast<const rd::SampleTextureNode*>(n)->texture_slot);
                break;
            case rd::EMatNodeKind::Param:
            {
                auto* p = static_cast<const rd::ParamNode*>(n);
                w.u32(p->param_slot);
                w.u8(static_cast<std::uint8_t>(p->type));
                break;
            }
            case rd::EMatNodeKind::Math:
            {
                auto* m = static_cast<const rd::MathNode*>(n);
                w.u8(static_cast<std::uint8_t>(m->op));
                w.u8(static_cast<std::uint8_t>(m->operand_type));
                break;
            }
            case rd::EMatNodeKind::Swizzle:
            {
                auto* s = static_cast<const rd::SwizzleNode*>(n);
                w.u8(static_cast<std::uint8_t>(s->source_type));
                w.u8(static_cast<std::uint8_t>(s->out_type));
                for (std::uint8_t c : s->components) w.u8(c);
                break;
            }
            case rd::EMatNodeKind::Construct:
                w.u8(static_cast<std::uint8_t>(static_cast<const rd::ConstructNode*>(n)->out_type));
                break;
            case rd::EMatNodeKind::DecodeNormal:
            case rd::EMatNodeKind::TbnTransform:
            case rd::EMatNodeKind::OutputSurface:
            default:
                break;  // no payload
            }
        }

        // ── decode: construct a node by kind + read its payload off the reader ─
        std::unique_ptr<rd::Node> readNode(ByteReader& c, std::uint16_t kind_raw)
        {
            const auto kind = static_cast<rd::EMatNodeKind>(kind_raw);
            switch (kind)
            {
            case rd::EMatNodeKind::Constant:
            {
                auto n = std::make_unique<rd::ConstantNode>();
                float v[4]{}; for (float& f : v) c.f32(f);
                std::uint8_t vt{}; c.u8(vt);
                n->setType(static_cast<rd::EMatValueType>(vt));
                for (int k = 0; k < 4; ++k) n->value[k] = v[k];
                return n;
            }
            case rd::EMatNodeKind::Input:
            {
                auto n = std::make_unique<rd::InputNode>();
                std::uint8_t in{}; c.u8(in);
                n->input = static_cast<rd::EMaterialInput>(in);
                return n;
            }
            case rd::EMatNodeKind::SampleTexture:
            {
                auto n = std::make_unique<rd::SampleTextureNode>();
                c.u32(n->texture_slot);
                return n;
            }
            case rd::EMatNodeKind::Param:
            {
                std::uint32_t slot{}; c.u32(slot);
                std::uint8_t t{}; c.u8(t);
                auto n = std::make_unique<rd::ParamNode>(static_cast<rd::EMatValueType>(t));
                n->param_slot = slot;
                return n;
            }
            case rd::EMatNodeKind::Math:
            {
                std::uint8_t op{}, ot{}; c.u8(op); c.u8(ot);
                auto n = std::make_unique<rd::MathNode>(static_cast<rd::EMathOp>(op));
                n->setOperandType(static_cast<rd::EMatValueType>(ot));
                return n;
            }
            case rd::EMatNodeKind::Swizzle:
            {
                std::uint8_t st{}, ot{}; c.u8(st); c.u8(ot);
                auto n = std::make_unique<rd::SwizzleNode>(static_cast<rd::EMatValueType>(st),
                                                           static_cast<rd::EMatValueType>(ot));
                for (int k = 0; k < 4; ++k) { std::uint8_t comp{}; c.u8(comp); n->components[k] = comp; }
                return n;
            }
            case rd::EMatNodeKind::Construct:
            {
                std::uint8_t ot{}; c.u8(ot);
                return std::make_unique<rd::ConstructNode>(static_cast<rd::EMatValueType>(ot));
            }
            case rd::EMatNodeKind::DecodeNormal:  return std::make_unique<rd::DecodeNormalNode>();
            case rd::EMatNodeKind::TbnTransform:  return std::make_unique<rd::TbnTransformNode>();
            case rd::EMatNodeKind::OutputSurface: return std::make_unique<rd::OutputSurfaceNode>();
            default:                              return nullptr;
            }
        }

        // Buffered per-input pin source (resolved in pass 2 after all nodes exist).
        struct PinSrc
        {
            bool          has_source = false;
            std::uint64_t src_node   = 0;
            std::uint32_t src_pin    = 0;
            float         constant[4]{};
        };
    } // namespace

    std::vector<std::byte> encodeMaterialGraph(const rd::MaterialGraph& g)
    {
        ByteWriter w;
        w.reserve(256);

        w.u32(kMatGraphMagic);
        w.u32(kMatGraphEndianTag);
        w.u32(kMatGraphVersion);

        w.u8(static_cast<std::uint8_t>(g.shading_model));
        w.u8(static_cast<std::uint8_t>(g.render_state.alpha_mode));
        w.f32(g.render_state.alpha_cutoff);
        w.u8(g.render_state.double_sided ? 1u : 0u);

        w.u32(static_cast<std::uint32_t>(g.texture_slots.size()));
        for (const auto& t : g.texture_slots)
            w.str(t.name);

        w.u32(static_cast<std::uint32_t>(g.param_slots.size()));
        for (const auto& p : g.param_slots)
        {
            w.str(p.name);
            w.u8(static_cast<std::uint8_t>(p.type));
            for (float v : p.dflt) w.f32(v);
        }

        // Nodes in ascending-id order — deterministic (same graph -> same bytes).
        std::vector<rd::node_id> ids;
        ids.reserve(g.nodes().size());
        for (const auto& [id, n] : g.nodes()) ids.push_back(id);
        std::sort(ids.begin(), ids.end());

        w.u32(static_cast<std::uint32_t>(ids.size()));
        for (rd::node_id id : ids)
        {
            const rd::Node* n = g.node(id);
            if (!n) continue;
            w.u64(id);
            w.u16(static_cast<std::uint16_t>(n->kind()));
            w.str(n->name());
            // v2: editor canvas position (inside the node record — decode
            // remaps node ids, so an id-keyed side table would dangle).
            w.u8(n->ui_placed ? 1u : 0u);
            w.f32(n->ui_pos[0]);
            w.f32(n->ui_pos[1]);
            writePayload(w, n);

            w.u32(static_cast<std::uint32_t>(n->inputs().size()));
            for (const rd::DataPin& pin : n->inputs())
            {
                if (pin.source.valid())
                {
                    w.u8(1u);
                    w.u64(pin.source.node);
                    w.u32(pin.source.pin);
                }
                else
                {
                    w.u8(0u);
                    for (float v : pin.constant) w.f32(v);
                }
            }
        }

        w.u32(kMatGraphTrailer);
        return std::move(w).take();
    }

    bool decodeMaterialGraph(std::span<const std::byte> blob,
                             rd::MaterialGraph&         out,
                             std::string*               error_out) noexcept
    {
        ByteReader c{blob, error_out};

        std::uint32_t magic = 0, endian = 0, version = 0;
        if (!c.u32(magic))   return false;
        if (magic != kMatGraphMagic)        { c.fail("bad magic");        return false; }
        if (!c.u32(endian))  return false;
        if (endian != kMatGraphEndianTag)   { c.fail("bad endian tag");   return false; }
        if (!c.u32(version)) return false;
        // Lenient version RANGE: v1 blobs (no node positions) keep decoding —
        // their nodes land unplaced and the editor's grid layout takes over.
        if (version < 1u || version > kMatGraphVersion)
        {
            c.fail("schema version mismatch");
            return false;
        }

        rd::MaterialGraph g;  // fresh; whole-graph move into out at the end

        std::uint8_t sm = 0, am = 0, ds = 0;
        if (!c.u8(sm)) return false;
        g.shading_model = static_cast<rd::EMaterialShadingModel>(sm);
        if (!c.u8(am)) return false;
        g.render_state.alpha_mode = static_cast<rd::EAlphaMode>(am);
        if (!c.f32(g.render_state.alpha_cutoff)) return false;
        if (!c.u8(ds)) return false;
        g.render_state.double_sided = (ds != 0);

        std::uint32_t tcount = 0;
        if (!c.u32(tcount)) return false;
        if (tcount > kMaxMatGraphSlots) { c.fail("texture slot count too large"); return false; }
        for (std::uint32_t i = 0; i < tcount; ++i)
        {
            rd::TextureSlotDecl t;
            if (!c.str(t.name, kMaxMatGraphName)) return false;
            g.texture_slots.push_back(std::move(t));
        }

        std::uint32_t pcount = 0;
        if (!c.u32(pcount)) return false;
        if (pcount > kMaxMatGraphSlots) { c.fail("param slot count too large"); return false; }
        for (std::uint32_t i = 0; i < pcount; ++i)
        {
            rd::ParamSlotDecl p;
            if (!c.str(p.name, kMaxMatGraphName)) return false;
            std::uint8_t t = 0;
            if (!c.u8(t)) return false;
            p.type = static_cast<rd::EMatValueType>(t);
            for (int k = 0; k < 4; ++k)
                if (!c.f32(p.dflt[k])) return false;
            g.param_slots.push_back(std::move(p));
        }

        std::uint32_t ncount = 0;
        if (!c.u32(ncount)) return false;
        if (ncount > kMaxMatGraphNodes) { c.fail("node count too large"); return false; }

        // Pass 1: create nodes (addNode mints a FRESH id; remap old->new) + buffer
        // each node's input-pin sources for pass 2 (sources may forward-reference
        // a node not yet created).
        std::unordered_map<std::uint64_t, rd::node_id> remap;
        struct Buffered { rd::node_id new_id; std::vector<PinSrc> pins; };
        std::vector<Buffered> buffered;
        buffered.reserve(ncount);

        for (std::uint32_t i = 0; i < ncount; ++i)
        {
            std::uint64_t old_id = 0;
            std::uint16_t kind   = 0;
            if (!c.u64(old_id)) return false;
            if (!c.u16(kind))   return false;
            std::string name;
            if (!c.str(name, kMaxMatGraphName)) return false;

            // v2+: canvas position; v1 nodes stay unplaced (grid layout).
            std::uint8_t ui_placed = 0;
            float        ui_x = 0.0f, ui_y = 0.0f;
            if (version >= 2u)
            {
                if (!c.u8(ui_placed)) return false;
                if (!c.f32(ui_x))     return false;
                if (!c.f32(ui_y))     return false;
            }

            auto node = readNode(c, kind);
            if (!c.ok()) return false;
            if (!node) { c.fail("unknown node kind"); return false; }
            node->setName(std::move(name));
            node->ui_placed = (ui_placed != 0);
            node->ui_pos[0] = ui_x;
            node->ui_pos[1] = ui_y;

            std::uint32_t in_count = 0;
            if (!c.u32(in_count)) return false;
            if (in_count > kMaxMatGraphSlots) { c.fail("input pin count too large"); return false; }

            std::vector<PinSrc> pins;
            pins.reserve(in_count);
            for (std::uint32_t k = 0; k < in_count; ++k)
            {
                PinSrc ps;
                std::uint8_t has = 0;
                if (!c.u8(has)) return false;
                if (has)
                {
                    ps.has_source = true;
                    if (!c.u64(ps.src_node)) return false;
                    if (!c.u32(ps.src_pin))  return false;
                }
                else
                {
                    for (int j = 0; j < 4; ++j)
                        if (!c.f32(ps.constant[j])) return false;
                }
                pins.push_back(ps);
            }

            const rd::node_id new_id = g.addNode(std::move(node));
            remap.emplace(old_id, new_id);
            buffered.push_back(Buffered{ new_id, std::move(pins) });
        }

        std::uint32_t trailer = 0;
        if (!c.u32(trailer)) return false;
        if (trailer != kMatGraphTrailer) { c.fail("bad trailer"); return false; }
        if (!c.ok()) return false;

        // Pass 2: restore each input pin's source (remapping the node id) / constant.
        for (const Buffered& b : buffered)
        {
            rd::Node* n = g.node(b.new_id);
            if (!n) continue;
            const std::size_t cnt = std::min(n->inputs().size(), b.pins.size());
            for (std::size_t k = 0; k < cnt; ++k)
            {
                rd::DataPin&  pin = n->inputs()[k];
                const PinSrc& ps  = b.pins[k];
                if (ps.has_source)
                {
                    auto it = remap.find(ps.src_node);
                    if (it != remap.end())
                        pin.source = rd::PinLink{ it->second, ps.src_pin };
                }
                else
                {
                    for (int j = 0; j < 4; ++j) pin.constant[j] = ps.constant[j];
                }
            }
        }

        out = std::move(g);
        return true;
    }
} // namespace lux::asset::detail
