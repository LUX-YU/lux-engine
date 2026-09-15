#include <algorithm>
#include <charconv>
#include <cmath>
#include <locale>
#include <lux/engine/flowforge/graph/FlowSource.hpp>
#include <sstream>
#include <toml++/toml.hpp>
#include <unordered_map>
#include <unordered_set>

namespace lux::flowforge
{
namespace
{
auto fail(EFlowSourceError code, std::string field = {}, NodeId node = {}, PinId pin = {}) noexcept
{
    return lux::cxx::unexpected(FlowSourceFailure{code, std::move(field), node, pin});
}
template <class T> bool number(std::string_view text, T &value) noexcept
{
    if (text.empty())
    {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
bool textValid(std::string_view text, FlowSourceLimits limits) noexcept
{
    if (text.size() > limits.max_string_bytes)
    {
        return false;
    }
    for (std::size_t i{}; i < text.size();)
    {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (!first)
        {
            return false;
        }
        if (first < 0x80)
        {
            continue;
        }
        const unsigned tails = first >= 0xc2 && first <= 0xdf   ? 1
                               : first >= 0xe0 && first <= 0xef ? 2
                               : first >= 0xf0 && first <= 0xf4 ? 3
                                                                : 0;
        if (!tails || tails > text.size() - i)
        {
            return false;
        }
        std::uint32_t scalar = first & ((1U << (6 - tails)) - 1);
        for (unsigned tail{}; tail < tails; ++tail)
        {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0U) != 0x80U)
            {
                return false;
            }
            scalar = (scalar << 6U) | (next & 0x3fU);
        }
        const bool overlong = scalar < (tails == 1 ? 0x80U : tails == 2 ? 0x800U : 0x10000U);
        if (overlong || scalar > 0x10ffffU || (scalar >= 0xd800U && scalar <= 0xdfffU))
        {
            return false;
        }
    }
    return true;
}
bool literalValid(const FlowSourceLiteral &literal) noexcept
{
    std::int64_t signed_value{};
    std::uint64_t unsigned_value{};
    double real{};
    switch (literal.kind)
    {
    case EFlowLiteralKind::NONE:
    case EFlowLiteralKind::ZERO:
        return literal.value.empty();
    case EFlowLiteralKind::BOOLEAN:
        return literal.value == "true" || literal.value == "false";
    case EFlowLiteralKind::SIGNED:
        return number(literal.value, signed_value);
    case EFlowLiteralKind::UNSIGNED:
        return number(literal.value, unsigned_value);
    case EFlowLiteralKind::REAL:
        return number(literal.value, real) && std::isfinite(real);
    default:
        return false;
    }
}
bool fields(const toml::table &table, std::initializer_list<std::string_view> names) noexcept
{
    return std::ranges::all_of(table, [&](const auto &item) {
        return std::find(names.begin(), names.end(), item.first.str()) != names.end();
    });
}
using View = toml::node_view<const toml::node>;
bool readText(View value, std::string &out)
{
    auto text = value.value<std::string>();
    if (!text)
    {
        return false;
    }
    out = std::move(*text);
    return true;
}
bool readId(View value, std::uint64_t &out) noexcept
{
    const auto text = value.value<std::string_view>();
    return text && number(*text, out);
}
template <class T> bool readEnum(View value, T &out, T maximum) noexcept
{
    const auto integer = value.value<std::int64_t>();
    if (!integer || *integer < 0 || *integer > static_cast<std::int64_t>(maximum))
    {
        return false;
    }
    out = static_cast<T>(*integer);
    return true;
}
toml::table literalTable(const FlowSourceLiteral &value)
{
    return toml::table{{"kind", static_cast<std::int64_t>(value.kind)}, {"value", value.value}};
}
bool readLiteral(View view, FlowSourceLiteral &out)
{
    const auto *table = view.as_table();
    return table && fields(*table, {"kind", "value"}) && readEnum(view["kind"], out.kind, EFlowLiteralKind::REAL) &&
           readText(view["value"], out.value);
}
toml::array arguments(const std::vector<FlowSourceArgument> &values)
{
    toml::array result;
    for (const auto &value : values)
    {
        result.push_back(toml::table{{"name", value.name}, {"type", value.type}});
    }
    return result;
}
bool readArguments(View view, std::vector<FlowSourceArgument> &out, FlowSourceLimits limits)
{
    const auto *array = view.as_array();
    if (!array || array->size() > limits.max_pins)
    {
        return false;
    }
    for (const auto &item : *array)
    {
        const auto *table = item.as_table();
        FlowSourceArgument argument;
        if (!table || !fields(*table, {"name", "type"}) || !readText((*table)["name"], argument.name) ||
            !readText((*table)["type"], argument.type))
        {
            return false;
        }
        out.push_back(std::move(argument));
    }
    return true;
}
toml::array pins(const std::vector<FlowSourcePin> &values)
{
    toml::array result;
    for (const auto &value : values)
    {
        result.push_back(toml::table{{"id", std::to_string(value.id.value)},
                                     {"kind", static_cast<std::int64_t>(value.kind)},
                                     {"name", value.name},
                                     {"type", value.type},
                                     {"literal", literalTable(value.literal)}});
    }
    return result;
}
bool readPins(View view, std::vector<FlowSourcePin> &out, FlowSourceLimits limits)
{
    const auto *array = view.as_array();
    if (!array || array->size() > limits.max_pins)
    {
        return false;
    }
    for (const auto &item : *array)
    {
        const auto *table = item.as_table();
        FlowSourcePin pin;
        if (!table || !fields(*table, {"id", "kind", "name", "type", "literal"}))
        {
            return false;
        }
        const bool valid = readId((*table)["id"], pin.id.value) &&
                           readEnum((*table)["kind"], pin.kind, EPinKind::DATA_OUT) &&
                           readText((*table)["name"], pin.name) && readText((*table)["type"], pin.type) &&
                           readLiteral((*table)["literal"], pin.literal);
        if (!valid)
        {
            return false;
        }
        out.push_back(std::move(pin));
    }
    return true;
}
toml::table eventTable(const lux::script::ScriptEventSourceDescription &value)
{
    return toml::table{{"system", value.system_name},
                       {"name", value.event_name},
                       {"system_id", std::to_string(value.system_id)},
                       {"event_id", std::to_string(value.event_id)},
                       {"route", static_cast<std::int64_t>(value.route)},
                       {"payload", value.payload.canonical_name},
                       {"payload_type", std::to_string(value.payload.type_id)},
                       {"payload_kind", value.payload.abi_kind},
                       {"payload_size", value.payload.size},
                       {"payload_alignment", value.payload.alignment},
                       {"payload_hash", std::to_string(value.payload_schema_hash)},
                       {"payload_version", value.payload_schema_version},
                       {"delivery_hook", std::to_string(value.delivery_hook_id)},
                       {"delivery_hash", std::to_string(value.delivery_schema_hash)},
                       {"delivery_version", value.delivery_schema_version}};
}
bool readEvent(View view, lux::script::ScriptEventSourceDescription &out)
{
    const auto *table = view.as_table();
    if (!table || !fields(*table, {"system", "name", "system_id", "event_id", "route", "payload", "payload_type",
                                   "payload_kind", "payload_size", "payload_alignment", "payload_hash",
                                   "payload_version", "delivery_hook", "delivery_hash", "delivery_version"}))
    {
        return false;
    }
    const auto integer = [&](std::string_view key, auto &destination) {
        const auto value = view[key].value<std::int64_t>();
        using T = std::remove_reference_t<decltype(destination)>;
        if (!value || *value < 0 || *value > (std::numeric_limits<T>::max)())
        {
            return false;
        }
        destination = static_cast<T>(*value);
        return true;
    };
    return readText(view["system"], out.system_name) && readText(view["name"], out.event_name) &&
           readId(view["system_id"], out.system_id) && readId(view["event_id"], out.event_id) &&
           readEnum(view["route"], out.route, lux::script::EScriptEventRoute::ENTITY_TARGETED) &&
           readText(view["payload"], out.payload.canonical_name) && readId(view["payload_type"], out.payload.type_id) &&
           integer("payload_kind", out.payload.abi_kind) && integer("payload_size", out.payload.size) &&
           integer("payload_alignment", out.payload.alignment) &&
           readId(view["payload_hash"], out.payload_schema_hash) &&
           integer("payload_version", out.payload_schema_version) &&
           readId(view["delivery_hook"], out.delivery_hook_id) &&
           readId(view["delivery_hash"], out.delivery_schema_hash) &&
           integer("delivery_version", out.delivery_schema_version);
}
} // namespace

namespace
{
std::size_t parameterIndex(ENodeOperation operation) noexcept
{
    using O = ENodeOperation;
    if ((operation >= O::ADD && operation <= O::CMP_GE) || operation == O::GET_OBJECT || operation == O::SET_OBJECT)
    {
        return 1;
    }
    switch (operation)
    {
    case O::FUNC_DEF_START:
    case O::ON_EVENT:
        return 2;
    case O::FUNC_RETURN:
    case O::GRAPH_FUNC_CALL:
    case O::GET_VARIABLE:
    case O::SET_VARIABLE:
        return 3;
    case O::GET_FIELD:
    case O::SET_FIELD:
        return 4;
    case O::NATIVE_FUNC_CALL:
        return 5;
    case O::SCRIPT_ABILITY_CALL:
        return 6;
    case O::SCRIPT_EVENT_WAIT:
        return 7;
    default:
        return 0;
    }
}

template <class Text> bool signatureValid(const FlowSourceSignature &value, Text &text, FlowSourceLimits limits)
{
    for (const auto &args : {std::cref(value.arguments), std::cref(value.results)})
    {
        if (args.get().size() > limits.max_pins)
        {
            return false;
        }
        for (const auto &argument : args.get())
        {
            if (argument.type.empty() || !text(argument.name) || !text(argument.type))
            {
                return false;
            }
        }
    }
    return true;
}

template <class Text> bool parametersValid(const FlowSourceNode &node, Text &text, FlowSourceLimits limits)
{
    if (node.parameters.index() != parameterIndex(node.operation))
    {
        return false;
    }
    return std::visit(
        [&](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::monostate>)
            {
                return true;
            }
            else if constexpr (std::is_same_v<T, FlowSourceType>)
            {
                return !value.name.empty() && text(value.name);
            }
            else if constexpr (std::is_same_v<T, FlowSourceSignature>)
            {
                return signatureValid(value, text, limits) &&
                       (node.operation != ENodeOperation::ON_EVENT || value.results.empty());
            }
            else if constexpr (std::is_same_v<T, FlowSourceReference>)
            {
                return value.id != 0 && value.id != UINT64_MAX;
            }
            else if constexpr (std::is_same_v<T, FlowSourceField>)
            {
                return !value.owner.empty() && !value.member.empty() && !value.type.empty() && text(value.owner) &&
                       text(value.member) && text(value.type);
            }
            else if constexpr (std::is_same_v<T, FlowSourceNativeCall>)
            {
                return !value.member.empty() && !value.signature.empty() && text(value.owner) && text(value.member) &&
                       text(value.signature) && signatureValid(value.parameters, text, limits);
            }
            else if constexpr (std::is_same_v<T, FlowSourceAbility>)
            {
                return !value.contract.empty() && !value.method.empty() && value.schema_version && value.schema_hash &&
                       text(value.contract) && text(value.method);
            }
            else
            {
                return value.valid() && text(value.system_name) && text(value.event_name) &&
                       text(value.payload.canonical_name);
            }
        },
        node.parameters);
}

toml::table parametersTable(const FlowSourceParameters &parameters)
{
    return std::visit(
        [](const auto &value) -> toml::table {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::monostate>)
            {
                return {};
            }
            else if constexpr (std::is_same_v<T, FlowSourceType>)
            {
                return toml::table{{"type", value.name}};
            }
            else if constexpr (std::is_same_v<T, FlowSourceSignature>)
            {
                return toml::table{{"arguments", arguments(value.arguments)}, {"results", arguments(value.results)}};
            }
            else if constexpr (std::is_same_v<T, FlowSourceReference>)
            {
                return toml::table{{"reference", std::to_string(value.id)}};
            }
            else if constexpr (std::is_same_v<T, FlowSourceField>)
            {
                return toml::table{{"owner", value.owner}, {"member", value.member}, {"type", value.type}};
            }
            else if constexpr (std::is_same_v<T, FlowSourceNativeCall>)
            {
                return toml::table{{"owner", value.owner},
                                   {"member", value.member},
                                   {"signature", value.signature},
                                   {"arguments", arguments(value.parameters.arguments)},
                                   {"results", arguments(value.parameters.results)}};
            }
            else if constexpr (std::is_same_v<T, FlowSourceAbility>)
            {
                return toml::table{{"contract", value.contract},
                                   {"method", value.method},
                                   {"schema_version", value.schema_version},
                                   {"schema_hash", std::to_string(value.schema_hash)}};
            }
            else
            {
                return eventTable(value);
            }
        },
        parameters);
}

