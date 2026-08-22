// =============================================================================
//  flow_graph_codec_test — FlowGraphCodec round-trip regression (M-C).
//
//    1. Builds a representative graph: variables, control flow (for/branch),
//       arithmetic, Get/Set variable, a graph function (FuncDef + FuncReturn
//       + GraphFuncCall) and a registry-created native call.
//    2. encode -> decode -> re-encode: the second encode must be BYTE-EQUAL
//       to the first (stable ids + deterministic ordering make the codec a
//       fixed point).
//    3. Structural spot checks on the decoded graph (variables, node count,
//       links, constants, positions).
//    4. Defensive decode: truncated blob and bad magic must fail cleanly.
//
//  Self-checking: exit code 0 on success.
// =============================================================================

#include <cstdio>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lux/engine/authoring/assets/FlowGraphCodec.hpp>
#include <lux/engine/authoring/flowforge/ControlNode.hpp>
#include <lux/engine/authoring/flowforge/ArithmeticNode.hpp>
#include <lux/engine/authoring/flowforge/FunctionalNode.hpp>
#include <lux/engine/authoring/flowforge/ObjectNode.hpp>
#include <lux/engine/authoring/flowforge/NodeRegistry.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/cxx/binary/Binary.hpp>

using namespace lux::flowforge;

static int g_failed = 0;
static void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_failed;
}

// A native signature registered into the GLOBAL registry, mirroring how the
// editor exposes native calls (the codec re-instantiates by creator name).
static lux::meta::RefFunction makeSinkIntFn()
{
    lux::meta::RefFunction fn{};
    fn.invokable.name        = "lux_test_sink_int";
    fn.invokable.full_name   = "lux_test_sink_int";
    fn.invokable.return_type = lux::meta::ref_type_of_v<void>;
    fn.invokable.parameters  = {
        lux::meta::RefParam{ "value", lux::meta::ref_type_of_v<int> },
    };
    return fn;
}

static void registerTestNatives()
{
    static lux::meta::RefFunction sink_fn = makeSinkIntFn();
    auto info      = std::make_unique<NodeCreatInfo>();
    info->name     = "Test Sink";
    info->category = "Test";
    info->creator  = []() -> std::unique_ptr<Node> {
        return std::make_unique<NativeFuncCall>(sink_fn);
    };
    NodeRegistry::global().registerNode(std::move(info));  // dup-safe
}

static bool replaceLengthPrefixedString(
    std::vector<std::byte>& blob,
    std::string_view current,
    std::string_view replacement)
{
    if (current.empty() || blob.size() < current.size() + sizeof(std::uint32_t))
        return false;

    for (std::size_t offset = sizeof(std::uint32_t);
         offset + current.size() <= blob.size();
         ++offset)
    {
        const bool matches = std::equal(
            current.begin(),
            current.end(),
            blob.begin() + static_cast<std::ptrdiff_t>(offset),
            [](char lhs, std::byte rhs)
            {
                return static_cast<unsigned char>(lhs) ==
                    std::to_integer<unsigned char>(rhs);
            });
        if (!matches)
            continue;

        const auto length_offset = offset - sizeof(std::uint32_t);
        const auto encoded_length =
            std::to_integer<std::uint32_t>(blob[length_offset]) |
            (std::to_integer<std::uint32_t>(blob[length_offset + 1u]) << 8u) |
            (std::to_integer<std::uint32_t>(blob[length_offset + 2u]) << 16u) |
            (std::to_integer<std::uint32_t>(blob[length_offset + 3u]) << 24u);
        if (encoded_length != current.size())
            continue;

        const auto replacement_length =
            static_cast<std::uint32_t>(replacement.size());
        for (std::size_t byte = 0u; byte < sizeof(std::uint32_t); ++byte)
        {
            blob[length_offset + byte] = std::byte{
                static_cast<unsigned char>(
                    (replacement_length >> (byte * 8u)) & 0xffu)};
        }
        const auto begin = blob.begin() + static_cast<std::ptrdiff_t>(offset);
        blob.erase(begin, begin + static_cast<std::ptrdiff_t>(current.size()));
        std::vector<std::byte> replacement_bytes;
        replacement_bytes.reserve(replacement.size());
        for (const char value : replacement)
        {
            replacement_bytes.push_back(std::byte{
                static_cast<unsigned char>(value)});
        }
        blob.insert(
            blob.begin() + static_cast<std::ptrdiff_t>(offset),
            replacement_bytes.begin(),
            replacement_bytes.end());
        return true;
    }
    return false;
}

