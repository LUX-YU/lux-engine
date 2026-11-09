#include <lux/engine/scene/SceneStreamingCodec.hpp>
#include <lux/engine/serialization/Serialization.hpp>

#include <algorithm>
#include <array>
#include <tuple>

namespace lux::scene
{
    namespace
    {
        constexpr auto kInvalid = error::errorId("lux.scene.content.invalid");
        constexpr auto kCapacity = error::errorId("lux.scene.content.capacity");

        cxx::expected<void, error::Error>
        validateStreamingData(const SceneStreamingData& data, std::size_t limit) noexcept
        {
            const error::Error failure{kInvalid};
            for (const auto& group : data.groups)
            {
                const bool invalid_blocks = group.blocks.empty() || !std::ranges::is_sorted(group.blocks) ||
                    std::ranges::adjacent_find(group.blocks) != group.blocks.end();
                const bool invalid_geometry = group.geometry && *group.geometry >= data.geometry_indexes.size();
                if (invalid_blocks || invalid_geometry) return cxx::unexpected(failure);
            }
            // Bidirectional membership prevents duplicate or missing block ownership without another map.
            std::size_t group_members{};
            for (std::size_t group = 0; group < data.groups.size(); ++group)
            {
                for (auto block : data.groups[group].blocks)
                {
                    if (block >= data.block_groups.size() || data.block_groups[block] != group)
                        return cxx::unexpected(failure);
                    ++group_members;
                }
            }
            std::size_t mapped{};
            for (auto group : data.block_groups)
            {
                if (group == UINT32_MAX) continue;
                if (group >= data.groups.size()) return cxx::unexpected(failure);
                ++mapped;
            }
            if (mapped != group_members) return cxx::unexpected(failure);
            for (std::size_t geometry = 0; geometry < data.geometry_indexes.size(); ++geometry)
            {
                const auto& index = data.geometry_indexes[geometry];
                if (index.dimension != ESceneSpatialDimension::TWO && index.dimension != ESceneSpatialDimension::THREE)
                    return cxx::unexpected(failure);
                const auto valid_bounds = [](const spatial::Spatial3DQueryBounds& value) noexcept
                {
                    return math::isFinite(value.minimum) && math::isFinite(value.maximum) &&
                        value.minimum.x <= value.maximum.x && value.minimum.y <= value.maximum.y &&
                        value.minimum.z <= value.maximum.z;
                };
                for (const auto& node : index.nodes)
                {
                    if (!valid_bounds(node.bounds)) return cxx::unexpected(failure);
                }
                for (const auto& entry : index.entries)
                {
                    const bool invalid_group = entry.group >= data.groups.size() ||
                        data.groups[entry.group].geometry != geometry;
                    const bool invalid_2d = index.dimension == ESceneSpatialDimension::TWO &&
                        (entry.bounds.minimum.z != 0 || entry.bounds.maximum.z != 0);
                    if (invalid_group || invalid_2d || !valid_bounds(entry.bounds))
                        return cxx::unexpected(failure);
                }
                if (index.nodes.empty() != index.entries.empty()) return cxx::unexpected(failure);
                // All child indices must be forward. Counts also reject shared children and unreachable nodes.
                if (index.nodes.size() > limit / 2) return cxx::unexpected(error::Error{kCapacity});
                std::vector<std::uint8_t> parents(index.nodes.size());
                std::vector<std::uint8_t> depths(index.nodes.size(), 1);
                std::size_t next_entry{};
                const auto contains = [&](const auto& outer, const auto& inner) noexcept
                {
                    return outer.minimum.x <= inner.minimum.x && outer.maximum.x >= inner.maximum.x &&
                        outer.minimum.y <= inner.minimum.y && outer.maximum.y >= inner.maximum.y &&
                        outer.minimum.z <= inner.minimum.z && outer.maximum.z >= inner.maximum.z;
                };
                for (std::size_t n = 0; n < index.nodes.size(); ++n)
                {
                    const auto& node = index.nodes[n];
                    const bool invalid_parent = (n == 0 ? parents[n] != 0 : parents[n] != 1) || depths[n] > 63;
                    const bool invalid_2d = index.dimension == ESceneSpatialDimension::TWO &&
                        (node.bounds.minimum.z != 0 || node.bounds.maximum.z != 0);
                    if (invalid_parent || invalid_2d) return cxx::unexpected(failure);
                    if (!node.count)
                    {
                        if (node.first <= n || std::uint64_t(node.first) + 1 >= index.nodes.size())
                            return cxx::unexpected(failure);
                        for (auto child : {node.first, node.first + 1})
                        {
                            if (++parents[child] != 1 || !contains(node.bounds, index.nodes[child].bounds))
                                return cxx::unexpected(failure);
                            depths[child] = depths[n] + 1;
                        }
                    }
                    else
                    {
                        const bool invalid_range = node.count > 8 || node.first > index.entries.size() ||
                            node.count > index.entries.size() - node.first;
                        if (invalid_range) return cxx::unexpected(failure);
                        // Entry coverage is checked below in entry order, not in node allocation order.
                        next_entry += node.count;
                        for (const auto& entry : std::span(index.entries).subspan(node.first, node.count))
                        {
                            if (!contains(node.bounds, entry.bounds)) return cxx::unexpected(failure);
                        }
                    }
                }
                if (next_entry != index.entries.size()) return cxx::unexpected(failure);
                // Sorted disjoint leaf ranges cover every entry exactly once. Scratch remains cold and bounded.
                if (index.nodes.size() > limit / (2 + sizeof(std::pair<std::uint32_t, std::uint32_t>)))
                    return cxx::unexpected(error::Error{kCapacity});
                std::vector<std::pair<std::uint32_t, std::uint32_t>> leaves;
                for (const auto& node : index.nodes)
                {
                    if (node.count) leaves.emplace_back(node.first, node.count);
                }
                std::ranges::sort(leaves);
                next_entry = 0;
                for (const auto& [first, count] : leaves)
                {
                    if (first != next_entry) return cxx::unexpected(failure);
                    next_entry += count;
                }
            }
            const bool invalid_bootstrap = !std::ranges::is_sorted(data.bootstrap_groups) ||
                std::ranges::adjacent_find(data.bootstrap_groups) != data.bootstrap_groups.end() ||
                (!data.bootstrap_groups.empty() && data.bootstrap_groups.back() >= data.groups.size());
            if (invalid_bootstrap) return cxx::unexpected(failure);
            return {};
        }

