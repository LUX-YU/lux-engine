#include <lux/engine/editor/workspace/LayoutPlan.hpp>
#include <lux/engine/editor/workspace/RecoveryManifest.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <limits>
#include <sstream>

namespace lux::editor::workspace
{
    namespace
    {
        auto malformed(std::string detail)
        {
            return lux::cxx::unexpected(WorkspaceFailure{EWorkspaceError::INVALID_DATA, std::move(detail)});
        }
        auto capacity()
        {
            return lux::cxx::unexpected(WorkspaceFailure{EWorkspaceError::CAPACITY, "workspace codec budget"});
        }
        std::uint32_t number(const toml::table& table, std::string_view key)
        {
            const auto value = table[key].value<std::int64_t>();
            return value && *value >= 0 && *value <= UINT32_MAX ? static_cast<std::uint32_t>(*value) : 0;
        }
        std::string hex(std::span<const std::byte> bytes)
        {
            constexpr char digits[] = "0123456789abcdef";
            std::string result;
            result.reserve(bytes.size() * 2);
            for (auto byte : bytes)
            {
                const auto value = std::to_integer<unsigned>(byte);
                result += digits[value >> 4];
                result += digits[value & 15];
            }
            return result;
        }
        WorkspaceResult<std::vector<std::byte>> unhex(const toml::table& table, WorkspaceLimits limits)
        {
            const auto encoded = table["bytes"].value<std::string>();
            const auto size = table["size"].value<std::int64_t>();
            if (!encoded || !size || *size < 0 || encoded->size() % 2 ||
                static_cast<std::uint64_t>(*size) != encoded->size() / 2)
                return malformed("payload length");
            if (encoded->size() / 2 > limits.opaque_bytes)
                return capacity();
            auto digit = [](char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                return -1;
            };
            std::vector<std::byte> result;
            result.reserve(encoded->size() / 2);
            for (std::size_t i = 0; i < encoded->size(); i += 2)
            {
                const int high = digit((*encoded)[i]), low = digit((*encoded)[i + 1]);
                if (high < 0 || low < 0)
                    return malformed("payload hex");
                result.push_back(std::byte((high << 4) | low));
            }
            return result;
        }
        toml::table envelope(std::uint32_t schema, std::span<const std::byte> bytes)
        {
            return toml::table{
                {"schema", std::int64_t(schema)},
                {"size", std::int64_t(bytes.size())},
                {"bytes", hex(bytes)}
            };
        }
        bool unknown(const toml::table& table, std::initializer_list<std::string_view> known)
        {
            return std::ranges::any_of(table, [&](const auto& item) {
                return std::ranges::find(known, item.first.str()) == known.end();
            });
        }
        void encodeCommon(toml::table& table, const auto& value)
        {
            table.insert("schema", std::int64_t(value.schema));
            toml::array opaque;
            for (const auto& item : value.opaque)
            {
                auto row = envelope(item.schema, item.bytes);
                row.insert("type", item.type);
                opaque.push_back(std::move(row));
            }
            table.insert("opaque", std::move(opaque));
            if (value.legacy_origin)
                table.insert(
                    "origin",
                    toml::table{{"key", value.legacy_origin->key}, {"digest", value.legacy_origin->digest}}
                );
        }
        WorkspaceResult<void> decodeCommon(const toml::table& table, auto& value, WorkspaceLimits limits, bool& extra, std::uint32_t maximum_schema = 1)
        {
            value.schema = number(table, "schema");
            if (value.schema == 0 || value.schema > maximum_schema)
                return lux::cxx::unexpected(WorkspaceFailure{EWorkspaceError::UNSUPPORTED_VERSION, "file schema"});
            const auto* opaque = table["opaque"].as_array();
            if (!opaque)
                return malformed("opaque array");
            if (opaque->size() > limits.entries)
                return capacity();
            std::size_t total{};
            for (const auto& item : *opaque)
            {
                const auto* row = item.as_table();
                if (!row)
                    return malformed("opaque row");
                auto bytes = unhex(*row, limits);
                if (!bytes)
                    return lux::cxx::unexpected(bytes.error());
                if (bytes->size() > limits.opaque_bytes - total)
                    return capacity();
                total += bytes->size();
                value.opaque.push_back(
                    {(*row)["type"].value_or(std::string{}), number(*row, "schema"), std::move(*bytes)}
                );
                extra |= unknown(*row, {"type", "schema", "size", "bytes"});
            }
            if (table.contains("origin"))
            {
                const auto* origin = table["origin"].as_table();
                if (!origin)
                    return malformed("origin");
                value.legacy_origin =
                    LegacyOrigin{(*origin)["key"].value_or(std::string{}), (*origin)["digest"].value_or(std::string{})};
                extra |= unknown(*origin, {"key", "digest"});
            }
            return {};
        }
        WorkspaceResult<toml::table> parse(std::span<const std::byte> bytes, WorkspaceLimits limits)
        {
            if (bytes.size() > limits.file_bytes)
                return capacity();
            auto parsed = toml::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            if (!parsed)
                return malformed("TOML syntax");
            std::vector<std::pair<const toml::node*, std::size_t>> pending{{&parsed.table(), 1}};
            while (!pending.empty())
            {
                const auto [node, depth] = pending.back();
                pending.pop_back();
                if (depth > limits.depth)
                    return capacity();
                if (const auto* object = node->as_table())
                    for (const auto& [key, child] : *object)
                        pending.emplace_back(&child, depth + 1);
                if (const auto* array = node->as_array())
                    for (const auto& child : *array)
                        pending.emplace_back(&child, depth + 1);
            }
            return std::move(parsed).table();
        }
        WorkspaceResult<std::vector<std::byte>> encode(const toml::table& table, WorkspaceLimits limits)
        {
            std::ostringstream stream;
            stream << table;
            const auto text = std::move(stream).str();
            if (text.size() > limits.file_bytes)
                return capacity();
            const auto bytes = std::as_bytes(std::span(text));
            return std::vector<std::byte>(bytes.begin(), bytes.end());
        }
        void preserveExtra(auto& value, bool extra, std::span<const std::byte> bytes)
        {
            if (extra)
                value.opaque.push_back({"lux.workspace.unrecognized.fields", 1, {bytes.begin(), bytes.end()}});
        }
    }
    WorkspaceResult<std::vector<std::byte>> encodeLayout(const DockLayout& value, WorkspaceLimits limits)
    {
        auto valid = ValidatedLayout::validate(value, limits);
        if (!valid)
            return lux::cxx::unexpected(valid.error());
        toml::table table{{"id", value.id.value}, {"label", value.label}};
        encodeCommon(table, value);
        toml::array slots, nodes, roots;
        for (const auto& slot : value.slots)
        {
            slots.push_back(toml::table{
                {"id", std::int64_t(slot.id.value)},
                {"key", slot.restore_key.name()},
                {"type", slot.type.name()},
                {"visible", slot.visible},
                {"state", envelope(slot.state.schema, slot.state.bytes)}
            });
        }
        for (const auto& node : value.dock.nodes)
        {
            toml::array members;
            for (const auto slot : node.slots)
                members.push_back(std::int64_t(slot.value));
            nodes.push_back(toml::table{
                {"id", std::int64_t(node.id)},
                {"split", std::int64_t(node.split)},
                {"first", std::int64_t(node.first)},
                {"second", std::int64_t(node.second)},
                {"ratio", node.ratio},
                {"slots", std::move(members)}
            });
        }
        for (const auto& root : value.dock.roots)
            roots.push_back(toml::table{
                {"node", std::int64_t(root.node)},
                {"x", root.x},
                {"y", root.y},
                {"width", root.width},
                {"height", root.height},
                {"floating", root.floating}
            });
        table.insert("slots", std::move(slots));
        table.insert("nodes", std::move(nodes));
        table.insert("roots", std::move(roots));
        return encode(table, limits);
    }
    WorkspaceResult<DockLayout> decodeLayout(std::span<const std::byte> bytes, WorkspaceLimits limits)
    {
        auto table = parse(bytes, limits);
        if (!table)
            return lux::cxx::unexpected(table.error());
        DockLayout value;
        bool extra = unknown(*table, {"schema", "opaque", "origin", "id", "label", "slots", "nodes", "roots"});
        auto common = decodeCommon(*table, value, limits, extra);
        if (!common)
            return lux::cxx::unexpected(common.error());
        value.id.value = (*table)["id"].value_or(std::string{});
        value.label = (*table)["label"].value_or(std::string{});
        const auto* slots = (*table)["slots"].as_array();
        const auto* nodes = (*table)["nodes"].as_array();
        const auto* roots = (*table)["roots"].as_array();
        if (!slots || !nodes || !roots)
            return malformed("layout arrays");
        if (slots->size() > limits.entries || nodes->size() > limits.entries || roots->size() > limits.entries)
            return capacity();
        for (const auto& item : *slots)
        {
            const auto* row = item.as_table();
            if (!row || !(*row)["state"].as_table() || !(*row)["visible"].is_boolean())
                return malformed("slot");
            const auto& state = *(*row)["state"].as_table();
            auto payload = unhex(state, limits);
            if (!payload)
                return lux::cxx::unexpected(payload.error());
            value.slots.push_back(
                {{number(*row, "id")},
                 views::ViewRestoreKey{(*row)["key"].value_or(std::string{})},
                 views::ViewTypeId{(*row)["type"].value_or(std::string{})},
                 (*row)["visible"].value_or(false),
                 {number(state, "schema"), std::move(*payload)}}
            );
            extra |=
                unknown(*row, {"id", "key", "type", "visible", "state"}) || unknown(state, {"schema", "size", "bytes"});
        }
        for (const auto& item : *nodes)
        {
            const auto* row = item.as_table();
            if (!row || !(*row)["slots"].as_array() || !(*row)["split"].is_integer())
                return malformed("dock node");
            for (const auto field : {"first", "second"})
            {
                const auto child = (*row)[field].value<std::int64_t>();
                if (!child || *child < 0 || *child > UINT32_MAX)
                    return malformed("dock child identity");
            }
            const auto split = (*row)["split"].value_or(std::int64_t{-1});
            if (split < 0 || split > 2)
                return malformed("split enum");
            DockNode node{
                number(*row, "id"),
                static_cast<EDockSplit>(split),
                number(*row, "first"),
                number(*row, "second"),
                (*row)["ratio"].value_or(std::numeric_limits<double>::quiet_NaN())
            };
            const auto& members = *(*row)["slots"].as_array();
            if (members.size() > limits.entries)
                return capacity();
            for (const auto& member : members)
            {
                auto id = member.value<std::int64_t>();
                if (!id || *id <= 0 || *id > UINT32_MAX)
                    return malformed("slot reference");
                node.slots.push_back({static_cast<std::uint32_t>(*id)});
            }
            value.dock.nodes.push_back(std::move(node));
            extra |= unknown(*row, {"id", "split", "first", "second", "ratio", "slots"});
        }
        for (const auto& item : *roots)
        {
            const auto* row = item.as_table();
            if (!row || !(*row)["floating"].is_boolean())
                return malformed("dock root");
            const auto nan = std::numeric_limits<double>::quiet_NaN();
            value.dock.roots.push_back(
                {number(*row, "node"),
                 (*row)["x"].value_or(nan),
                 (*row)["y"].value_or(nan),
                 (*row)["width"].value_or(nan),
                 (*row)["height"].value_or(nan),
                 (*row)["floating"].value_or(false)}
            );
            extra |= unknown(*row, {"node", "x", "y", "width", "height", "floating"});
        }
        preserveExtra(value, extra, bytes);
        auto valid = ValidatedLayout::validate(std::move(value), limits);
        if (!valid)
            return lux::cxx::unexpected(valid.error());
        return valid->value();
    }
    WorkspaceResult<std::vector<std::byte>> encodePreferences(const UserPreferences& value, WorkspaceLimits limits)
    {
        auto valid = validatePreferences(value, limits);
        if (!valid)
            return lux::cxx::unexpected(valid.error());
        toml::table table;
        encodeCommon(table, value);
        if (value.selected_layout)
            table.insert("selected", value.selected_layout->value);
        return encode(table, limits);
    }
    WorkspaceResult<UserPreferences> decodePreferences(std::span<const std::byte> bytes, WorkspaceLimits limits)
    {
        auto table = parse(bytes, limits);
        if (!table)
            return lux::cxx::unexpected(table.error());
        UserPreferences value;
        bool extra = unknown(*table, {"schema", "opaque", "origin", "selected"});
        auto common = decodeCommon(*table, value, limits, extra);
        if (!common)
            return lux::cxx::unexpected(common.error());
        if (table->contains("selected"))
            value.selected_layout = LayoutId{(*table)["selected"].value_or(std::string{})};
        preserveExtra(value, extra, bytes);
        auto valid = validatePreferences(value, limits);
        if (!valid)
            return lux::cxx::unexpected(valid.error());
        return value;
    }
    WorkspaceResult<std::vector<std::byte>> encodeRecovery(const RecoveryManifest& value, WorkspaceLimits limits)
    {
        auto valid = validateRecovery(value, limits);
        if (!valid)
            return lux::cxx::unexpected(valid.error());
        toml::table table;
        encodeCommon(table, value);
        toml::array entries;
        for (const auto& entry : value.entries)
        {
            toml::array contents;
            for (const auto& content : entry.contents)
                contents.push_back(toml::table{{"locator", content.locator}, {"unpersisted", content.unpersisted_changes}});
            toml::table row{{"key", entry.restore_key.name()}, {"type", entry.type.name()}, {"contents", std::move(contents)}};
            if (entry.primary)
                row.insert("primary", std::int64_t(*entry.primary));
            entries.push_back(std::move(row));
        }
        table.insert("entries", std::move(entries));
        return encode(table, limits);
    }
    WorkspaceResult<RecoveryManifest> decodeRecovery(std::span<const std::byte> bytes, WorkspaceLimits limits)
    {
        auto table = parse(bytes, limits);
        if (!table)
            return lux::cxx::unexpected(table.error());
        RecoveryManifest value;
        bool extra = unknown(*table, {"schema", "opaque", "origin", "entries"});
        auto common = decodeCommon(*table, value, limits, extra, 2);
        if (!common)
            return lux::cxx::unexpected(common.error());
        const auto* entries = (*table)["entries"].as_array();
        if (!entries)
            return malformed("recovery entries");
        if (entries->size() > limits.entries)
            return capacity();
        for (const auto& item : *entries)
        {
            const auto* row = item.as_table();
            if (!row)
                return malformed("recovery entry");
            RecoveryEntry entry{views::ViewRestoreKey{(*row)["key"].value_or(std::string{})},
                                views::ViewTypeId{(*row)["type"].value_or(std::string{})}};
            if (value.schema == 1)
            {
                if (!(*row)["unpersisted"].is_boolean())
                    return malformed("recovery entry");
                entry.contents.push_back({(*row)["locator"].value_or(std::string{}),
                                          (*row)["unpersisted"].value_or(false)});
                entry.primary = 0;
                extra |= unknown(*row, {"key", "type", "locator", "unpersisted"});
            }
            else
            {
                const auto* contents = (*row)["contents"].as_array();
                if (!contents)
                    return malformed("recovery contents");
                if (contents->size() > 64)
                    return capacity();
                for (const auto& child : *contents)
                {
                    const auto* content = child.as_table();
                    if (!content || !(*content)["unpersisted"].is_boolean())
                        return malformed("recovery content");
                    entry.contents.push_back({(*content)["locator"].value_or(std::string{}),
                                              (*content)["unpersisted"].value_or(false)});
                    extra |= unknown(*content, {"locator", "unpersisted"});
                }
                if (row->contains("primary"))
                {
                    const auto primary = (*row)["primary"].value<std::int64_t>();
                    if (!primary || *primary < 0 || *primary >= static_cast<std::int64_t>(entry.contents.size()))
                        return malformed("recovery primary");
                    entry.primary = static_cast<std::uint32_t>(*primary);
                }
                extra |= unknown(*row, {"key", "type", "contents", "primary"});
            }
            value.entries.push_back(std::move(entry));
        }
        value.schema = 2; // Read-only v1 conversion; no IO/publication occurs in this codec.
        preserveExtra(value, extra, bytes);
        auto valid = validateRecovery(value, limits);
        if (!valid)
            return lux::cxx::unexpected(valid.error());
        return value;
    }
}