static FlowGraph buildRepresentativeGraph()
{
    const auto* i32 = &lux::meta::ref_type_of_v<int32_t>;

    FlowGraph g;
    const uint64_t sum = g.addVariable(
        "sum", i32, lux::meta::RuntimeObject(int32_t{0}));
    const DataPinInfo sum_info{"sum", i32};

    // fib-shaped function (signature only exercises FuncDef/Return/Call wiring)
    auto def  = std::make_unique<FuncDefNode>(
        "helper",
        std::vector<FuncArgInfo>{ { i32, "n" } },
        std::vector<FuncArgInfo>{ { i32, "value" } });
    auto fret = std::make_unique<FuncReturnNode>(*def);
    auto fcall = std::make_unique<GraphFuncCallNode>(*def);

    auto start  = std::make_unique<StartNode>();
    auto loop   = std::make_unique<ForLoopNode>();
    auto branch = std::make_unique<BranchNode>();
    auto setsum = std::make_unique<SetVariableNode>(sum, sum_info);
    auto getsum = std::make_unique<GetVariableNode>(sum, sum_info);
    auto add    = std::make_unique<BinaryOpNode>(ENodeOperation::ADD, i32);
    auto mod    = std::make_unique<BinaryOpNode>(ENodeOperation::MODULO, i32);
    auto cmp    = std::make_unique<BinaryOpNode>(ENodeOperation::CMP_EQ, i32);
    auto seq    = std::make_unique<SequenceNode>();
    seq->addExecOutPin();  // one extra leg -> exercises the SEQUENCE payload
    auto ret    = std::make_unique<ReturnNode>();

    // Native call via the registry (stamps the creator name).
    auto* sink_info = NodeRegistry::global().findNodeByName("Test Sink");
    auto sink = sink_info->creator();
    auto* sink_call = static_cast<NativeFuncCall*>(sink.get());

    const_cast<DataInPin&>(mod->rhs()).setConstantData(lux::meta::RuntimeObject(int32_t{2}));
    const_cast<DataInPin&>(cmp->rhs()).setConstantData(lux::meta::RuntimeObject(int32_t{0}));

    LastLink ll;
    // fn body: def -> fret (returns n)
    def->execOutPin().linkTo(&fret->execInPin(), ll);
    const_cast<DataOutPin&>(*def->argPins()[0]).linkTo(fret->retPins()[0].get(), ll);

    // main: start -> for; body -> branch(iv%2==0) then: set sum = sum + iv
    //       completed -> seq -> [call helper -> sink(result)] ...
    start->execOutPin().linkTo(&loop->execInPin(), ll);
    loop->execOutPin().linkTo(&branch->execInPin(), ll);
    const_cast<ExecOutPin&>(branch->execOutPinUp()).linkTo(&setsum->execInPin(), ll);
    const_cast<ExecOutPin&>(loop->completed()).linkTo(&seq->execInPin(), ll);
    seq->execOutPin().linkTo(&fcall->execInPin(), ll);
    fcall->execOutPin().linkTo(&sink_call->execInPin(), ll);
    sink_call->execOutPin().linkTo(&ret->execInPin(), ll);

    const_cast<DataOutPin&>(loop->indexPin()).linkTo(const_cast<DataInPin*>(&mod->lhs()), ll);
    const_cast<DataOutPin&>(mod->result()).linkTo(const_cast<DataInPin*>(&cmp->lhs()), ll);
    const_cast<DataOutPin&>(cmp->result()).linkTo(const_cast<DataInPin*>(&branch->dataInPin()), ll);
    const_cast<DataOutPin&>(getsum->valuePin()).linkTo(const_cast<DataInPin*>(&add->lhs()), ll);
    const_cast<DataOutPin&>(loop->indexPin()).linkTo(const_cast<DataInPin*>(&add->rhs()), ll);
    const_cast<DataOutPin&>(add->result()).linkTo(const_cast<DataInPin*>(&setsum->valueIn()), ll);
    const_cast<DataOutPin&>(*fcall->resultPins()[0]).linkTo(
        sink_call->dataInPins()[0].get(), ll);

    // canvas positions on a couple of nodes
    start->ui_placed = true; start->ui_pos[0] = 60.0f;  start->ui_pos[1] = 140.0f;
    loop->ui_placed  = true; loop->ui_pos[0]  = 320.0f; loop->ui_pos[1]  = 140.0f;

    g.addNodes(std::move(def));
    g.addNodes(std::move(fret));
    g.addNodes(std::move(fcall));
    g.addNodes(std::move(start));
    g.addNodes(std::move(loop));
    g.addNodes(std::move(branch));
    g.addNodes(std::move(setsum));
    g.addNodes(std::move(getsum));
    g.addNodes(std::move(add));
    g.addNodes(std::move(mod));
    g.addNodes(std::move(cmp));
    g.addNodes(std::move(seq));
    g.addNodes(std::move(sink));
    g.addNodes(std::move(ret));
    return g;
}

