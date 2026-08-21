#include <lux/engine/toolchain/entity_scene/TaggedPayloadTranscoder.hpp>

#include <lux/engine/core/serialization/Archive.hpp>
#include <lux/engine/core/serialization/NameTable.hpp>
#include <lux/engine/ecs/serialization/TaggedPropertyArchive.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <span>
#include <string_view>

namespace lux::toolchain
{
    namespace
    {
        constexpr std::uint32_t kMaximumNameCount = 1u << 20u;
        constexpr std::size_t kMaximumNameBytes = 4096u;
        constexpr std::uint32_t kMaximumNesting = 64u;

        [[nodiscard]] EntitySceneCookFailure failure(
            EEntitySceneCookError error,
            std::string detail)
        {
            return {error, std::move(detail)};
        }

        [[nodiscard]] bool readU32(
            std::span<const std::byte> bytes,
            std::size_t& cursor,
            std::uint32_t& value) noexcept
        {
            if (cursor > bytes.size() || bytes.size() - cursor < 4u)
                return false;
            value = static_cast<std::uint32_t>(bytes[cursor]) |
                (static_cast<std::uint32_t>(bytes[cursor + 1u]) << 8u) |
                (static_cast<std::uint32_t>(bytes[cursor + 2u]) << 16u) |
                (static_cast<std::uint32_t>(bytes[cursor + 3u]) << 24u);
            cursor += 4u;
            return true;
        }

        void appendU32(
            std::vector<std::byte>& bytes,
            std::uint32_t value)
        {
            for (std::size_t index = 0u; index < 4u; ++index)
            {
                bytes.push_back(static_cast<std::byte>(
                    (value >> (index * 8u)) & 0xffu));
            }
        }

        [[nodiscard]] bool validNameTable(
            std::span<const std::string> names) noexcept
        {
            if (names.empty() || !names.front().empty() ||
                names.size() > kMaximumNameCount)
            {
                return false;
            }
            std::vector<std::string_view> ordered;
            ordered.reserve(names.size() - 1u);
            for (std::size_t index = 1u; index < names.size(); ++index)
            {
                if (names[index].empty() ||
                    names[index].size() > kMaximumNameBytes)
                    return false;
                ordered.push_back(names[index]);
            }
            std::sort(ordered.begin(), ordered.end());
            return std::adjacent_find(ordered.begin(), ordered.end()) ==
                ordered.end();
        }

        [[nodiscard]] bool validCanonicalNameTable(
            std::span<const std::string> names) noexcept
        {
            if (!validNameTable(names))
                return false;
            return std::is_sorted(names.begin() + 1u, names.end());
        }

        [[nodiscard]] std::uint32_t canonicalIndex(
            std::span<const std::string> names,
            std::string_view name) noexcept
        {
            const auto found = std::lower_bound(
                names.begin() + 1u,
                names.end(),
                name,
                [](const std::string& lhs, std::string_view rhs)
                {
                    return lhs < rhs;
                });
            if (found == names.end() || *found != name)
                return std::numeric_limits<std::uint32_t>::max();
            return static_cast<std::uint32_t>(found - names.begin());
        }

        struct WalkContext final
        {
            std::span<const std::string> source_names;
            std::span<const std::string> destination_names;
            std::vector<std::string>* used_names{};
            std::vector<std::byte>* output{};
        };

        [[nodiscard]] bool walkObject(
            std::span<const std::byte> object,
            const WalkContext& context,
            std::uint32_t depth) noexcept
        {
            if (depth > kMaximumNesting)
                return false;

            std::size_t cursor = 0u;
            while (true)
            {
                std::uint32_t source_index = 0u;
                if (!readU32(object, cursor, source_index))
                    return false;
                if (source_index == lux::ecs::serialization::kEndOfObject)
                {
                    if (context.output)
                        appendU32(*context.output, source_index);
                    return cursor == object.size();
                }
                if (source_index == 0u ||
                    source_index >= context.source_names.size())
                {
                    return false;
                }

                if (object.size() - cursor < 5u)
                    return false;
                const auto wire_type = std::to_integer<std::uint8_t>(
                    object[cursor++]);
                std::uint32_t payload_size = 0u;
                if (!readU32(object, cursor, payload_size) ||
                    payload_size > object.size() - cursor)
                {
                    return false;
                }

                const auto& source_name = context.source_names[source_index];
                if (context.used_names)
                    context.used_names->push_back(source_name);

                if (context.output)
                {
                    const auto destination_index = canonicalIndex(
                        context.destination_names,
                        source_name);
                    if (destination_index ==
                        std::numeric_limits<std::uint32_t>::max())
                    {
                        return false;
                    }
                    appendU32(*context.output, destination_index);
                    context.output->push_back(static_cast<std::byte>(wire_type));
                    appendU32(*context.output, payload_size);
                }

                const auto payload = object.subspan(cursor, payload_size);
                if (wire_type == static_cast<std::uint8_t>(
                        lux::ecs::serialization::EArchiveType::Struct))
                {
                    if (!walkObject(payload, context, depth + 1u))
                        return false;
                }
                else if (context.output)
                {
                    context.output->insert(
                        context.output->end(),
                        payload.begin(),
                        payload.end());
                }
                cursor += payload_size;
            }
        }
    }

