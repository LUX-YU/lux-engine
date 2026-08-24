#include <lux/engine/authoring/assets/FlowGraphCodec.hpp>

#include <lux/engine/core/serialization/ByteIO.hpp>
#include <lux/engine/authoring/flowforge/ControlNode.hpp>
#include <lux/engine/authoring/flowforge/ArithmeticNode.hpp>
#include <lux/engine/authoring/flowforge/FunctionalNode.hpp>
#include <lux/engine/authoring/flowforge/ObjectNode.hpp>

#include <lux/cxx/binary/ScalarSchema.hpp>

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lux::authoring::detail
{
    using lux::core::serialization::ByteReader;
    using lux::core::serialization::ByteWriter;
    namespace
    {
        namespace ff = ::lux::flowforge;

        // ScalarSchema is the persistent identity. RefType pointers are used
        // only for the in-process bridge to FlowForge's reflected values.
        struct ScalarTypeEntry
        {
            lux::cxx::ScalarSchema    schema;
            const lux::meta::RefType* type;
            std::uint32_t             size;
        };

        template <typename T, lux::cxx::EScalarKind Kind>
        constexpr ScalarTypeEntry scalarEntry()
        {
            return ScalarTypeEntry{
                lux::cxx::ScalarSchema{Kind, 1u, 0u},
                &lux::meta::ref_type_of_v<T>,
                static_cast<std::uint32_t>(sizeof(T)) };
        }

        const auto& scalarTable()
        {
            static const ScalarTypeEntry table[]{
                scalarEntry<bool, lux::cxx::EScalarKind::BOOL>(),
                scalarEntry<int8_t, lux::cxx::EScalarKind::I8>(),
                scalarEntry<uint8_t, lux::cxx::EScalarKind::U8>(),
                scalarEntry<int16_t, lux::cxx::EScalarKind::I16>(),
                scalarEntry<uint16_t, lux::cxx::EScalarKind::U16>(),
                scalarEntry<int32_t, lux::cxx::EScalarKind::I32>(),
                scalarEntry<uint32_t, lux::cxx::EScalarKind::U32>(),
                scalarEntry<int64_t, lux::cxx::EScalarKind::I64>(),
                scalarEntry<uint64_t, lux::cxx::EScalarKind::U64>(),
                scalarEntry<float, lux::cxx::EScalarKind::F32>(),
                scalarEntry<double, lux::cxx::EScalarKind::F64>(),
            };
            return table;
        }

        const ScalarTypeEntry* scalarByType(
            const lux::meta::RefType* type)
        {
            if (!type)
            {
                return nullptr;
            }
            for (const auto& entry : scalarTable())
            {
                if (entry.type->hash == type->hash &&
                    entry.type->name == type->name)
                {
                    return &entry;
                }
            }
            return nullptr;
        }

        const ScalarTypeEntry* scalarBySchema(
            lux::cxx::ScalarSchema schema)
        {
            for (const auto& entry : scalarTable())
            {
                if (entry.schema == schema)
                {
                    return &entry;
                }
            }
            return nullptr;
        }

        void writeSchema(ByteWriter& writer, lux::cxx::ScalarSchema schema)
        {
            writer.u8(static_cast<std::uint8_t>(schema.kind));
            writer.u8(schema.major);
            writer.u8(schema.minor);
        }

        bool readSchema(ByteReader& reader, lux::cxx::ScalarSchema& schema)
        {
            std::uint8_t kind = 0u;
            if (!reader.u8(kind) || !reader.u8(schema.major) ||
                !reader.u8(schema.minor))
            {
                return false;
            }
            schema.kind = static_cast<lux::cxx::EScalarKind>(kind);
            if (!schema.isKnown() || schema.major != 1u || schema.minor != 0u)
            {
                reader.fail("unknown scalar schema");
                return false;
            }
            return true;
        }

        bool writeScalarType(
            ByteWriter& writer,
            const lux::meta::RefType* type)
        {
            const auto* entry = scalarByType(type);
            if (!entry)
            {
                return false;
            }
            writeSchema(writer, entry->schema);
            return true;
        }

        const lux::meta::RefType* readScalarType(ByteReader& reader)
        {
            lux::cxx::ScalarSchema schema;
            if (!readSchema(reader, schema))
            {
                return nullptr;
            }
            const auto* entry = scalarBySchema(schema);
            if (!entry)
            {
                reader.fail("unsupported scalar schema");
                return nullptr;
            }
            return entry->type;
        }

        lux::meta::RuntimeObject makeScalarObject(
            lux::cxx::ScalarSchema schema,
            const void* raw)
        {
            using lux::meta::RuntimeObject;
            auto load = [&](auto tag) {
                using T = decltype(tag);
                T v{};
                std::memcpy(&v, raw, sizeof(T));
                return RuntimeObject(v);
            };
            switch (schema.kind)
            {
            case lux::cxx::EScalarKind::BOOL: return load(bool{});
            case lux::cxx::EScalarKind::I8: return load(int8_t{});
            case lux::cxx::EScalarKind::U8: return load(uint8_t{});
            case lux::cxx::EScalarKind::I16: return load(int16_t{});
            case lux::cxx::EScalarKind::U16: return load(uint16_t{});
            case lux::cxx::EScalarKind::I32: return load(int32_t{});
            case lux::cxx::EScalarKind::U32: return load(uint32_t{});
            case lux::cxx::EScalarKind::I64: return load(int64_t{});
            case lux::cxx::EScalarKind::U64: return load(uint64_t{});
            case lux::cxx::EScalarKind::F32: return load(float{});
            case lux::cxx::EScalarKind::F64: return load(double{});
            default: return {};
            }
        }

        // ── constant payload (variables' defaults + data-in pin constants) ──
        bool writeConstant(ByteWriter& w, const lux::meta::RuntimeObject& obj,
                           std::string* err)
        {
            const auto* t = obj.type();
            if (!t || !obj.isValid())
            {
                if (err) *err = "invalid constant value";
                return false;
            }
            const auto& string_type = lux::meta::ref_type_of_v<std::string>;
            if (t->hash == string_type.hash && t->name == string_type.name)
            {
                writeSchema(w, lux::cxx::ScalarSchema{
                    lux::cxx::EScalarKind::UTF8, 1u, 0u});
                w.str(*static_cast<const std::string*>(obj.data()));
                return true;
            }
            const ScalarTypeEntry* e = scalarByType(t);
            if (!e)
            {
                if (err) *err = "constant type is not serializable (scalar/string only)";
                return false;
            }
            writeSchema(w, e->schema);
            w.bytes(obj.data(), e->size);
            return true;
        }

        bool readConstant(ByteReader& c, lux::meta::RuntimeObject& out)
        {
            lux::cxx::ScalarSchema schema;
            if (!readSchema(c, schema))
            {
                return false;
            }
            if (schema.kind == lux::cxx::EScalarKind::UTF8)
            {
                std::string s;
                if (!c.str(s, kMaxFlowGraphName))
                {
                    return false;
                }
                out = lux::meta::RuntimeObject(std::move(s));
                return true;
            }
            const ScalarTypeEntry* e = scalarBySchema(schema);
            if (!e) { c.fail("unknown constant type"); return false; }
            std::uint8_t raw[8]{};
            if (e->size > sizeof(raw)) { c.fail("constant too large"); return false; }
            if (!c.bytes(raw, e->size))
            {
                return false;
            }
            out = makeScalarObject(schema, raw);
            if (!out.isValid()) { c.fail("unknown constant type"); return false; }
            return true;
        }

        // ── op classification for the node payload ──────────────────────────
        bool isBinaryOp(ff::ENodeOperation op)
        {
            switch (op)
            {
            case ff::ENodeOperation::ADD:
            case ff::ENodeOperation::SUBTRACT:
            case ff::ENodeOperation::MULTIPLY:
            case ff::ENodeOperation::DIVIDE:
            case ff::ENodeOperation::MODULO:
            case ff::ENodeOperation::LOGICAL_AND:
            case ff::ENodeOperation::LOGICAL_OR:
            case ff::ENodeOperation::CMP_EQ:
            case ff::ENodeOperation::CMP_NE:
            case ff::ENodeOperation::CMP_LT:
            case ff::ENodeOperation::CMP_LE:
            case ff::ENodeOperation::CMP_GT:
            case ff::ENodeOperation::CMP_GE:
                return true;
            default:
                return false;
            }
        }

        bool isUnaryOp(ff::ENodeOperation op)
        {
            return op == ff::ENodeOperation::NEGATE
                || op == ff::ENodeOperation::LOGICAL_NOT;
        }

        bool isPlainOp(ff::ENodeOperation op)
        {
            switch (op)
            {
            case ff::ENodeOperation::START:
            case ff::ENodeOperation::BRANCH:
            case ff::ENodeOperation::FOR_LOOP:
            case ff::ENodeOperation::WHILE_LOOP:
            case ff::ENodeOperation::RETURN:
            case ff::ENodeOperation::BREAK:
                return true;
            default:
                return false;
            }
        }

        std::unique_ptr<ff::Node> makePlainNode(ff::ENodeOperation op)
        {
            switch (op)
            {
            case ff::ENodeOperation::START:      return std::make_unique<ff::StartNode>();
            case ff::ENodeOperation::BRANCH:     return std::make_unique<ff::BranchNode>();
            case ff::ENodeOperation::FOR_LOOP:   return std::make_unique<ff::ForLoopNode>();
            case ff::ENodeOperation::WHILE_LOOP: return std::make_unique<ff::WhileLoopNode>();
            case ff::ENodeOperation::RETURN:     return std::make_unique<ff::ReturnNode>();
            case ff::ENodeOperation::BREAK:      return std::make_unique<ff::BreakNode>();
            default:                             return nullptr;
            }
        }

        // Ordinal of `pin` inside `pins`; kMaxFlowGraphPins when absent.
        std::uint16_t ordinalOf(const std::vector<ff::Pin*>& pins, const ff::Pin* pin)
        {
            for (std::size_t i = 0; i < pins.size(); ++i)
                if (pins[i] == pin)
                {
                    return static_cast<std::uint16_t>(i);
                }
            return static_cast<std::uint16_t>(kMaxFlowGraphPins);
        }
    } // namespace

    std::vector<std::byte> encodeFlowGraph(const ff::FlowGraph& g,
                                           std::string*         error_out)
    {
        auto fail = [&](const char* msg) {
            if (error_out) *error_out = msg;
            return std::vector<std::byte>{};
        };

        ByteWriter w;
        w.reserve(512);
        w.u32(kFlowGraphMagic);
        w.u32(kFlowGraphEndianTag);
        w.u32(kFlowGraphVersion);

        // ── variables ────────────────────────────────────────────────────
        w.u32(static_cast<std::uint32_t>(g.variables().size()));
        for (const auto& v : g.variables())
        {
            if (!v.type)
            {
                return fail("graph variable has no type");
            }
            w.u64(v.id);
            w.str(v.name);
            if (!writeScalarType(w, v.type))
            {
                return fail("graph variable has an unsupported type");
            }
            std::string err;
            if (!writeConstant(w, v.default_value, &err))
            {
                return fail("variable default: unsupported constant");
            }
        }

        // ── nodes, ascending stable id (deterministic bytes) ─────────────
        std::vector<const ff::Node*> nodes;
        nodes.reserve(g.nodes().size());
        for (const auto& storage : g.nodes())
            if (storage.node)
            {
                nodes.push_back(storage.node.get());
            }
        std::sort(nodes.begin(), nodes.end(),
                  [](const ff::Node* a, const ff::Node* b) { return a->id() < b->id(); });

        w.u32(static_cast<std::uint32_t>(nodes.size()));
        for (const ff::Node* n : nodes)
        {
            const auto op = n->operation();
            w.u64(n->id());
            w.u16(static_cast<std::uint16_t>(op));
            w.str(n->name());
            w.u8(n->ui_placed ? 1u : 0u);
            w.f32(n->ui_pos[0]);
            w.f32(n->ui_pos[1]);

            if (isPlainOp(op))
            {
                // no payload
            }
            else if (op == ff::ENodeOperation::SEQUENCE)
            {
                const auto& seq = static_cast<const ff::SequenceNode&>(*n);
                w.u16(static_cast<std::uint16_t>(seq.execOutPins().size()));
            }
            else if (isBinaryOp(op))
            {
                const auto& bin = static_cast<const ff::BinaryOpNode&>(*n);
                if (!bin.operandType())
                {
                    return fail("binary op node has no operand type");
                }
                if (!writeScalarType(w, bin.operandType()))
                {
                    return fail("binary op node has an unsupported operand type");
                }
            }
            else if (isUnaryOp(op))
            {
                const auto& un = static_cast<const ff::UnaryOpNode&>(*n);
                if (!un.operandType())
                {
                    return fail("unary op node has no operand type");
                }
                if (!writeScalarType(w, un.operandType()))
                {
                    return fail("unary op node has an unsupported operand type");
                }
            }
            else if (op == ff::ENodeOperation::GET_VARIABLE)
            {
                w.u64(static_cast<const ff::GetVariableNode&>(*n).variableId());
            }
            else if (op == ff::ENodeOperation::SET_VARIABLE)
            {
                w.u64(static_cast<const ff::SetVariableNode&>(*n).variableId());
            }
            else if (op == ff::ENodeOperation::NATIVE_FUNC_CALL)
            {
                if (n->creatorName().empty())
                {
                    return fail("native call node was not created from the "
                                "NodeRegistry; it cannot be serialized");
                }
                w.str(n->creatorName());
            }
            else if (op == ff::ENodeOperation::FUNC_DEF_START)
            {
                const auto& def = static_cast<const ff::FuncDefNode&>(*n);
                w.u16(static_cast<std::uint16_t>(def.argInfos().size()));
                for (const auto& a : def.argInfos())
                {
                    if (!a.type)
                    {
                        return fail("function argument has no type");
                    }
                    if (!writeScalarType(w, a.type))
                    {
                        return fail("function argument has an unsupported type");
                    }
                    w.str(a.name);
                }
                w.u16(static_cast<std::uint16_t>(def.retInfos().size()));
                for (const auto& r : def.retInfos())
                {
                    if (!r.type)
                    {
                        return fail("function return value has no type");
                    }
                    if (!writeScalarType(w, r.type))
                    {
                        return fail("function return value has an unsupported type");
                    }
                    w.str(r.name);
                }
            }
            else if (op == ff::ENodeOperation::FUNC_RETURN)
            {
                const auto* def = static_cast<const ff::FuncReturnNode&>(*n).def();
                if (!def)
                {
                    return fail("function return has no owning definition");
                }
                w.u64(def->id());
            }
            else if (op == ff::ENodeOperation::GRAPH_FUNC_CALL)
            {
                const auto* def = static_cast<const ff::GraphFuncCallNode&>(*n).callee();
                if (!def)
                {
                    return fail("graph call has no callee");
                }
                w.u64(def->id());
            }
            else if (op == ff::ENodeOperation::GET_FIELD
                  || op == ff::ENodeOperation::SET_FIELD)
            {
                const lux::meta::RefClass* cls = nullptr;
                const lux::meta::RefField* field = nullptr;
                if (op == ff::ENodeOperation::GET_FIELD)
                {
                    const auto& gf = static_cast<const ff::GetFieldNode&>(*n);
                    cls = gf.ownerClass(); field = gf.field();
                }
                else
                {
                    const auto& sf = static_cast<const ff::SetFieldNode&>(*n);
                    cls = sf.ownerClass(); field = sf.field();
                }
                if (!cls || !field)
                {
                    return fail("field node has no reflection info");
                }
                w.str(cls->full_name);
                w.str(field->name);
            }
            else if (op == ff::ENodeOperation::ON_EVENT)
            {
                const auto& ev = static_cast<const ff::OnEventNode&>(*n);
                w.u16(static_cast<std::uint16_t>(ev.paramInfos().size()));
                for (const auto& p : ev.paramInfos())
                {
                    if (!p.type)
                    {
                        return fail("event parameter has no type");
                    }
                    if (!writeScalarType(w, p.type))
                    {
                        return fail("event parameter has an unsupported type");
                    }
                    w.str(p.name);
                }
            }
            else
            {
                return fail("node kind is not serializable yet");
            }
        }

        // ── links (source-ordered; deterministic) ────────────────────────
        struct WireLink
        {
            std::uint64_t src_node, dst_node;
            std::uint16_t src_ord, dst_ord;
        };
        std::vector<WireLink> links;
        for (const ff::Node* n : nodes)
        {
            const auto& outs = n->outPins();
            for (std::size_t i = 0; i < outs.size(); ++i)
            {
                const ff::Pin* p = outs[i];
                auto push = [&](const ff::Pin* dst) {
                    const ff::Node* dn = dst->node();
                    links.push_back(WireLink{
                        n->id(), dn->id(),
                        static_cast<std::uint16_t>(i),
                        ordinalOf(dn->inPins(), dst) });
                };
                if (p->kind() == ff::EPinKind::EXEC_OUT)
                {
                    if (const auto* dst =
                            static_cast<const ff::ExecOutPin*>(p)->nextPin())
                        push(dst);
                }
                else if (p->kind() == ff::EPinKind::DATA_OUT)
                {
                    for (const ff::DataInPin* dst :
                         static_cast<const ff::DataOutPin*>(p)->linkPins())
                        push(dst);
                }
            }
        }
        w.u32(static_cast<std::uint32_t>(links.size()));
        for (const WireLink& l : links)
        {
            w.u64(l.src_node);
            w.u16(l.src_ord);
            w.u64(l.dst_node);
            w.u16(l.dst_ord);
        }

        // ── data-in constants ─────────────────────────────────────────────
        struct WireConst
        {
            const ff::DataInPin* pin;
            std::uint64_t        node;
            std::uint16_t        ord;
        };
        std::vector<WireConst> consts;
        for (const ff::Node* n : nodes)
        {
            const auto& ins = n->inPins();
            for (std::size_t i = 0; i < ins.size(); ++i)
            {
                if (ins[i]->kind() != ff::EPinKind::DATA_IN)
                {
                    continue;
                }
                const auto* dpin = static_cast<const ff::DataInPin*>(ins[i]);
                if (!dpin->validConstant())
                {
                    continue;
                }
                consts.push_back(WireConst{
                    dpin, n->id(), static_cast<std::uint16_t>(i) });
            }
        }
        w.u32(static_cast<std::uint32_t>(consts.size()));
        for (const WireConst& cst : consts)
        {
            w.u64(cst.node);
            w.u16(cst.ord);
            std::string err;
            if (!writeConstant(w, cst.pin->constantData(), &err))
            {
                return fail("pin constant: unsupported constant type");
            }
        }

        w.u32(kFlowGraphTrailer);
        return std::move(w).take();
    }

    bool decodeFlowGraph(std::span<const std::byte>    blob,
                         ff::FlowGraph&                out,
                         ff::NodeRegistry&             registry,
                         std::string*                  error_out)
    {
        ByteReader c{blob, error_out};

        std::uint32_t magic = 0, endian = 0, version = 0;
        if (!c.u32(magic))
        {
            return false;
        }
        if (magic != kFlowGraphMagic)      { c.fail("bad magic");      return false; }
        if (!c.u32(endian))
        {
            return false;
        }
        if (endian != kFlowGraphEndianTag) { c.fail("bad endian tag"); return false; }
        if (!c.u32(version))
        {
            return false;
        }
        if (version != kFlowGraphVersion)
        {
            c.fail("schema version mismatch");
            return false;
        }

        ff::FlowGraph g;  // fresh; whole-graph move into out at the end

        // ── variables ─────────────────────────────────────────────────────
        std::uint32_t vcount = 0;
        if (!c.u32(vcount))
        {
            return false;
        }
        if (vcount > kMaxFlowGraphPins) { c.fail("variable count too large"); return false; }
        for (std::uint32_t i = 0; i < vcount; ++i)
        {
            std::uint64_t id = 0;
            std::string   name;
            if (!c.u64(id))
            {
                return false;
            }
            if (!c.str(name, kMaxFlowGraphName))
            {
                return false;
            }
            const auto* type = readScalarType(c);
            if (!type)
            {
                return false;
            }
            lux::meta::RuntimeObject dflt;
            if (!readConstant(c, dflt))
            {
                return false;
            }

            if (!g.addVariableWithId(
                    id, std::move(name), type, std::move(dflt)))
            {
                c.fail("duplicate variable id");
                return false;
            }
        }

        // ── nodes ─────────────────────────────────────────────────────────
        std::uint32_t ncount = 0;
        if (!c.u32(ncount))
        {
            return false;
        }
        if (ncount > kMaxFlowGraphNodes) { c.fail("node count too large"); return false; }

        std::unordered_map<std::uint64_t, ff::Node*> by_id;
        by_id.reserve(ncount);

        auto adopt = [&](std::uint64_t id, std::unique_ptr<ff::Node> node,
                         const std::string& display,
                         std::uint8_t placed, float x, float y) -> ff::Node* {
            node->setName(display);
            node->ui_placed = (placed != 0);
            node->ui_pos[0] = x;
            node->ui_pos[1] = y;
            ff::Node* raw = node.get();
            g.addNodesWithId(id, std::move(node));
            if (!by_id.emplace(id, raw).second)
            {
                return nullptr;  // duplicate id
            }
            return raw;
        };

        // FUNC_RETURN / GRAPH_FUNC_CALL reference a FuncDefNode by id —
        // buffer them and construct after every def exists.
        struct DeferredRec
        {
            std::uint64_t     id;
            ff::ENodeOperation op;
            std::string       display;
            std::uint8_t      placed;
            float             x, y;
            std::uint64_t     def_id;
        };
        std::vector<DeferredRec> deferred;

        // Shared signature reader (FUNC_DEF_START args/rets, ON_EVENT params).
        auto readSig = [&](std::vector<ff::FuncArgInfo>& sig) -> bool {
            std::uint16_t count = 0;
            if (!c.u16(count))
            {
                return false;
            }
            if (count > kMaxFlowGraphPins) { c.fail("signature too large"); return false; }
            for (std::uint16_t k = 0; k < count; ++k)
            {
                std::string   pname;
                const auto* type = readScalarType(c);
                if (!type)
                {
                    return false;
                }
                if (!c.str(pname, kMaxFlowGraphName))
                {
                    return false;
                }
                sig.push_back(ff::FuncArgInfo{type, std::move(pname)});
            }
            return true;
        };

        for (std::uint32_t i = 0; i < ncount; ++i)
        {
            std::uint64_t id = 0;
            std::uint16_t op_raw = 0;
            std::string   display;
            std::uint8_t  placed = 0;
            float         x = 0.0f, y = 0.0f;
            if (!c.u64(id))
            {
                return false;
            }
            if (!c.u16(op_raw))
            {
                return false;
            }
            if (!c.str(display, kMaxFlowGraphName))
            {
                return false;
            }
            if (!c.u8(placed))
            {
                return false;
            }
            if (!c.f32(x))
            {
                return false;
            }
            if (!c.f32(y))
            {
                return false;
            }

            const auto op = static_cast<ff::ENodeOperation>(op_raw);
            std::unique_ptr<ff::Node> node;

            if (isPlainOp(op))
            {
                node = makePlainNode(op);
            }
            else if (op == ff::ENodeOperation::SEQUENCE)
            {
                std::uint16_t extra = 0;
                if (!c.u16(extra))
                {
                    return false;
                }
                if (extra > kMaxFlowGraphPins) { c.fail("sequence pin count too large"); return false; }
                auto seq = std::make_unique<ff::SequenceNode>();
                for (std::uint16_t k = 0; k < extra; ++k)
                {
                    seq->addExecOutPin();
                }
                node = std::move(seq);
            }
            else if (isBinaryOp(op) || isUnaryOp(op))
            {
                const auto* type = readScalarType(c);
                if (!type)
                {
                    return false;
                }
                if (isBinaryOp(op))
                {
                    node = std::make_unique<ff::BinaryOpNode>(op, type);
                }
                else
                    node = std::make_unique<ff::UnaryOpNode>(op, type);
            }
            else if (op == ff::ENodeOperation::GET_VARIABLE
                  || op == ff::ENodeOperation::SET_VARIABLE)
            {
                std::uint64_t var_id = 0;
                if (!c.u64(var_id))
                {
                    return false;
                }
                const auto* var = g.findVariable(var_id);
                if (!var) { c.fail("node references an unknown variable"); return false; }
                const ff::DataPinInfo info{ var->name, var->type };
                if (op == ff::ENodeOperation::GET_VARIABLE)
                {
                    node = std::make_unique<ff::GetVariableNode>(var_id, info);
                }
                else
                    node = std::make_unique<ff::SetVariableNode>(var_id, info);
            }
            else if (op == ff::ENodeOperation::NATIVE_FUNC_CALL)
            {
                std::string creator;
                if (!c.str(creator, kMaxFlowGraphName))
                {
                    return false;
                }
                ff::NodeCreatInfo* info = registry.findNodeByName(creator);
                if (!info || !info->creator)
                {
                    c.fail("unknown node creator (native function not registered)");
                    return false;
                }
                node = info->creator();
                if (!node || node->operation() != ff::ENodeOperation::NATIVE_FUNC_CALL)
                {
                    c.fail("node creator mismatch");
                    return false;
                }
            }
            else if (op == ff::ENodeOperation::FUNC_DEF_START)
            {
                std::vector<ff::FuncArgInfo> args, rets;
                if (!readSig(args))
                {
                    return false;
                }
                if (!readSig(rets))
                {
                    return false;
                }
                node = std::make_unique<ff::FuncDefNode>(
                    display, std::move(args), std::move(rets));
            }
            else if (op == ff::ENodeOperation::ON_EVENT)
            {
                std::vector<ff::FuncArgInfo> params;
                if (!readSig(params))
                {
                    return false;
                }
                node = std::make_unique<ff::OnEventNode>(display, std::move(params));
            }
            else if (op == ff::ENodeOperation::GET_FIELD
                  || op == ff::ENodeOperation::SET_FIELD)
            {
                std::string cls_name, field_name;
                if (!c.str(cls_name, kMaxFlowGraphName))
                {
                    return false;
                }
                if (!c.str(field_name, kMaxFlowGraphName))
                {
                    return false;
                }
                const auto* cls =
                    lux::meta::ReflectionRegistry::instance().findClass(cls_name);
                if (!cls) { c.fail("field node references an unregistered class"); return false; }
                const lux::meta::RefField* field = nullptr;
                for (const auto& f : cls->fields)
                    if (f.name == field_name) { field = &f; break; }
                if (!field) { c.fail("field node references an unknown field"); return false; }
                if (op == ff::ENodeOperation::GET_FIELD)
                {
                    node = std::make_unique<ff::GetFieldNode>(*cls, *field);
                }
                else
                    node = std::make_unique<ff::SetFieldNode>(*cls, *field);
            }
            else if (op == ff::ENodeOperation::FUNC_RETURN
                  || op == ff::ENodeOperation::GRAPH_FUNC_CALL)
            {
                std::uint64_t def_id = 0;
                if (!c.u64(def_id))
                {
                    return false;
                }
                deferred.push_back(DeferredRec{ id, op, display, placed, x, y, def_id });
                continue;
            }
            else
            {
                c.fail("unknown node kind");
                return false;
            }

            if (!node) { c.fail("node construction failed"); return false; }
            if (!adopt(id, std::move(node), display, placed, x, y))
            {
                c.fail("duplicate node id");
                return false;
            }
        }

        // Pass 1b: nodes referencing a FuncDefNode.
        for (const DeferredRec& rec : deferred)
        {
            auto it = by_id.find(rec.def_id);
            if (it == by_id.end()
                || it->second->operation() != ff::ENodeOperation::FUNC_DEF_START)
            {
                c.fail("node references an unknown function definition");
                return false;
            }
            const auto& def = static_cast<const ff::FuncDefNode&>(*it->second);
            std::unique_ptr<ff::Node> node;
            if (rec.op == ff::ENodeOperation::FUNC_RETURN)
            {
                node = std::make_unique<ff::FuncReturnNode>(def);
            }
            else
                node = std::make_unique<ff::GraphFuncCallNode>(def);
            if (!adopt(rec.id, std::move(node), rec.display, rec.placed, rec.x, rec.y))
            {
                c.fail("duplicate node id");
                return false;
            }
        }

        // ── links ─────────────────────────────────────────────────────────
        std::uint32_t lcount = 0;
        if (!c.u32(lcount))
        {
            return false;
        }
        if (lcount > kMaxFlowGraphNodes) { c.fail("link count too large"); return false; }
        for (std::uint32_t i = 0; i < lcount; ++i)
        {
            std::uint64_t src_node = 0, dst_node = 0;
            std::uint16_t src_ord = 0, dst_ord = 0;
            if (!c.u64(src_node))
            {
                return false;
            }
            if (!c.u16(src_ord))
            {
                return false;
            }
            if (!c.u64(dst_node))
            {
                return false;
            }
            if (!c.u16(dst_ord))
            {
                return false;
            }

            auto s_it = by_id.find(src_node);
            auto d_it = by_id.find(dst_node);
            if (s_it == by_id.end() || d_it == by_id.end())
            {
                c.fail("link references an unknown node");
                return false;
            }
            const auto& outs = s_it->second->outPins();
            const auto& ins  = d_it->second->inPins();
            if (src_ord >= outs.size() || dst_ord >= ins.size())
            {
                c.fail("link references an unknown pin");
                return false;
            }
            ff::LastLink ll;
            if (outs[src_ord]->linkTo(ins[dst_ord], ll) != ff::ELinkError::SUCCESS)
            {
                c.fail("link rejected (pin kind/type mismatch — stale file?)");
                return false;
            }
        }

        // ── data-in constants ─────────────────────────────────────────────
        std::uint32_t ccount = 0;
        if (!c.u32(ccount))
        {
            return false;
        }
        if (ccount > kMaxFlowGraphNodes) { c.fail("constant count too large"); return false; }
        for (std::uint32_t i = 0; i < ccount; ++i)
        {
            std::uint64_t node_id = 0;
            std::uint16_t ord = 0;
            if (!c.u64(node_id))
            {
                return false;
            }
            if (!c.u16(ord))
            {
                return false;
            }
            lux::meta::RuntimeObject value;
            if (!readConstant(c, value))
            {
                return false;
            }

            auto it = by_id.find(node_id);
            if (it == by_id.end()) { c.fail("constant references an unknown node"); return false; }
            const auto& ins = it->second->inPins();
            if (ord >= ins.size() || ins[ord]->kind() != ff::EPinKind::DATA_IN)
            {
                c.fail("constant references a non-data pin");
                return false;
            }
            auto* dpin = static_cast<ff::DataInPin*>(ins[ord]);
            if (!dpin->setConstantData(std::move(value)))
            {
                c.fail("constant type does not match the pin");
                return false;
            }
        }

        std::uint32_t trailer = 0;
        if (!c.u32(trailer))
        {
            return false;
        }
        if (trailer != kFlowGraphTrailer) { c.fail("bad trailer"); return false; }
        if (!c.ok())
        {
            return false;
        }

        out = std::move(g);
        return true;
    }
} // namespace lux::authoring::detail
