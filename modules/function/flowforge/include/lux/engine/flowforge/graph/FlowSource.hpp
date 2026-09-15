#pragma once

#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityCatalog.hpp>
#include <lux/engine/function/script/ScriptEvent.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <variant>

namespace lux::flowforge
{
// Owned source values only. A const source never exposes Node::graph(), Pin::node(), or metadata pointers.
// Types and native call signatures use qualified names, not process addresses or registry indices.
struct FlowSourceArgument final
{
    std::string name, type;
    bool operator==(const FlowSourceArgument &) const = default;
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
    bool operator==(const FlowSourceLiteral &) const = default;
};
struct FlowSourcePin final
{
    PinId id;
    EPinKind kind{};
    std::string name, type;
    FlowSourceLiteral literal;
    bool operator==(const FlowSourcePin &) const = default;
};
struct FlowSourceType final
{
    std::string name;
    bool operator==(const FlowSourceType &) const = default;
};
struct FlowSourceSignature final
{
    std::vector<FlowSourceArgument> arguments, results;
    bool operator==(const FlowSourceSignature &) const = default;
};
struct FlowSourceReference final
{
    std::uint64_t id{};
    bool operator==(const FlowSourceReference &) const = default;
};
struct FlowSourceField final
{
    std::string owner, member, type;
    bool operator==(const FlowSourceField &) const = default;
};
struct FlowSourceNativeCall final
{
    std::string owner, member, signature;
    FlowSourceSignature parameters;
    bool operator==(const FlowSourceNativeCall &) const = default;
};
struct FlowSourceAbility final
{
    std::string contract, method;
    std::uint32_t schema_version{};
    std::uint64_t schema_hash{};
    bool operator==(const FlowSourceAbility &) const = default;
};
// Each operation has exactly one parameter schema. Control nodes have no parameters.
// These values are immutable captures/codec input, never a second writable FlowGraph.
using FlowSourceParameters =
    std::variant<std::monostate, FlowSourceType, FlowSourceSignature, FlowSourceReference, FlowSourceField,
                 FlowSourceNativeCall, FlowSourceAbility, lux::script::ScriptEventSourceDescription>;

struct FlowSourceNode final
{
    NodeId id;
    ENodeOperation operation{};
    std::string name, creator;
    std::vector<FlowSourcePin> inputs, outputs;
    lux::graph::GraphNodeLayout layout;
    FlowSourceParameters parameters;
    bool operator==(const FlowSourceNode &) const = default;
};
struct FlowSourceVariable final
{
    std::uint64_t id{};
    std::string name, type;
    FlowSourceLiteral value;
    bool operator==(const FlowSourceVariable &) const = default;
};
struct FlowSourceLink final
{
    PinId from, to;
    bool operator==(const FlowSourceLink &) const = default;
};
struct FlowSourceExport final
{
    std::uint64_t id{}, symbol{};
    NodeId entry;
    std::vector<lux::script::ScriptBindingHintTarget> hints;
    bool operator==(const FlowSourceExport &) const = default;
};
struct FlowSourceDocument final
{
    lux::asset::AssetId id;
    std::string name;
    std::vector<FlowSourceNode> nodes;
    std::vector<FlowSourceVariable> variables;
    std::vector<FlowSourceLink> links;
    std::vector<FlowSourceExport> exports;
    bool operator==(const FlowSourceDocument &) const = default;
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
    UNSUPPORTED_LITERAL
};
struct FlowSourceFailure final
{
    EFlowSourceError code{};
    std::string field;
    NodeId node;
    PinId pin;
    std::uint32_t line{}, column{};
};
template <class T> using FlowSourceResult = lux::cxx::expected<T, FlowSourceFailure>;

// Cold metadata remains immutable. A dynamic contribution supplies an owner for every referenced
// span, descriptor and module; static program-lifetime metadata needs no lease. A materialized graph
// still borrows this environment, so its owner must retain the environment until graph destruction.
struct FlowSourceEnvironment final
{
    std::span<const lux::meta::RefType *const> types;
    std::span<const lux::meta::RefClass *const> classes;
    std::span<const lux::meta::RefFunction *const> functions;
    ScriptAbilityNodeCatalogView abilities;
    std::span<const lux::script::ScriptEventSourceDescription> events;
    std::shared_ptr<const void> code_lifetime;
};
[[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<void> validateFlowSourceEnvironment(
    const FlowSourceEnvironment &, FlowSourceLimits = {}) noexcept;
[[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<void> validateFlowSource(const FlowSourceDocument &,
                                                                                    FlowSourceLimits = {}) noexcept;
[[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<std::string> encodeFlowSource(
    const FlowSourceDocument &, FlowSourceLimits = {}) noexcept;
[[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<FlowSourceDocument> decodeFlowSource(
    std::string_view, FlowSourceLimits = {}) noexcept;
[[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<FlowSourceDocument> captureFlowSource(
    lux::asset::AssetId, std::string name, const FlowGraph &, FlowSourceLimits = {}) noexcept;
[[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<FlowSourceNode> captureFlowNode(const Node &) noexcept;
[[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<std::vector<FuncArgInfo>> materializeFlowArguments(
    std::span<const FlowSourceArgument>, const FlowSourceEnvironment & = {}, FlowSourceLimits = {}) noexcept;
[[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<FlowSourceLiteral> captureFlowLiteral(
    const lux::meta::RuntimeObject &) noexcept;
[[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<lux::meta::RuntimeObject> materializeFlowLiteral(
    const FlowSourceLiteral &, const lux::meta::RefType &) noexcept;
[[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<void> validateFlowVariable(const FlowSourceVariable &,
                                                                                      FlowSourceLimits = {}) noexcept;
[[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<FlowSourceVariable> captureFlowVariable(
    const FlowGraph::GraphVariable &) noexcept;
[[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<FlowGraph::GraphVariable> materializeFlowVariable(
    const FlowSourceVariable &, const FlowSourceEnvironment & = {}) noexcept;
[[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<FlowGraph> materializeFlowSource(
    const FlowSourceDocument &, const FlowSourceEnvironment & = {}, FlowSourceLimits = {}) noexcept;
[[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<void> validateFlowSourceConnection(
    const FlowSourcePin &from, const FlowSourcePin &to, const FlowSourceEnvironment & = {}) noexcept;
[[nodiscard]] LUX_ENGINE_FLOWFORGE_PUBLIC FlowSourceResult<void> validateFlowSourceLiteral(
    const FlowSourcePin &, const FlowSourceEnvironment & = {}) noexcept;
} // namespace lux::flowforge
