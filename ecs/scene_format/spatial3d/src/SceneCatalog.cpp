#include <lux/engine/ecs/scene_format/spatial3d/SceneCatalog.hpp>

#include <lux/engine/core/serialization/Archive.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>
#include <utility>

namespace lux::ecs::scene_format::spatial3d
{
    namespace
    {
        using Error = SceneCatalogError;
        using Failure = SceneCatalogFailure;

        [[nodiscard]] Failure failure(Error error, std::string detail)
        {
            return {error, std::move(detail)};
        }

        [[nodiscard]] bool bandLess(
            const SceneCatalogBand& lhs,
            const SceneCatalogBand& rhs) noexcept
        {
            if (lhs.source.name() != rhs.source.name())
                return lhs.source.name() < rhs.source.name();
            if (lhs.demand_channel.name() != rhs.demand_channel.name())
                return lhs.demand_channel.name() < rhs.demand_channel.name();
            if (lhs.level != rhs.level)
                return lhs.level < rhs.level;
            if (lhs.cell_world_size != rhs.cell_world_size)
                return lhs.cell_world_size < rhs.cell_world_size;
            if (lhs.active_distance_scale != rhs.active_distance_scale)
            {
                return lhs.active_distance_scale <
                    rhs.active_distance_scale;
            }
            return lhs.resident_distance_scale <
                rhs.resident_distance_scale;
        }

        [[nodiscard]] bool entryLess(
            const SceneCatalogEntry& lhs,
            const SceneCatalogEntry& rhs) noexcept
        {
            if (lhs.band != rhs.band)
                return lhs.band < rhs.band;
            if (lhs.coordinate != rhs.coordinate)
                return lhs.coordinate < rhs.coordinate;
            return lhs.section.value() < rhs.section.value();
        }

        [[nodiscard]] bool sameBandIdentity(
            const SceneCatalogBand& lhs,
            const SceneCatalogBand& rhs) noexcept
        {
            return lhs.source.view() == rhs.source.view() &&
                lhs.demand_channel.view() == rhs.demand_channel.view() &&
                lhs.level == rhs.level;
        }

        [[nodiscard]] bool validBand(
            const SceneCatalogBand& band) noexcept
        {
            return lux::ecs::scene_format::isValidStableId(band.source) &&
                lux::ecs::scene_format::isValidDemandChannelId(
                    band.demand_channel) &&
                std::isfinite(band.cell_world_size) &&
                band.cell_world_size > 0.0 &&
                std::isfinite(band.active_distance_scale) &&
                band.active_distance_scale > 0.0 &&
                std::isfinite(band.resident_distance_scale) &&
                band.resident_distance_scale >=
                    band.active_distance_scale;
        }

        void canonicalize(SceneCatalog& catalog)
        {
            std::vector<std::uint32_t> order(catalog.bands.size());
            std::iota(order.begin(), order.end(), 0u);
            std::ranges::sort(
                order,
                [&catalog](std::uint32_t lhs, std::uint32_t rhs)
                {
                    return bandLess(catalog.bands[lhs], catalog.bands[rhs]);
                });
            std::vector<std::uint32_t> remap(catalog.bands.size());
            std::vector<SceneCatalogBand> bands;
            bands.reserve(catalog.bands.size());
            for (std::uint32_t index = 0u; index < order.size(); ++index)
            {
                remap[order[index]] = index;
                bands.push_back(std::move(catalog.bands[order[index]]));
            }
            catalog.bands = std::move(bands);
            for (auto& entry : catalog.entries)
            {
                if (entry.band < remap.size())
                    entry.band = remap[entry.band];
            }
            std::ranges::sort(catalog.entries, entryLess);
        }
    }

