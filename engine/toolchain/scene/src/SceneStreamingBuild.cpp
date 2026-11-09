#include <lux/engine/toolchain/scene/SceneStreamingBuild.hpp>
#include <lux/engine/PartitionDeclaration.hpp>
#include <lux/engine/EngineMetaFunctionRegistry.hpp>
#include <lux/engine/assets/AssetRepository.hpp>
#include <lux/engine/scene/SceneContent.hpp>
#include <lux/engine/scene/SceneAssetCodec.hpp>
#include <lux/engine/serialization/Serialization.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <tuple>
#include <unordered_set>

namespace lux::engine::toolchain
{
    namespace
    {
        constexpr auto kInvalid = error::errorId("lux.scene.content.invalid");
        constexpr auto kCapacity = error::errorId("lux.scene.content.capacity");

        bool validateGeometryBounds(const spatial::Spatial3DQueryBounds& bounds) noexcept
        {
            return math::isFinite(bounds.minimum) && math::isFinite(bounds.maximum) &&
                bounds.minimum.x <= bounds.maximum.x && bounds.minimum.y <= bounds.maximum.y &&
                bounds.minimum.z <= bounds.maximum.z;
        }

        cxx::expected<world::SceneGridConfiguration, error::Error>
        decodeGridConfiguration(std::span<const std::byte> bytes, scene::ESceneSpatialDimension dimension) noexcept
        {
            world::SceneGridConfiguration config;
            const auto codec = serialization::makePortableValueCodec<world::SceneGridConfiguration>();
            auto decoded = codec.decode(bytes, &config);
            const bool invalid = !decoded || !math::isFinite(config.origin) || !std::isfinite(config.cell_size) ||
                config.cell_size <= 0 || (dimension == scene::ESceneSpatialDimension::TWO && config.origin.z != 0);
            if (invalid) return cxx::unexpected(error::Error{kInvalid});
            return config;
        }

        void
        buildSpatialNode(
            scene::SceneSpatialIndex& index, std::uint32_t node, std::uint32_t first, std::uint32_t count
        ) noexcept
        {
            auto& initial = index.nodes[node];
            initial.bounds = index.entries[first].bounds;
            std::array<double, 3> low{INFINITY, INFINITY, INFINITY};
            std::array<double, 3> high{-INFINITY, -INFINITY, -INFINITY};
            const auto center = [](const auto& bounds) noexcept
            {
                return std::array{
                    std::midpoint(bounds.minimum.x, bounds.maximum.x),
                    std::midpoint(bounds.minimum.y, bounds.maximum.y),
                    std::midpoint(bounds.minimum.z, bounds.maximum.z)
                };
            };
            for (const auto& entry : std::span(index.entries).subspan(first, count))
            {
                auto& b = initial.bounds;
                b.minimum.x = std::min(b.minimum.x, entry.bounds.minimum.x);
                b.minimum.y = std::min(b.minimum.y, entry.bounds.minimum.y);
                b.minimum.z = std::min(b.minimum.z, entry.bounds.minimum.z);
                b.maximum.x = std::max(b.maximum.x, entry.bounds.maximum.x);
                b.maximum.y = std::max(b.maximum.y, entry.bounds.maximum.y);
                b.maximum.z = std::max(b.maximum.z, entry.bounds.maximum.z);
                const auto c = center(entry.bounds);
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    low[axis] = std::min(low[axis], c[axis]);
                    high[axis] = std::max(high[axis], c[axis]);
                }
            }
            if (count <= 8)
            {
                initial.first = first;
                initial.count = count;
                return;
            }
            std::size_t axis = 0;
            for (std::size_t i = 1; i < static_cast<std::size_t>(index.dimension); ++i)
            {
                // Halving before subtraction avoids infinity for finite extreme coordinates.
                if (high[i] / 2 - low[i] / 2 > high[axis] / 2 - low[axis] / 2) axis = i;
            }
            auto entries = std::span(index.entries).subspan(first, count);
            std::ranges::sort(entries, [&](const auto& left, const auto& right) noexcept
            {
                const auto lc = center(left.bounds)[axis];
                const auto rc = center(right.bounds)[axis];
                const auto& a = left.bounds;
                const auto& b = right.bounds;
                return std::tuple{lc, left.group, a.minimum.x, a.minimum.y, a.minimum.z,
                    a.maximum.x, a.maximum.y, a.maximum.z} < std::tuple{rc, right.group,
                    b.minimum.x, b.minimum.y, b.minimum.z, b.maximum.x, b.maximum.y, b.maximum.z};
            });
            const auto child = static_cast<std::uint32_t>(index.nodes.size());
            initial.first = child;
            initial.count = 0;
            index.nodes.resize(index.nodes.size() + 2);
            const auto half = count / 2;
            buildSpatialNode(index, child, first, half);
            buildSpatialNode(index, child + 1, first + half, count - half);
        }