int main()
{
    lux::meta::meta_module_init();
    registerTestNatives();
    std::printf("FlowGraph codec test\n====================\n");

    FlowGraph g = buildRepresentativeGraph();

    // ---- 1/2. encode -> decode -> re-encode fixed point ----------------------
    std::string err;
    const auto blob1 = lux::authoring::detail::encodeFlowGraph(g, &err);
    check(!blob1.empty(), ("encode succeeded: " + err).c_str());

    FlowGraph decoded;
    check(lux::authoring::detail::decodeFlowGraph(
              std::span<const std::byte>(blob1), decoded,
              NodeRegistry::global(), &err),
          ("decode succeeded: " + err).c_str());

    const auto blob2 = lux::authoring::detail::encodeFlowGraph(decoded, &err);
    check(!blob2.empty(), "re-encode succeeded");
    check(blob1 == blob2, "re-encode is byte-equal (codec is a fixed point)");

    // ---- 3. structural spot checks -------------------------------------------
    check(decoded.variables().size() == 1
              && decoded.variables()[0].name == "sum",
          "variable table round-tripped");
    check(decoded.nodes().size() == g.nodes().size(),
          "node count round-tripped");

    bool found_placed_start = false;
    bool found_native       = false;
    for (const auto& storage : decoded.nodes())
    {
        const Node* n = storage.node.get();
        if (!n) continue;
        if (n->operation() == ENodeOperation::START && n->ui_placed
            && n->ui_pos[0] == 60.0f && n->ui_pos[1] == 140.0f)
            found_placed_start = true;
        if (n->operation() == ENodeOperation::NATIVE_FUNC_CALL
            && n->creatorName() == "Test Sink")
            found_native = true;
    }
    check(found_placed_start, "canvas position round-tripped");
    check(found_native,       "native call re-instantiated via the registry");

    // Field nodes persist the canonical reflected class name. Math values are
    // now owned and named by Core Math; the old Resource name is a hard cut.
    const auto* position_class =
        lux::meta::ReflectionRegistry::instance().findClass(
            "lux::math::Position3d");
    const lux::meta::RefField* x_field = nullptr;
    if (position_class)
    {
        const auto found = std::ranges::find(
            position_class->fields,
            std::string_view{"x"},
            &lux::meta::RefField::name);
        if (found != position_class->fields.end())
            x_field = &*found;
    }
    check(position_class != nullptr && x_field != nullptr,
          "canonical Math reflection class and field are registered");
    if (position_class && x_field)
    {
        static lux::meta::RefFunction position_source{};
        position_source.invokable.name = "lux_test_position_source";
        position_source.invokable.full_name = "lux_test_position_source";
        position_source.invokable.return_type = position_class->type;
        auto position_source_info = std::make_unique<NodeCreatInfo>();
        position_source_info->name = "Test Position Source";
        position_source_info->category = "Test";
        position_source_info->creator = []() -> std::unique_ptr<Node>
        {
            return std::make_unique<NativeFuncCall>(position_source);
        };
        NodeRegistry::global().registerNode(std::move(position_source_info));

        FlowGraph field_graph;
        auto* source_info =
            NodeRegistry::global().findNodeByName("Test Position Source");
        auto source = source_info->creator();
        auto* source_call = static_cast<NativeFuncCall*>(source.get());
        auto get_field =
            std::make_unique<GetFieldNode>(*position_class, *x_field);
        LastLink field_link;
        const_cast<DataOutPin&>(source_call->result()).linkTo(
            const_cast<DataInPin*>(&get_field->objectPin()),
            field_link);
        field_graph.addNodes(std::move(source));
        field_graph.addNodes(std::move(get_field));
        const auto field_blob =
            lux::authoring::detail::encodeFlowGraph(field_graph, &err);
        check(!field_blob.empty(), ("Math field node encoded: " + err).c_str());

        FlowGraph decoded_field_graph;
        check(lux::authoring::detail::decodeFlowGraph(
                  std::span<const std::byte>(field_blob),
                  decoded_field_graph,
                  NodeRegistry::global(),
                  &err),
              ("Math field node decoded: " + err).c_str());
        const auto field_blob_roundtrip =
            lux::authoring::detail::encodeFlowGraph(decoded_field_graph, &err);
        check(field_blob == field_blob_roundtrip,
              "Math field node re-encodes byte-for-byte");

        auto legacy_field_blob = field_blob;
        const std::string legacy_name =
            std::string{"lux::"} + "spa" + "tial::" +
            "Position" + "3D";
        check(replaceLengthPrefixedString(
                  legacy_field_blob,
                  position_class->full_name,
                  legacy_name),
              "legacy class-name payload constructed");
        FlowGraph rejected_field_graph;
        check(!lux::authoring::detail::decodeFlowGraph(
                  std::span<const std::byte>(legacy_field_blob),
                  rejected_field_graph,
                  NodeRegistry::global(),
                  &err),
              "legacy Resource field-node class name is rejected");
    }

    // The first variable's persistent type identity is the frozen
    // ScalarSchema triplet, never a compiler-derived type_hash.
    lux::cxx::BinaryReader wire{blob1, blob1.size()};
    std::uint32_t wire_u32 = 0u;
    std::uint64_t wire_u64 = 0u;
    std::string_view wire_name;
    bool schema_located = wire.readUnsigned(wire_u32) &&
        wire.readUnsigned(wire_u32) &&
        wire.readUnsigned(wire_u32) &&
        wire.readUnsigned(wire_u32) &&
        wire.readUnsigned(wire_u64) &&
        wire.readUnsigned(wire_u32) &&
        wire.readString(wire_u32, wire_name);
    const auto schema_offset = wire.offset();
    schema_located = schema_located && schema_offset + 3u <= blob1.size();
    check(
        schema_located &&
            blob1[schema_offset] == std::byte{
                static_cast<std::uint8_t>(lux::cxx::EScalarKind::I32)} &&
            blob1[schema_offset + 1u] == std::byte{1u} &&
            blob1[schema_offset + 2u] == std::byte{0u},
        "variable type uses stable ScalarSchema i32/v1.0");

    // ---- 4. defensive decode ---------------------------------------------------
    {
        FlowGraph junk;
        auto truncated = blob1;
        truncated.resize(truncated.size() / 2);
        check(!lux::authoring::detail::decodeFlowGraph(
                  std::span<const std::byte>(truncated), junk,
                  NodeRegistry::global(), &err),
              "truncated blob rejected");
    }
    {
        FlowGraph junk;
        auto bad = blob1;
        bad[0] = std::byte{0x00};
        check(!lux::authoring::detail::decodeFlowGraph(
                  std::span<const std::byte>(bad), junk,
                  NodeRegistry::global(), &err),
              "bad magic rejected");
    }
    {
        FlowGraph junk;
        auto old_version = blob1;
        old_version[8] = std::byte{2u};
        check(!lux::authoring::detail::decodeFlowGraph(
                  std::span<const std::byte>(old_version), junk,
                  NodeRegistry::global(), &err),
              "compiler-hash FlowGraph v2 is rejected by the v3 hard cut");
    }
    if (schema_located)
    {
        FlowGraph junk;
        auto unknown_schema = blob1;
        unknown_schema[schema_offset] = std::byte{0u};
        check(!lux::authoring::detail::decodeFlowGraph(
                  std::span<const std::byte>(unknown_schema), junk,
                  NodeRegistry::global(), &err),
              "unknown scalar schema is rejected");
    }

    if (g_failed != 0)
    {
        std::printf("flow_graph_codec_test: %d check(s) FAILED\n", g_failed);
        return 1;
    }
    std::printf("flow_graph_codec_test: all checks passed\n");
    return 0;
}
