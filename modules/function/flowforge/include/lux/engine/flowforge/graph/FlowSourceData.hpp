#pragma once

#include <lux/engine/flowforge/FlowForgeFailure.hpp>
#include <lux/engine/function/graph/GraphTypes.hpp>
#include <lux/engine/function/script/ScriptBindingHint.hpp>
#include <lux/engine/function/script/ScriptEvent.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace lux::flowforge
{
    using graph::NodeId;
    using graph::PinId;

    // Owned source values only. A const source never exposes mutable graph storage or metadata pointers.
    // Types and native call signatures use qualified names, not process addresses or registry indices.
    struct FlowSourceArgument final
    {
        std::string name, type;
        bool operator==(const FlowSourceArgument&) const = default;
    };
    enum class EFlowLiteralKind : std::uint8_t
    {
        NONE,
        ZERO,
        BOOLEAN,
        SIGNED,
        UNSIGNED,
        REAL
    };

    struct FlowSourceLiteral final
    {
        EFlowLiteralKind kind{};
        std::string value;
        bool operator==(const FlowSourceLiteral&) const = default;
    };

    // Frozen file values, not a runtime Pin class or its membership state.
    enum class EFlowSourcePinKind : std::uint8_t
    {
        UNKNOWN = 0,
        EXEC_IN = 1,
        EXEC_OUT = 2,
        DATA_IN = 3,
        DATA_OUT = 4
    };

    struct FlowSourcePin final
    {
        PinId id;
        EFlowSourcePinKind kind{};
        std::string name, type;
        FlowSourceLiteral literal;
        graph::PinSemanticId semantic;
        bool operator==(const FlowSourcePin&) const = default;
    };

    struct FlowSourceType final
    {
        std::string name;
        bool operator==(const FlowSourceType&) const = default;
    };

    struct FlowSourceSignature final
    {
        std::vector<FlowSourceArgument> arguments, results;
        bool operator==(const FlowSourceSignature&) const = default;
    };

    struct FlowSourceReference final
    {
        std::uint64_t id{};
        bool operator==(const FlowSourceReference&) const = default;
    };

    struct FlowSourceField final
    {
        std::string owner, member, type;
        bool operator==(const FlowSourceField&) const = default;
    };

    struct FlowSourceNativeCall final
    {
        std::string owner, member, signature;
        FlowSourceSignature parameters;
        bool operator==(const FlowSourceNativeCall&) const = default;
    };

    struct FlowSourceAbility final
    {
        std::string contract, method;
        std::uint32_t schema_version{};
        std::uint64_t schema_hash{};
        bool operator==(const FlowSourceAbility&) const = default;
    };

    struct FlowSourcePayload final
    {
        std::string bytes;
        bool operator==(const FlowSourcePayload&) const = default;
    };

    // Builtin adapters have fixed parameter schemas; registered nodes own their codec bytes. Control nodes have no
    // parameters. These values are immutable captures/codec input, never a second writable FlowGraph.
    using VFlowSourceParameters = std::variant<
        std::monostate,
        FlowSourceType,
        FlowSourceSignature,
        FlowSourceReference,
        FlowSourceField,
        FlowSourceNativeCall,
        FlowSourceAbility,
        lux::script::ScriptEventSourceDescription,
        FlowSourcePayload>;

    struct FlowSourceNode final
    {
        NodeId id;
        std::string type;
        std::uint32_t version{1};
        std::string name, creator;
        std::vector<FlowSourcePin> inputs, outputs;
        lux::graph::GraphNodeLayout layout;
        VFlowSourceParameters parameters;
        bool operator==(const FlowSourceNode&) const = default;
    };

    struct FlowSourceVariable final
    {
        std::uint64_t id{};
        std::string name, type;
        FlowSourceLiteral value;
        bool operator==(const FlowSourceVariable&) const = default;
    };

    struct FlowSourceLink final
    {
        PinId from, to;
        bool operator==(const FlowSourceLink&) const = default;
    };

    struct FlowSourceExport final
    {
        std::uint64_t id{}, symbol{};
        NodeId entry;
        std::vector<lux::script::ScriptBindingHintTarget> hints;
        bool operator==(const FlowSourceExport&) const = default;
    };

    struct FlowSource final
    {
        lux::asset::AssetId id;
        std::string name;
        std::vector<FlowSourceNode> nodes;
        std::vector<FlowSourceVariable> variables;
        std::vector<FlowSourceLink> links;
        std::vector<FlowSourceExport> exports;
        bool operator==(const FlowSource&) const = default;
    };

    struct FlowSourceLimits final
    {
        std::size_t max_bytes{16U * 1024U * 1024U}, max_nodes{16384}, max_pins{131072};
        std::size_t max_variables{4096}, max_exports{4096}, max_string_bytes{4096};
    };
    enum class EFlowSourceError : std::uint8_t
    {
        INVALID_ARGUMENT,
        LIMIT_EXCEEDED,
        PARSE_FAILURE,
        UNSUPPORTED_FORMAT,
        UNKNOWN_FIELD,
        INVALID_IDENTITY,
        INVALID_VALUE,
        UNKNOWN_NODE_KIND,
        INVALID_TOPOLOGY,
        UNKNOWN_TYPE,
        UNKNOWN_REFLECTION_MEMBER,
        UNKNOWN_ABILITY,
        SCHEMA_MISMATCH,
        UNSUPPORTED_LITERAL,
        NODE_CODEC_FAILURE
    };

    struct FlowSourceFailure final
    {
        EFlowSourceError code{};
        std::string field;
        NodeId node;
        PinId pin;
        std::uint32_t line{}, column{};
        std::optional<FlowForgeFailure> cause;
    };
    template <class T> using FlowSourceResult = lux::cxx::expected<T, FlowSourceFailure>;

} // namespace lux::flowforge