bool readParameters(View view, FlowSourceNode &node, FlowSourceLimits limits)
{
    const auto *table = view.as_table();
    if (!table)
    {
        return false;
    }
    switch (parameterIndex(node.operation))
    {
    case 0:
        return table->empty();
    case 1: {
        auto &value = node.parameters.emplace<FlowSourceType>();
        return fields(*table, {"type"}) && readText(view["type"], value.name);
    }
    case 2: {
        auto &value = node.parameters.emplace<FlowSourceSignature>();
        return fields(*table, {"arguments", "results"}) && readArguments(view["arguments"], value.arguments, limits) &&
               readArguments(view["results"], value.results, limits);
    }
    case 3: {
        auto &value = node.parameters.emplace<FlowSourceReference>();
        return fields(*table, {"reference"}) && readId(view["reference"], value.id);
    }
    case 4: {
        auto &value = node.parameters.emplace<FlowSourceField>();
        return fields(*table, {"owner", "member", "type"}) && readText(view["owner"], value.owner) &&
               readText(view["member"], value.member) && readText(view["type"], value.type);
    }
    case 5: {
        auto &value = node.parameters.emplace<FlowSourceNativeCall>();
        return fields(*table, {"owner", "member", "signature", "arguments", "results"}) &&
               readText(view["owner"], value.owner) && readText(view["member"], value.member) &&
               readText(view["signature"], value.signature) &&
               readArguments(view["arguments"], value.parameters.arguments, limits) &&
               readArguments(view["results"], value.parameters.results, limits);
    }
    case 6: {
        auto &value = node.parameters.emplace<FlowSourceAbility>();
        const auto version = view["schema_version"].value<std::int64_t>();
        if (!version || *version < 0 || *version > UINT32_MAX)
        {
            return false;
        }
        value.schema_version = static_cast<std::uint32_t>(*version);
        return fields(*table, {"contract", "method", "schema_version", "schema_hash"}) &&
               readText(view["contract"], value.contract) && readText(view["method"], value.method) &&
               readId(view["schema_hash"], value.schema_hash);
    }
    case 7:
        return readEvent(view, node.parameters.emplace<lux::script::ScriptEventSourceDescription>());
    default:
        return false;
    }
}
} // namespace