        cxx::expected<scene::SceneStreamingData, error::Error>
        buildLoadGroups(
            const scene::SceneData& source, std::span<const std::span<const scene::SceneObjectSummary>> summaries,
            const EngineMetaFunctionRegistry& metadata, std::size_t limit
        ) noexcept
        {
            std::size_t charged{};
            const auto charge = [&](std::size_t count, std::size_t size) noexcept
            {
                if (count && size > (limit - charged) / count) return false;
                charged += count * size;
                return true;
            };
            if (!charge(source.blocks.size(), 32 + sizeof(scene::SceneLoadGroup)))
                return cxx::unexpected(error::Error{kCapacity});
            std::unordered_set<world::WorldObjectId, world::WorldObjectIdHash> identities;
            for (std::uint32_t block = 0; block < summaries.size(); ++block)
            {
                for (const auto& object : summaries[block])
                {
                    // Cold hash scratch is budgeted conservatively; this is not an allocator byte counter.
                    if (!charge(1, sizeof(object) + 80) ||
                        !charge(object.geometry.size(), sizeof(scene::SceneGeometryBounds)))
                        return cxx::unexpected(error::Error{kCapacity});
                    if (!world::isValid(object.id) || !identities.insert(object.id).second)
                        return cxx::unexpected(error::Error{kInvalid});
                }
            }
            scene::SceneStreamingData output;
            output.block_groups.resize(source.blocks.size(), UINT32_MAX);
            // Each content block is independently loadable; no Entity-reference closure is built.
            for (std::uint32_t block = 0; block < source.blocks.size(); ++block)
            {
                if (!source.blocks[block].size) continue;
                output.block_groups[block] = static_cast<std::uint32_t>(output.groups.size());
                output.groups.push_back({{block}, {}});
            }
            const auto geometry_count = source.description.world.supported_geometries.size();
            constexpr auto geometry_bytes =
                sizeof(std::vector<scene::SceneSpatialEntry>) + sizeof(scene::SceneSpatialIndex);
            if (!charge(geometry_count, geometry_bytes))
                return cxx::unexpected(error::Error{kCapacity});
            std::vector<std::vector<scene::SceneSpatialEntry>> geometry_entries(geometry_count);
            for (std::uint32_t block = 0; block < summaries.size(); ++block)
            {
                for (const auto& object : summaries[block])
                {
                    for (const auto& bounds : object.geometry)
                    {
                        if (bounds.geometry >= geometry_count)
                            return cxx::unexpected(error::Error{kInvalid, {bounds.geometry}});
                        auto& owner = output.groups[output.block_groups[block]].geometry;
                        if (owner && *owner != bounds.geometry)
                            return cxx::unexpected(error::Error{kInvalid, {block}});
                        owner = bounds.geometry;
                        // Includes map scratch, tree and entry copies during the cold build.
                        if (!charge(1, 320)) return cxx::unexpected(error::Error{kCapacity});
                        geometry_entries[bounds.geometry].push_back({bounds.bounds, output.block_groups[block]});
                    }
                }
            }
            output.geometry_indexes.reserve(geometry_count);
            for (std::size_t geometry = 0; geometry < geometry_count; ++geometry)
            {
                const auto& choice = source.description.world.supported_geometries[geometry].partitioner;
                const auto* algorithm = metadata.findPartitioner(choice.name);
                const bool invalid = !algorithm || !algorithm->build_index ||
                    algorithm->configuration_version != choice.configuration_version;
                if (invalid) return cxx::unexpected(error::Error{kInvalid, {geometry}});
                auto index = algorithm->build_index(geometry_entries[geometry], choice.configuration);
                if (!index) return cxx::unexpected(index.error());
                if (index->dimension != algorithm->dimension) return cxx::unexpected(error::Error{kInvalid});
                output.geometry_indexes.push_back(std::move(*index));
            }
            for (auto block : source.bootstrap_blocks)
            {
                if (block >= output.block_groups.size() || output.block_groups[block] == UINT32_MAX)
                    return cxx::unexpected(error::Error{kInvalid});
                output.bootstrap_groups.push_back(output.block_groups[block]);
            }
            std::ranges::sort(output.bootstrap_groups);
            output.bootstrap_groups.erase(std::unique(output.bootstrap_groups.begin(), output.bootstrap_groups.end()),
                output.bootstrap_groups.end());
            return output;
        }
    }

    template <scene::ESceneSpatialDimension Dimension>
    cxx::expected<scene::SceneSpatialIndex, error::Error>
    buildSpatialIndex(std::span<const scene::SceneSpatialEntry> entries, std::span<const std::byte> bytes) noexcept
    {
        auto config = decodeGridConfiguration(bytes, Dimension);
        if (!config) return cxx::unexpected(config.error());
        if (entries.size() >= UINT32_MAX / 2) return cxx::unexpected(error::Error{kCapacity});
        using Cell = std::tuple<std::uint32_t, std::int64_t, std::int64_t, std::int64_t>;
        std::map<Cell, spatial::Spatial3DQueryBounds> cells;
        for (const auto& entry : entries)
        {
            const auto& bounds = entry.bounds;
            const bool invalid_2d = Dimension == scene::ESceneSpatialDimension::TWO &&
                (bounds.minimum.z != 0 || bounds.maximum.z != 0);
            if (!validateGeometryBounds(bounds) || invalid_2d || entry.group == UINT32_MAX)
                return cxx::unexpected(error::Error{kInvalid});
            std::array<double, 3> center{std::midpoint(bounds.minimum.x, bounds.maximum.x),
                std::midpoint(bounds.minimum.y, bounds.maximum.y), std::midpoint(bounds.minimum.z, bounds.maximum.z)};
            const std::array origin{config->origin.x, config->origin.y, config->origin.z};
            std::array<std::int64_t, 3> coordinate{};
            for (std::size_t axis = 0; axis < static_cast<std::size_t>(Dimension); ++axis)
            {
                const auto value = std::floor((center[axis] - origin[axis]) / config->cell_size);
                if (!std::isfinite(value) || value < -0x1p63 || value >= 0x1p63)
                    return cxx::unexpected(error::Error{kInvalid});
                coordinate[axis] = static_cast<std::int64_t>(value);
            }
            auto [found, inserted] = cells.try_emplace(
                std::tuple{entry.group, coordinate[0], coordinate[1], coordinate[2]}, bounds);
            if (!inserted)
            {
                auto& b = found->second;
                b.minimum.x = std::min(b.minimum.x, bounds.minimum.x);
                b.minimum.y = std::min(b.minimum.y, bounds.minimum.y);
                b.minimum.z = std::min(b.minimum.z, bounds.minimum.z);
                b.maximum.x = std::max(b.maximum.x, bounds.maximum.x);
                b.maximum.y = std::max(b.maximum.y, bounds.maximum.y);
                b.maximum.z = std::max(b.maximum.z, bounds.maximum.z);
            }
        }
        scene::SceneSpatialIndex index;
        index.dimension = Dimension;
        index.entries.reserve(cells.size());
        for (const auto& [key, bounds] : cells) index.entries.push_back({bounds, std::get<0>(key)});
        if (!index.entries.empty())
        {
            index.nodes.reserve(index.entries.size() * 2);
            index.nodes.emplace_back();
            buildSpatialNode(index, 0, 0, static_cast<std::uint32_t>(index.entries.size()));
        }
        return index;
    }

    template cxx::expected<scene::SceneSpatialIndex, error::Error>
    buildSpatialIndex<scene::ESceneSpatialDimension::TWO>(
        std::span<const scene::SceneSpatialEntry>, std::span<const std::byte>) noexcept;
    template cxx::expected<scene::SceneSpatialIndex, error::Error>
    buildSpatialIndex<scene::ESceneSpatialDimension::THREE>(
        std::span<const scene::SceneSpatialEntry>, std::span<const std::byte>) noexcept;

    exec::task<cxx::expected<scene::SceneStreamingData, error::Error>>
    buildSceneStreaming(
        assets::AssetRepository& assets, assets::AssetDataRef<scene::SceneData> base,
        const scene::SceneDescription& description, std::span<const scene::SceneBlockUpdate> updates,
        const EngineMetaFunctionRegistry& metadata, std::size_t max_metadata_bytes
    ) noexcept
    {
        if (base && !assets.isCurrent(base)) co_return cxx::unexpected(error::Error{assets::kAssetChanged});
        auto& execution = execution::ExecutionRuntime::instance();
        const auto main = execution.mainScheduler();
        const auto cpu = execution.cpuScheduler();
        scene::SceneData candidate;
        candidate.description = description;
        if (base)
        {
            candidate.blocks = base->blocks;
            candidate.bootstrap_blocks = base->bootstrap_blocks;
        }
        std::vector<const scene::SceneBlockUpdate*> ordered;
        ordered.reserve(updates.size());
        for (const auto& update : updates) ordered.push_back(&update);
        std::ranges::sort(ordered, {}, [](const auto* update) noexcept { return update->block; });
        auto slots = candidate.blocks.size();
        std::uint32_t previous = UINT32_MAX;
        for (const auto* update : ordered)
        {
            const bool invalid_slot = update->block == UINT32_MAX || update->block == previous || update->block > slots;
            const bool reuses_deleted = base && update->block < base->blocks.size() &&
                base->blocks[update->block].size == 0 && !update->objects.empty();
            if (invalid_slot || reuses_deleted || update->objects.size() != update->summaries.size())
                co_return cxx::unexpected(error::Error{kInvalid});
            if (update->block == slots) ++slots;
            previous = update->block;
            for (std::size_t i = 0; i < update->objects.size(); ++i)
            {
                if (update->objects[i].id != update->summaries[i].id)
                    co_return cxx::unexpected(error::Error{kInvalid});
            }
        }
        constexpr auto slot_bytes = sizeof(scene::SceneBlockDescription) +
            sizeof(std::vector<scene::SceneObjectSummary>) + sizeof(std::span<const scene::SceneObjectSummary>);
        if (slots >= UINT32_MAX || slots > max_metadata_bytes / slot_bytes)
            co_return cxx::unexpected(error::Error{kCapacity});
        candidate.blocks.resize(slots);
        std::size_t charged = slots * slot_bytes;
        std::vector<std::vector<scene::SceneObjectSummary>> old(slots);
        std::vector<std::span<const scene::SceneObjectSummary>> summaries(slots);
        const auto& geometries = description.world.supported_geometries;
        bool remapped = false;
        if (base)
        {
            const auto& previous = base->description.world.supported_geometries;
            remapped = geometries.size() < previous.size();
            for (std::size_t i = 0; i < std::min(geometries.size(), previous.size()); ++i)
                remapped = remapped || geometries[i].name != previous[i].name;
        }
        std::size_t changed = 0;
        for (std::size_t block = 0; block < slots; ++block)
        {
            auto& record = candidate.blocks[block];
            if (changed < ordered.size() && ordered[changed]->block == block)
            {
                const auto& update = *ordered[changed++];
                if (update.objects.empty())
                {
                    record = {};
                    continue;
                }
                auto digest = co_await stdexec::continues_on(stdexec::then(stdexec::schedule(cpu), [&]() noexcept
                    -> cxx::expected<cxx::algorithm::Sha256Digest, error::Error>
                {
                    std::vector<std::byte> bytes;
                    auto encoded = scene::encodeSceneSummary(update.summaries, bytes, max_metadata_bytes - charged);
                    if (!encoded) return cxx::unexpected(encoded.error());
                    return cxx::algorithm::Sha256::hash(bytes);
                }), main);
                if (!digest) co_return cxx::unexpected(digest.error());
                record.size = 1; // Only existence and summary digest enter the logical input hash.
                record.summary_digest = *digest;
                summaries[block] = update.summaries;
            }
            else if (record.size)
            {
                if (remapped) co_return cxx::unexpected(error::Error{kInvalid, {block}});
                if (!record.summary_size || record.summary_size > max_metadata_bytes - charged)
                    co_return cxx::unexpected(error::Error{kCapacity});
                auto bytes = co_await assets.readRange(base, record.offset, record.summary_size);
                if (!bytes) co_return cxx::unexpected(bytes.error());
                auto decoded = co_await stdexec::continues_on(stdexec::then(stdexec::schedule(cpu), [&]() noexcept
                {
                    if (cxx::algorithm::Sha256::hash(bytes->view()) != record.summary_digest)
                        return cxx::expected<std::vector<scene::SceneObjectSummary>, error::Error>{
                            cxx::unexpected(error::Error{kInvalid})};
                    return scene::decodeSceneSummary(bytes->view(), max_metadata_bytes - charged);
                }), main);
                if (!decoded) co_return cxx::unexpected(decoded.error());
                old[block] = std::move(*decoded);
                summaries[block] = old[block];
            }
            const auto charge = [&](std::size_t count, std::size_t bytes) noexcept
            {
                if (count && bytes > (max_metadata_bytes - charged) / count) return false;
                charged += count * bytes;
                return true;
            };
            if (!charge(summaries[block].size(), sizeof(scene::SceneObjectSummary)))
                co_return cxx::unexpected(error::Error{kCapacity});
            for (const auto& object : summaries[block])
            {
                if (!charge(object.geometry.size(), sizeof(scene::SceneGeometryBounds)))
                    co_return cxx::unexpected(error::Error{kCapacity});
            }
        }
        std::erase_if(candidate.bootstrap_blocks, [&](auto block) noexcept
        {
            return block >= candidate.blocks.size() || !candidate.blocks[block].size;
        });
        auto built = co_await stdexec::continues_on(stdexec::then(stdexec::schedule(cpu), [&]() noexcept
            -> cxx::expected<scene::SceneStreamingData, error::Error>
        {
            auto result = buildLoadGroups(candidate, summaries, metadata, max_metadata_bytes - charged);
            if (!result) return result;
            auto digest = scene::sceneStreamingInputDigest(candidate);
            if (!digest) return cxx::unexpected(digest.error());
            result->input_digest = *digest;
            return result;
        }), main);
        if (base && !assets.isCurrent(base)) co_return cxx::unexpected(error::Error{assets::kAssetChanged});
        co_return built;
    }
} // namespace lux::engine::toolchain
