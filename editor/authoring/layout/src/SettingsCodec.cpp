#include <lux/engine/editor/workspace/SettingsDocument.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace lux::editor::settings
{
    namespace
    {
        auto failure(ESettingsError code, std::string detail)
        {
            return cxx::unexpected(SettingsFailure{code, std::move(detail)});
        }
        SettingsResult<toml::table> parse(std::string_view text, SettingsLimits limits)
        {
            if (text.size() > limits.file_bytes)
                return failure(ESettingsError::CAPACITY, "settings file bytes");
            auto parsed = toml::parse(text);
            if (!parsed)
                return failure(ESettingsError::INVALID_VALUE, "settings TOML syntax");
            std::vector<std::pair<const toml::node*, std::size_t>> pending{{&parsed.table(), 1}};
            while (!pending.empty())
            {
                const auto [node, depth] = pending.back();
                pending.pop_back();
                if (depth > limits.depth)
                    return failure(ESettingsError::CAPACITY, "settings nesting depth");
                if (const auto* table = node->as_table())
                    for (const auto& [key, child] : *table)
                        pending.emplace_back(&child, depth + 1);
                if (const auto* array = node->as_array())
                    for (const auto& child : *array)
                        pending.emplace_back(&child, depth + 1);
            }
            return std::move(parsed).table();
        }
        std::string hex(std::span<const std::byte> bytes)
        {
            constexpr char digits[] = "0123456789abcdef";
            std::string result;
            result.reserve(bytes.size() * 2);
            for (const auto byte : bytes)
            {
                const auto value = std::to_integer<unsigned>(byte);
                result += digits[value >> 4];
                result += digits[value & 15];
            }
            return result;
        }
        SettingsResult<std::vector<std::byte>> unhex(const toml::table& row, SettingsLimits limits)
        {
            auto text = row["bytes"].value<std::string>();
            auto size = row["size"].value<std::int64_t>();
            const bool has_length = text && size && *size >= 0;
            const bool is_invalid_length = !has_length || text->size() % 2 || std::uint64_t(*size) != text->size() / 2;
            if (is_invalid_length)
                return failure(ESettingsError::INVALID_VALUE, "settings payload length");
            if (std::uint64_t(*size) > limits.file_bytes)
                return failure(ESettingsError::CAPACITY, "settings payload bytes");
            auto digit = [](char c)
            {
                if (c >= '0' && c <= '9')
                    return int(c - '0');
                if (c >= 'a' && c <= 'f')
                    return int(c - 'a') + 10;
                return -1;
            };
            std::vector<std::byte> result;
            result.reserve(static_cast<std::size_t>(*size));
            for (std::size_t i{}; i < text->size(); i += 2)
            {
                const auto high = digit((*text)[i]), low = digit((*text)[i + 1]);
                if (high < 0 || low < 0)
                    return failure(ESettingsError::INVALID_VALUE, "settings payload hex");
                result.push_back(std::byte((high << 4) | low));
            }
            return result;
        }
        SettingsResult<void> validateDocument(const SettingsDocument& value, SettingsLimits limits)
        {
            if (value.schema != 1)
                return failure(ESettingsError::UNSUPPORTED_VERSION, "settings document schema");
            if (value.scope > ESettingsScope::LAUNCH)
                return failure(ESettingsError::INVALID_SCOPE, "settings document scope");
            if (value.values.size() > limits.values)
                return failure(ESettingsError::CAPACITY, "settings value count");
            std::unordered_set<std::string_view> identities;
            std::size_t bytes{};
            for (const auto& row : value.values)
            {
                const bool is_invalid_identity =
                    row.id.empty() || row.id.size() > 4096 || row.id.find('\0') != row.id.npos;
                if (is_invalid_identity || !row.schema)
                    return failure(ESettingsError::INVALID_VALUE, "settings value identity/schema");
                if (!identities.insert(row.id).second)
                    return failure(ESettingsError::DUPLICATE, row.id);
                if (row.bytes.size() > limits.file_bytes - bytes)
                    return failure(ESettingsError::CAPACITY, "settings payload total");
                bytes += row.bytes.size();
            }
            return {};
        }
    } // namespace
    SettingsResult<SettingsDocument> decodeSettings(std::span<const std::byte> bytes, SettingsLimits limits)
    {
        const std::string_view text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        auto parsed = parse(text, limits);
        if (!parsed)
            return cxx::unexpected(parsed.error());
        auto schema = (*parsed)["schema"].value<std::int64_t>();
        if (!schema || *schema != 1)
            return failure(ESettingsError::UNSUPPORTED_VERSION, "settings document schema");
        auto scope = (*parsed)["scope"].value<std::int64_t>();
        const bool is_invalid_scope = !scope || *scope < 0 || *scope > static_cast<int>(ESettingsScope::LAUNCH);
        if (is_invalid_scope)
            return failure(ESettingsError::INVALID_SCOPE, "settings document scope");
        const auto* rows = (*parsed)["values"].as_array();
        if (!rows)
            return failure(ESettingsError::INVALID_VALUE, "settings values array");
        if (rows->size() > limits.values)
            return failure(ESettingsError::CAPACITY, "settings value count");
        SettingsDocument result;
        result.scope = static_cast<ESettingsScope>(*scope);
        for (const auto& node : *rows)
        {
            const auto* row = node.as_table();
            if (!row)
                return failure(ESettingsError::INVALID_VALUE, "settings value row");
            auto id = (*row)["id"].value<std::string>();
            auto version = (*row)["schema"].value<std::int64_t>();
            const bool is_invalid_row = !id || !version || *version < 1 || *version > UINT32_MAX;
            if (is_invalid_row)
                return failure(ESettingsError::INVALID_VALUE, "settings value identity/schema");
            auto payload = unhex(*row, limits);
            if (!payload)
                return cxx::unexpected(payload.error());
            result.values.push_back({std::move(*id), static_cast<std::uint32_t>(*version), std::move(*payload)});
        }
        auto valid = validateDocument(result, limits);
        if (!valid)
            return cxx::unexpected(valid.error());
        result.preserved = text;
        return result;
    }
    SettingsResult<std::vector<std::byte>> encodeSettings(const SettingsDocument& value, SettingsLimits limits)
    {
        auto valid = validateDocument(value, limits);
        if (!valid)
            return cxx::unexpected(valid.error());
        toml::table output;
        if (!value.preserved.empty())
        {
            auto parsed = parse(value.preserved, limits);
            if (!parsed)
                return cxx::unexpected(parsed.error());
            const auto schema = (*parsed)["schema"].value<std::int64_t>();
            if (!schema || *schema != 1)
                return failure(ESettingsError::UNSUPPORTED_VERSION, "preserved settings document schema");
            output = std::move(*parsed);
        }
        const auto* old_values = output["values"].as_array();
        toml::array rows;
        for (const auto& value_row : value.values)
        {
            toml::table row;
            if (old_values)
                for (const auto& old_node : *old_values)
                    if (const auto* old_row = old_node.as_table();
                        old_row && (*old_row)["id"].value<std::string>() == value_row.id)
                    {
                        row = *old_row;
                        break;
                    }
            row.insert_or_assign("id", value_row.id);
            row.insert_or_assign("schema", std::int64_t(value_row.schema));
            row.insert_or_assign("size", std::int64_t(value_row.bytes.size()));
            row.insert_or_assign("bytes", hex(value_row.bytes));
            rows.push_back(std::move(row));
        }
        output.insert_or_assign("schema", std::int64_t(value.schema));
        output.insert_or_assign("scope", std::int64_t(value.scope));
        output.insert_or_assign("values", std::move(rows));
        std::ostringstream stream;
        stream << output;
        const auto text = std::move(stream).str();
        if (text.size() > limits.file_bytes)
            return failure(ESettingsError::CAPACITY, "settings file bytes");
        const auto bytes = std::as_bytes(std::span(text));
        return std::vector<std::byte>(bytes.begin(), bytes.end());
    }
} // namespace lux::editor::settings