FlowSourceResult<void> validateFlowVariable(const FlowSourceVariable &variable, FlowSourceLimits limits) noexcept
{
    if (!variable.id || variable.id == UINT64_MAX)
    {
        return fail(EFlowSourceError::INVALID_IDENTITY, "variable");
    }
    const bool valid = !variable.name.empty() && !variable.type.empty() && textValid(variable.name, limits) &&
                       textValid(variable.type, limits) && textValid(variable.value.value, limits) &&
                       literalValid(variable.value);
    if (!valid)
    {
        return fail(EFlowSourceError::INVALID_VALUE, "variable");
    }
    return {};
}

FlowSourceResult<void> validateFlowSource(const FlowSourceDocument &source, FlowSourceLimits limits) noexcept
{
    const bool invalid_limits = !limits.max_bytes || !limits.max_nodes || !limits.max_pins || !limits.max_variables ||
                                !limits.max_exports || !limits.max_string_bytes;
    if (invalid_limits)
    {
        return fail(EFlowSourceError::INVALID_ARGUMENT, "limits");
    }
    if (source.id.isNull())
    {
        return fail(EFlowSourceError::INVALID_IDENTITY, "id");
    }
    const bool too_many = source.nodes.size() > limits.max_nodes || source.variables.size() > limits.max_variables ||
                          source.exports.size() > limits.max_exports || source.links.size() > limits.max_pins;
    if (too_many)
    {
        return fail(EFlowSourceError::LIMIT_EXCEEDED, "counts");
    }
    std::size_t charged{};
    const auto text = [&](std::string_view value) {
        if (!textValid(value, limits) || value.size() > limits.max_bytes - charged)
        {
            return false;
        }
        charged += value.size();
        return true;
    };
    if (!text(source.name))
    {
        return fail(EFlowSourceError::INVALID_VALUE, "name");
    }
    std::unordered_set<std::uint64_t> node_ids, variable_ids, export_ids, symbols;
    struct PinOwner
    {
        const FlowSourcePin *pin;
        NodeId node;
        std::size_t links{};
    };
    std::unordered_map<std::uint64_t, PinOwner> pin_ids;
    for (const auto &node : source.nodes)
    {
        if (!node.id.valid() || node.id.value == UINT64_MAX || !node_ids.insert(node.id.value).second)
        {
            return fail(EFlowSourceError::INVALID_IDENTITY, "node", node.id);
        }
        const bool unknown = node.operation == ENodeOperation::INVALID || node.operation > ENodeOperation::SEND_EVENT ||
                             node.operation == ENodeOperation::CREATE_OBJECT ||
                             node.operation == ENodeOperation::SEND_EVENT;
        if (unknown)
        {
            return fail(EFlowSourceError::UNKNOWN_NODE_KIND, "operation", node.id);
        }
        if (!text(node.name) || !text(node.creator) || !parametersValid(node, text, limits))
        {
            return fail(EFlowSourceError::INVALID_VALUE, "node.parameters", node.id);
        }
        if (!std::isfinite(node.layout.x) || !std::isfinite(node.layout.y))
        {
            return fail(EFlowSourceError::INVALID_VALUE, "layout", node.id);
        }
        for (const bool input : {true, false})
        {
            for (const auto &pin : input ? node.inputs : node.outputs)
            {
                if (pin_ids.size() >= limits.max_pins)
                {
                    return fail(EFlowSourceError::LIMIT_EXCEEDED, "pins", node.id);
                }
                if (!pin.id.valid() || pin.id.value == UINT64_MAX ||
                    !pin_ids.emplace(pin.id.value, PinOwner{&pin, node.id}).second)
                {
                    return fail(EFlowSourceError::INVALID_IDENTITY, "pin", node.id, pin.id);
                }
                const bool data = pin.kind == EPinKind::DATA_IN || pin.kind == EPinKind::DATA_OUT;
                const bool direction = input ? pin.kind == EPinKind::EXEC_IN || pin.kind == EPinKind::DATA_IN
                                             : pin.kind == EPinKind::EXEC_OUT || pin.kind == EPinKind::DATA_OUT;
                const bool valid = direction && data == !pin.type.empty() && text(pin.name) && text(pin.type) &&
                                   text(pin.literal.value) && literalValid(pin.literal) &&
                                   (pin.kind == EPinKind::DATA_IN || pin.literal.kind == EFlowLiteralKind::NONE);
                if (!valid)
                {
                    return fail(EFlowSourceError::INVALID_VALUE, "pin", node.id, pin.id);
                }
            }
        }
    }
    for (const auto &variable : source.variables)
    {
        if (!variable.id || !variable_ids.insert(variable.id).second)
        {
            return fail(EFlowSourceError::INVALID_IDENTITY, "variable");
        }
        if (auto valid = validateFlowVariable(variable, limits); !valid)
        {
            return valid;
        }
    }
    for (const auto &link : source.links)
    {
        auto from = pin_ids.find(link.from.value), to = pin_ids.find(link.to.value);
        if (from == pin_ids.end() || to == pin_ids.end())
        {
            return fail(EFlowSourceError::INVALID_TOPOLOGY, "link", {}, link.to);
        }
        auto &first = from->second;
        auto &second = to->second;
        const bool exec = first.pin->kind == EPinKind::EXEC_OUT && second.pin->kind == EPinKind::EXEC_IN;
        const bool data = first.pin->kind == EPinKind::DATA_OUT && second.pin->kind == EPinKind::DATA_IN;
        const bool invalid =
            first.node == second.node || (!exec && !data) || (exec && first.links) || (data && second.links);
        if (invalid)
        {
            return fail(EFlowSourceError::INVALID_TOPOLOGY, "link", second.node, link.to);
        }
        ++first.links;
        ++second.links;
    }
    for (const auto &item : source.exports)
    {
        const bool valid = item.id && item.symbol && export_ids.insert(item.id).second &&
                           symbols.insert(item.symbol).second && node_ids.contains(item.entry.value);
        if (!valid)
        {
            return fail(EFlowSourceError::INVALID_IDENTITY, "export", item.entry);
        }
        if (item.hints.size() > limits.max_exports)
        {
            return fail(EFlowSourceError::LIMIT_EXCEEDED, "hints", item.entry);
        }
        for (const auto &hint : item.hints)
        {
            if (hint.kind > lux::script::EScriptBindingHintKind::EVENT || !text(hint.qualified_name))
            {
                return fail(EFlowSourceError::INVALID_VALUE, "hint", item.entry);
            }
        }
    }
    return {};
}