    SceneCatalogResult<void>
    validateSceneCatalog(
        const SceneCatalog& config,
        const SceneCatalogCodecLimits& limits) noexcept
    {
        if (limits.maximum_bands == 0u ||
            limits.maximum_entries == 0u ||
            limits.maximum_encoded_bytes < 40u)
        {
            return lux::cxx::unexpected(failure(
                Error::InvalidArgument,
                "Spatial3D catalog codec limits are invalid"));
        }
        if (config.bands.empty() || config.entries.empty() ||
            config.bands.size() > limits.maximum_bands ||
            config.entries.size() > limits.maximum_entries)
        {
            return lux::cxx::unexpected(failure(
                Error::LimitExceeded,
                "Spatial3D catalog band/entry count violates its limit"));
        }
        if (!config.residency.valid())
        {
            return lux::cxx::unexpected(failure(
                Error::InvalidArgument,
                "Spatial3D resident capacity is invalid"));
        }
        // Validate every floating-point policy before invoking ordering
        // algorithms.  NaN cannot participate in a strict weak ordering and
        // therefore must never reach bandLess().
        if (std::ranges::any_of(
                config.bands,
                [](const auto& band) { return !validBand(band); }))
        {
            return lux::cxx::unexpected(failure(
                Error::InvalidBand,
                "Spatial3D catalog contains an invalid band"));
        }
        if (!std::ranges::is_sorted(config.bands, bandLess))
        {
            return lux::cxx::unexpected(failure(
                Error::NonCanonicalOrder,
                "Spatial3D catalog bands are not canonical"));
        }
        if (!std::ranges::is_sorted(config.entries, entryLess))
        {
            return lux::cxx::unexpected(failure(
                Error::NonCanonicalOrder,
                "Spatial3D catalog entries are not canonical"));
        }
        for (std::size_t index = 0u; index < config.bands.size(); ++index)
        {
            if (index != 0u &&
                 sameBandIdentity(
                     config.bands[index - 1u], config.bands[index]))
            {
                return lux::cxx::unexpected(failure(
                    Error::InvalidBand,
                    "Spatial3D catalog contains an invalid or duplicate "
                    "source/channel/level band identity"));
            }
        }
        std::vector<std::size_t> band_entries(config.bands.size(), 0u);
        std::set<uuids::uuid> sections;
        for (std::size_t index = 0u; index < config.entries.size(); ++index)
        {
            const auto& entry = config.entries[index];
            if (entry.band >= config.bands.size() || entry.section.empty())
            {
                return lux::cxx::unexpected(failure(
                    Error::InvalidEntry,
                    "Spatial3D catalog entry has an invalid band or Section"));
            }
            ++band_entries[entry.band];
            if (!sections.insert(entry.section.value()).second)
            {
                return lux::cxx::unexpected(failure(
                    Error::DuplicateSection,
                    "Spatial3D catalog repeats a Section id"));
            }
            if (index != 0u &&
                config.entries[index - 1u].band == entry.band &&
                config.entries[index - 1u].coordinate == entry.coordinate)
            {
                return lux::cxx::unexpected(failure(
                    Error::DuplicateLocation,
                    "Spatial3D catalog repeats a source/band/cell"));
            }
        }
        if (std::ranges::find(band_entries, 0u) != band_entries.end())
        {
            return lux::cxx::unexpected(failure(
                Error::InvalidBand,
                "Spatial3D catalog contains a band without entries"));
        }
        return {};
    }

    SceneCatalogResult<std::vector<std::byte>>
    encodeSceneCatalog(
        SceneCatalog config,
        const SceneCatalogCodecLimits& limits) noexcept
    {
        if (!config.residency.valid() ||
            std::ranges::any_of(
                config.bands,
                [](const auto& band) { return !validBand(band); }))
        {
            return lux::cxx::unexpected(failure(
                Error::InvalidBand,
                "Spatial3D catalog contains invalid capacity or band values"));
        }
        canonicalize(config);
        const auto valid = validateSceneCatalog(config, limits);
        if (!valid)
            return lux::cxx::unexpected(valid.error());
        std::vector<std::byte> bytes;
        lux::serialize::ArchiveWriter writer{bytes};
        writer.writePod(kSceneCatalogMagic);
        writer.writePod(kSceneCatalogSchemaVersion);
        writer.writePod(static_cast<std::uint32_t>(config.bands.size()));
        writer.writePod(static_cast<std::uint32_t>(config.entries.size()));
        writer.writePod(config.residency.maximum_decoded_bytes);
        writer.writePod(config.residency.maximum_entities);
        writer.writePod(config.residency.maximum_interest_sources);
        writer.writePod(config.residency.maximum_sections_per_interest);
        for (const auto& band : config.bands)
        {
            writer.writeString(band.source.name());
            writer.writeString(band.demand_channel.name());
            writer.writePod(band.level);
            writer.writePod(band.cell_world_size);
            writer.writePod(band.active_distance_scale);
            writer.writePod(band.resident_distance_scale);
        }
        for (const auto& entry : config.entries)
        {
            writer.writePod(entry.band);
            writer.writePod(entry.coordinate.x);
            writer.writePod(entry.coordinate.y);
            writer.writePod(entry.coordinate.z);
            writer.writeUuid(entry.section.value());
        }
        if (bytes.size() > limits.maximum_encoded_bytes)
        {
            return lux::cxx::unexpected(failure(
                Error::LimitExceeded,
                "Spatial3D catalog image exceeds its byte limit"));
        }
        return bytes;
    }