        bool writeStreamingData(serialization::BinaryWriter& writer, const SceneStreamingData& data) noexcept
        {
            const auto count = [&](std::size_t size) noexcept
            {
                return writer.writeUnsigned(static_cast<std::uint64_t>(size));
            };
            const auto bounds = [&](const spatial::Spatial3DQueryBounds& value) noexcept
            {
                return writer.writeFloat(value.minimum.x) && writer.writeFloat(value.minimum.y) &&
                    writer.writeFloat(value.minimum.z) && writer.writeFloat(value.maximum.x) &&
                    writer.writeFloat(value.maximum.y) && writer.writeFloat(value.maximum.z);
            };
            const auto ordinals = [&](const auto& values) noexcept
            {
                if (!count(values.size())) return false;
                for (auto value : values)
                {
                    if (!writer.writeUnsigned(value)) return false;
                }
                return true;
            };
            if (!writer.writeBytes(data.input_digest.bytes()) || !count(data.groups.size()))
                return false;
            for (const auto& group : data.groups)
            {
                if (!writer.writeUnsigned(group.geometry.value_or(UINT32_MAX)) || !ordinals(group.blocks)) return false;
            }
            if (!ordinals(data.block_groups) || !count(data.geometry_indexes.size())) return false;
            for (const auto& index : data.geometry_indexes)
            {
                if (!writer.writeUnsigned(static_cast<std::uint8_t>(index.dimension)) || !count(index.nodes.size()))
                    return false;
                for (const auto& node : index.nodes)
                {
                    if (!bounds(node.bounds) || !writer.writeUnsigned(node.first) || !writer.writeUnsigned(node.count))
                        return false;
                }
                if (!count(index.entries.size())) return false;
                for (const auto& entry : index.entries)
                {
                    if (!bounds(entry.bounds) || !writer.writeUnsigned(entry.group)) return false;
                }
            }
            return ordinals(data.bootstrap_groups);
        }