FlowSourceResult<std::string> encodeFlowSource(const FlowSourceDocument &source, FlowSourceLimits limits) noexcept
{
    if (auto valid = validateFlowSource(source, limits); !valid)
    {
        return lux::cxx::unexpected(valid.error());
    }
    toml::table document{{"format", "lux.flowforge.source"},
                         {"version", 1},
                         {"id", uuids::to_string(source.id.uuid())},
                         {"name", source.name}};
    toml::array nodes, variables, links, exports;
    for (const auto &node : source.nodes)
    {
        toml::table table{{"id", std::to_string(node.id.value)},
                          {"operation", static_cast<std::int64_t>(node.operation)},
                          {"name", node.name},
                          {"creator", node.creator},
                          {"inputs", pins(node.inputs)},
                          {"outputs", pins(node.outputs)},
                          {"parameters", parametersTable(node.parameters)},
                          {"layout", toml::table{{"x", static_cast<double>(node.layout.x)},
                                                 {"y", static_cast<double>(node.layout.y)},
                                                 {"placed", node.layout.placed}}}};
        nodes.push_back(std::move(table));
    }
    for (const auto &value : source.variables)
    {
        variables.push_back(toml::table{{"id", std::to_string(value.id)},
                                        {"name", value.name},
                                        {"type", value.type},
                                        {"value", literalTable(value.value)}});
    }
    for (const auto &link : source.links)
    {
        links.push_back(toml::table{{"from", std::to_string(link.from.value)}, {"to", std::to_string(link.to.value)}});
    }
    for (const auto &value : source.exports)
    {
        toml::array hints;
        for (const auto &hint : value.hints)
        {
            hints.push_back(toml::table{{"kind", static_cast<std::int64_t>(hint.kind)}, {"name", hint.qualified_name}});
        }
        exports.push_back(toml::table{{"id", std::to_string(value.id)},
                                      {"symbol", std::to_string(value.symbol)},
                                      {"entry", std::to_string(value.entry.value)},
                                      {"hints", std::move(hints)}});
    }
    document.insert("nodes", std::move(nodes));
    document.insert("variables", std::move(variables));
    document.insert("links", std::move(links));
    document.insert("exports", std::move(exports));
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << toml::toml_formatter{document};
    auto result = std::move(stream).str();
    if (result.size() > limits.max_bytes)
    {
        return fail(EFlowSourceError::LIMIT_EXCEEDED, "bytes");
    }
    return result;
}