    lux::cxx::expected<std::vector<std::string>, EntitySceneCookFailure>
    decodeTaggedPayloadNameTable(std::span<const std::byte> image) noexcept
    {
        std::size_t cursor = 0u;
        std::uint32_t declared_count = 0u;
        if (!readU32(image, cursor, declared_count) || declared_count == 0u ||
            declared_count > kMaximumNameCount)
        {
            return lux::cxx::unexpected(failure(
                EEntitySceneCookError::INVALID_NAME_TABLE,
                "invalid serialized tagged-property NameTable header"));
        }

        lux::serialize::ArchiveReader reader{image.data(), image.size()};
        auto decoded = lux::serialize::NameTable::deserialize(reader);
        if (!reader.ok() || !reader.eof() || decoded.size() != declared_count)
        {
            return lux::cxx::unexpected(failure(
                EEntitySceneCookError::INVALID_NAME_TABLE,
                "malformed or trailing serialized tagged-property NameTable"));
        }

        std::vector<std::string> names;
        names.reserve(decoded.size());
        for (std::uint32_t index = 0u; index < decoded.size(); ++index)
            names.emplace_back(decoded.at(index));
        if (!validNameTable(names))
        {
            return lux::cxx::unexpected(failure(
                EEntitySceneCookError::INVALID_NAME_TABLE,
                "tagged-property NameTable has an invalid sentinel or duplicate name"));
        }
        return names;
    }

    lux::cxx::expected<std::vector<std::string>, EntitySceneCookFailure>
    canonicalTaggedPayloadNames(
        std::span<const TaggedPayloadSource> payloads) noexcept
    {
        std::vector<std::string> used_names;
        for (const auto& source : payloads)
        {
            if (!validNameTable(source.names) || source.payload.empty())
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCookError::INVALID_NAME_TABLE,
                    "tagged payload has an invalid source NameTable"));
            }
            WalkContext context{
                source.names,
                {},
                &used_names,
                nullptr};
            if (!walkObject(source.payload, context, 0u))
            {
                return lux::cxx::unexpected(failure(
                    EEntitySceneCookError::INVALID_TAGGED_PAYLOAD,
                    "tagged payload is truncated, nested incorrectly, or has a bad name index"));
            }
        }

        std::sort(used_names.begin(), used_names.end());
        used_names.erase(
            std::unique(used_names.begin(), used_names.end()),
            used_names.end());
        used_names.insert(used_names.begin(), std::string{});
        return used_names;
    }

    lux::cxx::expected<std::vector<std::byte>, EntitySceneCookFailure>
    transcodeTaggedPayloadNames(
        const TaggedPayloadSource& source,
        std::span<const std::string> canonical_names) noexcept
    {
        if (!validNameTable(source.names) ||
            !validCanonicalNameTable(canonical_names))
        {
            return lux::cxx::unexpected(failure(
                EEntitySceneCookError::INVALID_NAME_TABLE,
                "tagged payload source or destination NameTable is invalid"));
        }
        std::vector<std::byte> result;
        result.reserve(source.payload.size());
        const WalkContext context{
            source.names,
            canonical_names,
            nullptr,
            &result};
        if (!walkObject(source.payload, context, 0u) ||
            result.size() != source.payload.size())
        {
            return lux::cxx::unexpected(failure(
                EEntitySceneCookError::INVALID_TAGGED_PAYLOAD,
                "cannot transcode tagged payload into the canonical NameTable"));
        }
        return result;
    }
}