        cxx::expected<SceneStreamingData, error::Error>
        readStreamingData(serialization::BinaryReader& reader, std::size_t& remaining_bytes) noexcept
        {
            SceneStreamingData data;
            error::Error failure{kInvalid};
            const auto limit = remaining_bytes;
            std::size_t charged = 0;
            const auto resize = [&](auto& values, std::size_t wire_size) noexcept
            {
                auto count = reader.readUnsigned<std::uint64_t>();
                using Value = typename std::remove_reference_t<decltype(values)>::value_type;
                const bool invalid = !count || *count >= UINT32_MAX || *count > reader.remaining() / wire_size;
                if (invalid) return false;
                if (*count > (limit - charged) / sizeof(Value))
                {
                    failure = error::Error{kCapacity};
                    return false;
                }
                charged += static_cast<std::size_t>(*count) * sizeof(Value);
                values.resize(static_cast<std::size_t>(*count));
                return true;
            };
            const auto ordinals = [&](auto& values) noexcept
            {
                if (!resize(values, 4)) return false;
                for (auto& value : values)
                {
                    auto decoded = reader.readUnsigned<std::uint32_t>();
                    if (!decoded) return false;
                    value = *decoded;
                }
                return true;
            };
            const auto bounds = [&](spatial::Spatial3DQueryBounds& value) noexcept
            {
                for (auto* axis : {&value.minimum.x, &value.minimum.y, &value.minimum.z,
                                  &value.maximum.x, &value.maximum.y, &value.maximum.z})
                {
                    auto decoded = reader.readFloat<double>();
                    if (!decoded) return false;
                    *axis = *decoded;
                }
                return true;
            };
            if (!reader.readBytes(std::as_writable_bytes(std::span(data.input_digest))) || !resize(data.groups, 12))
                return cxx::unexpected(failure);
            for (auto& group : data.groups)
            {
                auto geometry = reader.readUnsigned<std::uint32_t>();
                if (!geometry) return cxx::unexpected(failure);
                if (*geometry != UINT32_MAX) group.geometry = *geometry;
                if (!ordinals(group.blocks)) return cxx::unexpected(failure);
            }
            if (!ordinals(data.block_groups) || !resize(data.geometry_indexes, 17))
                return cxx::unexpected(failure);
            for (auto& index : data.geometry_indexes)
            {
                auto dimension = reader.readUnsigned<std::uint8_t>();
                if (!dimension || !resize(index.nodes, 56))
                    return cxx::unexpected(failure);
                index.dimension = static_cast<ESceneSpatialDimension>(*dimension);
                for (auto& node : index.nodes)
                {
                    if (!bounds(node.bounds)) return cxx::unexpected(failure);
                    auto first = reader.readUnsigned<std::uint32_t>();
                    auto count = reader.readUnsigned<std::uint32_t>();
                    if (!first || !count) return cxx::unexpected(failure);
                    node.first = *first;
                    node.count = *count;
                }
                if (!resize(index.entries, 52)) return cxx::unexpected(failure);
                for (auto& entry : index.entries)
                {
                    if (!bounds(entry.bounds)) return cxx::unexpected(failure);
                    auto group = reader.readUnsigned<std::uint32_t>();
                    if (!group) return cxx::unexpected(failure);
                    entry.group = *group;
                }
            }
            if (!ordinals(data.bootstrap_groups)) return cxx::unexpected(failure);
            if (auto valid = validateStreamingData(data, limit - charged); !valid)
                return cxx::unexpected(valid.error());
            remaining_bytes -= charged;
            return data;
        }
    }

    cxx::expected<void, error::Error>
    writeSceneStreaming(
        serialization::BinaryWriter& writer, const SceneStreamingData& data, std::size_t& remaining_bytes
    ) noexcept
    {
        const auto limit = remaining_bytes;
        std::size_t bytes = 64; // digest and four top-level counts; version belongs to the Scene root.
        const auto charge = [&](std::size_t count, std::size_t stride) noexcept
        {
            if (bytes > limit || (count && stride > (limit - bytes) / count)) return false;
            bytes += count * stride;
            return true;
        };
        if (!charge(data.groups.size(), 12) || !charge(data.block_groups.size(), 4) ||
            !charge(data.bootstrap_groups.size(), 4) || !charge(data.geometry_indexes.size(), 17))
            return cxx::unexpected(error::Error{kCapacity});
        for (const auto& group : data.groups)
        {
            if (!charge(group.blocks.size(), 4)) return cxx::unexpected(error::Error{kCapacity});
        }
        for (const auto& index : data.geometry_indexes)
        {
            if (!charge(index.nodes.size(), 56) || !charge(index.entries.size(), 52))
                return cxx::unexpected(error::Error{kCapacity});
        }
        if (auto valid = validateStreamingData(data, limit); !valid) return valid;
        if (!writeStreamingData(writer, data)) return cxx::unexpected(error::Error{kCapacity});
        remaining_bytes -= bytes;
        return {};
    }

    cxx::expected<SceneStreamingData, error::Error>
    readSceneStreaming(serialization::BinaryReader& reader, std::size_t& remaining_bytes) noexcept
    {
        return readStreamingData(reader, remaining_bytes);
    }
} // namespace lux::scene