FlowSourceResult<FlowSourceDocument> decodeFlowSource(std::string_view bytes, FlowSourceLimits limits) noexcept
{
    if (!limits.max_bytes || bytes.size() > limits.max_bytes)
    {
        return fail(EFlowSourceError::LIMIT_EXCEEDED, "bytes");
    }
    auto parsed = toml::parse(bytes);
    if (!parsed)
    {
        FlowSourceFailure error{EFlowSourceError::PARSE_FAILURE, std::string(parsed.error().description())};
        error.line = parsed.error().source().begin.line;
        error.column = parsed.error().source().begin.column;
        return lux::cxx::unexpected(std::move(error));
    }
    const auto &document = parsed.table();
    if (!fields(document, {"format", "version", "id", "name", "nodes", "variables", "links", "exports"}))
    {
        return fail(EFlowSourceError::UNKNOWN_FIELD, "document");
    }
    if (document["format"].value<std::string_view>() != "lux.flowforge.source" || document["version"].value<int>() != 1)
    {
        return fail(EFlowSourceError::UNSUPPORTED_FORMAT, "format/version");
    }
    FlowSourceDocument source;
    const auto uuid_text = document["id"].value<std::string_view>();
    if (!uuid_text)
    {
        return fail(EFlowSourceError::INVALID_IDENTITY, "id");
    }
    const auto uuid = uuids::uuid::from_string(*uuid_text);
    if (!uuid)
    {
        return fail(EFlowSourceError::INVALID_IDENTITY, "id");
    }
    source.id = lux::asset::AssetId(*uuid);
    if (!readText(document["name"], source.name))
    {
        return fail(EFlowSourceError::INVALID_VALUE, "name");
    }
    const auto *nodes = document["nodes"].as_array();
    const auto *variables = document["variables"].as_array();
    const auto *links = document["links"].as_array();
    const auto *exports = document["exports"].as_array();
    if (!nodes || !variables || !links || !exports)
    {
        return fail(EFlowSourceError::INVALID_VALUE, "arrays");
    }
    const bool too_many = nodes->size() > limits.max_nodes || variables->size() > limits.max_variables ||
                          links->size() > limits.max_pins || exports->size() > limits.max_exports;
    if (too_many)
    {
        return fail(EFlowSourceError::LIMIT_EXCEEDED, "counts");
    }
    for (const auto &item : *nodes)
    {
        const auto *table = item.as_table();
        if (!table)
        {
            return fail(EFlowSourceError::INVALID_VALUE, "node");
        }
        if (!fields(*table, {"id", "operation", "name", "creator", "inputs", "outputs", "parameters", "layout"}))
        {
            return fail(EFlowSourceError::UNKNOWN_FIELD, "node");
        }
        FlowSourceNode node;
        const View view{table};
        const bool valid = readId(view["id"], node.id.value) &&
                           readEnum(view["operation"], node.operation, ENodeOperation::SEND_EVENT) &&
                           readText(view["name"], node.name) && readText(view["creator"], node.creator) &&
                           readPins(view["inputs"], node.inputs, limits) &&
                           readPins(view["outputs"], node.outputs, limits) &&
                           readParameters(view["parameters"], node, limits);
        if (!valid)
        {
            return fail(EFlowSourceError::INVALID_VALUE, "node", node.id);
        }
        const auto *layout = view["layout"].as_table();
        if (!layout || !fields(*layout, {"x", "y", "placed"}))
        {
            return fail(EFlowSourceError::INVALID_VALUE, "layout", node.id);
        }
        const auto x = (*layout)["x"].value<double>(), y = (*layout)["y"].value<double>();
        const auto placed = (*layout)["placed"].value<bool>();
        if (!x || !y || !placed)
        {
            return fail(EFlowSourceError::INVALID_VALUE, "layout", node.id);
        }
        node.layout = lux::graph::GraphNodeLayout{static_cast<float>(*x), static_cast<float>(*y), *placed};
        source.nodes.push_back(std::move(node));
    }
    for (const auto &item : *variables)
    {
        const auto *table = item.as_table();
        if (!table || !fields(*table, {"id", "name", "type", "value"}))
        {
            return fail(EFlowSourceError::INVALID_VALUE, "variable");
        }
        FlowSourceVariable variable;
        const bool valid = readId((*table)["id"], variable.id) && readText((*table)["name"], variable.name) &&
                           readText((*table)["type"], variable.type) && readLiteral((*table)["value"], variable.value);
        if (!valid)
        {
            return fail(EFlowSourceError::INVALID_VALUE, "variable");
        }
        source.variables.push_back(std::move(variable));
    }
    for (const auto &item : *links)
    {
        const auto *table = item.as_table();
        FlowSourceLink link;
        if (!table || !fields(*table, {"from", "to"}) || !readId((*table)["from"], link.from.value) ||
            !readId((*table)["to"], link.to.value))
        {
            return fail(EFlowSourceError::INVALID_VALUE, "link");
        }
        source.links.push_back(link);
    }
    for (const auto &item : *exports)
    {
        const auto *table = item.as_table();
        if (!table || !fields(*table, {"id", "symbol", "entry", "hints"}))
        {
            return fail(EFlowSourceError::INVALID_VALUE, "export");
        }
        FlowSourceExport value;
        const bool valid = readId((*table)["id"], value.id) && readId((*table)["symbol"], value.symbol) &&
                           readId((*table)["entry"], value.entry.value);
        const auto *hints = (*table)["hints"].as_array();
        if (!valid || !hints || hints->size() > limits.max_exports)
        {
            return fail(EFlowSourceError::INVALID_VALUE, "export");
        }
        for (const auto &hint : *hints)
        {
            const auto *hint_table = hint.as_table();
            lux::script::ScriptBindingHintTarget target;
            if (!hint_table || !fields(*hint_table, {"kind", "name"}) ||
                !readEnum((*hint_table)["kind"], target.kind, lux::script::EScriptBindingHintKind::EVENT) ||
                !readText((*hint_table)["name"], target.qualified_name))
            {
                return fail(EFlowSourceError::INVALID_VALUE, "hint", value.entry);
            }
            value.hints.push_back(std::move(target));
        }
        source.exports.push_back(std::move(value));
    }
    if (auto valid = validateFlowSource(source, limits); !valid)
    {
        return lux::cxx::unexpected(valid.error());
    }
    return source;
}
} // namespace lux::flowforge