    SceneCatalogResult<SceneCatalog>
    decodeSceneCatalog(
        std::span<const std::byte> bytes,
        const SceneCatalogCodecLimits& limits) noexcept
    {
        constexpr std::size_t kHeaderBytes = 40u;
        if (bytes.size() > limits.maximum_encoded_bytes ||
            bytes.size() < kHeaderBytes)
        {
            return lux::cxx::unexpected(failure(
                Error::LimitExceeded,
                "Spatial3D catalog image violates its byte limit"));
        }
        lux::serialize::ArchiveReader reader{bytes.data(), bytes.size()};
        if (reader.readPod<std::uint32_t>() !=
            kSceneCatalogMagic)
        {
            return lux::cxx::unexpected(failure(
                Error::BadMagic, "Spatial3D catalog magic is invalid"));
        }
        if (reader.readPod<std::uint32_t>() !=
            kSceneCatalogSchemaVersion)
        {
            return lux::cxx::unexpected(failure(
                Error::UnsupportedVersion,
                "Spatial3D catalog schema version is unsupported"));
        }
        const auto band_count = reader.readPod<std::uint32_t>();
        const auto entry_count = reader.readPod<std::uint32_t>();
        SceneCatalogResidencyLimits residency;
        residency.maximum_decoded_bytes = reader.readPod<std::uint64_t>();
        residency.maximum_entities = reader.readPod<std::uint64_t>();
        residency.maximum_interest_sources = reader.readPod<std::uint32_t>();
        residency.maximum_sections_per_interest =
            reader.readPod<std::uint32_t>();
        if (!reader.ok() || band_count == 0u || entry_count == 0u ||
            band_count > limits.maximum_bands ||
            entry_count > limits.maximum_entries || !residency.valid())
        {
            return lux::cxx::unexpected(failure(
                Error::LimitExceeded,
                "Spatial3D catalog band/entry count violates its limit"));
        }
        // Reject impossible count declarations before reserving their
        // containers.  A band has two u32 string lengths, one u8 and three
        // doubles even when both strings are empty; an entry is fully fixed
        // width.  Without this lower-bound check a tiny hostile image could
        // request hundreds of MiB of allocations before the reader noticed
        // that the payload was truncated.
        constexpr std::size_t kMinimumBandBytes =
            sizeof(std::uint32_t) * 2u + sizeof(std::uint8_t) +
            sizeof(double) * 3u;
        constexpr std::size_t kEntryBytes =
            sizeof(std::uint32_t) + sizeof(std::int64_t) * 3u + 16u;
        const auto remaining = reader.remaining();
        if (band_count > remaining / kMinimumBandBytes)
        {
            return lux::cxx::unexpected(failure(
                Error::Truncated,
                "Spatial3D catalog band count exceeds the remaining image"));
        }
        const auto minimum_band_bytes =
            static_cast<std::size_t>(band_count) * kMinimumBandBytes;
        const auto after_bands_lower_bound = remaining - minimum_band_bytes;
        if (entry_count > after_bands_lower_bound / kEntryBytes)
        {
            return lux::cxx::unexpected(failure(
                Error::Truncated,
                "Spatial3D catalog entry count exceeds the remaining image"));
        }
        SceneCatalog result;
        result.residency = residency;
        result.bands.reserve(band_count);
        result.entries.reserve(entry_count);
        for (std::uint32_t index = 0u; index < band_count; ++index)
        {
            SceneCatalogBand band;
            band.source = SourceId{reader.readString()};
            band.demand_channel = lux::ecs::scene_format::DemandChannelId{
                reader.readString()};
            band.level = reader.readPod<std::uint8_t>();
            band.cell_world_size = reader.readPod<double>();
            band.active_distance_scale = reader.readPod<double>();
            band.resident_distance_scale = reader.readPod<double>();
            if (!reader.ok())
            {
                return lux::cxx::unexpected(failure(
                    Error::Truncated,
                    "Spatial3D catalog band is truncated"));
            }
            result.bands.push_back(std::move(band));
        }
        for (std::uint32_t index = 0u; index < entry_count; ++index)
        {
            SceneCatalogEntry entry;
            entry.band = reader.readPod<std::uint32_t>();
            entry.coordinate.x = reader.readPod<std::int64_t>();
            entry.coordinate.y = reader.readPod<std::int64_t>();
            entry.coordinate.z = reader.readPod<std::int64_t>();
            entry.section = lux::ecs::scene_format::EntitySectionId{
                reader.readUuid()};
            if (!reader.ok())
            {
                return lux::cxx::unexpected(failure(
                    Error::Truncated,
                    "Spatial3D catalog entry is truncated"));
            }
            result.entries.push_back(std::move(entry));
        }
        if (!reader.eof())
        {
            return lux::cxx::unexpected(failure(
                Error::TrailingBytes,
                "Spatial3D catalog image has trailing bytes"));
        }
        const auto valid = validateSceneCatalog(result, limits);
        if (!valid)
            return lux::cxx::unexpected(valid.error());
        return result;
    }
} // namespace lux::ecs::scene_format::spatial3d
