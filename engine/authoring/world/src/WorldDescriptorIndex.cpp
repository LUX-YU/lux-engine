#include <lux/engine/authoring/world/WorldDescriptorIndex.hpp>

#include <lux/engine/authoring/world/WorldSourceCodec.hpp>
#include <lux/engine/core/serialization/Archive.hpp>
#include <lux/cxx/algorithm/Sha256.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <list>
#include <limits>
#include <memory>
#include <ranges>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <Windows.h>
#endif

namespace lux::authoring
{
    namespace
    {
        using lux::serialize::ArchiveReader;
        using lux::serialize::ArchiveWriter;

        constexpr std::uint32_t kObjectMagic = 0x4f44584cu;
        constexpr std::uint32_t kObjectVersion = 2u;
        constexpr std::uint32_t kMaximumIndexActors = 16u * 1024u * 1024u;
        constexpr std::uint32_t kMaximumLayersPerActor = 1024u;
        constexpr std::uint64_t kMaximumRootBytes = 64ull * 1024ull * 1024ull;
        constexpr std::uint64_t kMaximumObjectBytes = 256ull * 1024ull * 1024ull;
        constexpr std::size_t kActorBucketCount = 4096u;
        constexpr std::size_t kSearchBucketCount = 4096u;
        constexpr std::size_t kActorObjectCacheBytes = 48u * 1024u * 1024u;
        constexpr std::size_t kSearchObjectCacheBytes = 16u * 1024u * 1024u;
        constexpr std::size_t kObjectCacheBytes =
            kActorObjectCacheBytes + kSearchObjectCacheBytes;
        constexpr std::size_t kMaximumIndexedTextBytes = 4096u;
        constexpr std::size_t kStagingShardCount = 64u;
        constexpr std::size_t kBucketsPerStagingShard =
            kActorBucketCount / kStagingShardCount;

        static_assert(kActorBucketCount == kSearchBucketCount);
        static_assert(
            kActorBucketCount % kStagingShardCount == 0u);

        enum class EObjectKind : std::uint8_t
        {
            PAGE,
            ACTOR_BUCKET,
            SEARCH_BUCKET
        };

        struct ObjectReference final
        {
            lux::cxx::algorithm::Sha256Digest digest;
            std::uint32_t actor_count{0u};
        };

        struct SearchRow final
        {
            std::uint64_t token{0u};
            lux::authoring::WorldActorId actor;

            friend bool operator==(const SearchRow&, const SearchRow&) =
                default;
        };

        [[nodiscard]] std::string idKey(const uuids::uuid& id)
        {
            return uuids::to_string(id);
        }

        [[nodiscard]] bool actorLess(
            const WorldDescriptorIndexActor& left,
            const WorldDescriptorIndexActor& right)
        {
            return std::ranges::lexicographical_compare(
                left.actor.value().as_bytes(),
                right.actor.value().as_bytes());
        }

        [[nodiscard]] std::uint16_t actorBucket(
            lux::authoring::WorldActorId actor) noexcept
        {
            const auto bytes = actor.value().as_bytes();
            return static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(bytes[0]) << 4u) |
                (static_cast<std::uint16_t>(bytes[1]) >> 4u));
        }

        [[nodiscard]] std::string macroKey(
            lux::authoring::PartitionSpaceId space,
            const lux::authoring::WorldMacroCoord& macro)
        {
            std::string result = uuids::to_string(space.value());
            result.push_back('/');
            result += std::to_string(static_cast<std::uint8_t>(macro.topology));
            if (const auto* planar = std::get_if<
                    lux::authoring::PlanarMacroCoord>(&macro.coordinate))
            {
                result += '/' + std::to_string(planar->a);
                result += '/' + std::to_string(planar->b);
            }
            else if (const auto* volume = std::get_if<
                         lux::authoring::VolumeMacroCoord>(&macro.coordinate))
            {
                result += '/' + std::to_string(volume->x);
                result += '/' + std::to_string(volume->y);
                result += '/' + std::to_string(volume->z);
            }
            return result;
        }

        [[nodiscard]] lux::cxx::expected<
            lux::cxx::algorithm::Sha256Digest,
            std::string>
        computeSourceDigest(const WorldSourceDocument& source)
        {
            auto encoded = encodeWorldSource(source);
            if (!encoded)
            {
                return lux::cxx::unexpected(std::move(encoded.error()));
            }
            return lux::cxx::algorithm::Sha256::hash(*encoded);
        }

        [[nodiscard]] lux::cxx::expected<std::vector<std::byte>, std::string>
        readFile(
            const std::filesystem::path& path,
            std::uint64_t maximum_bytes)
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream)
            {
                return lux::cxx::unexpected(std::string{"index object is absent"});
            }
            const auto end = stream.tellg();
            if (end < 0 || static_cast<std::uint64_t>(end) > maximum_bytes)
            {
                return lux::cxx::unexpected(std::string{"index object is too large"});
            }
            std::vector<std::byte> bytes(static_cast<std::size_t>(end));
            stream.seekg(0);
            if (!bytes.empty() && !stream.read(
                    reinterpret_cast<char*>(bytes.data()), end))
            {
                return lux::cxx::unexpected(
                    std::string{"cannot read index object"});
            }
            return bytes;
        }

        [[nodiscard]] lux::cxx::expected<void, std::string> atomicWrite(
            const std::filesystem::path& path,
            std::span<const std::byte> bytes)
        {
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
            {
                return lux::cxx::unexpected(
                    std::string{"cannot create Descriptor Index directory: "} +
                    error.message());
            }
            auto temporary = path;
            temporary += ".tmp";
            {
                std::ofstream stream(
                    temporary, std::ios::binary | std::ios::trunc);
                if (!stream || !stream.write(
                        reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size())))
                {
                    return lux::cxx::unexpected(
                        std::string{"cannot write Descriptor Index"});
                }
            }
#if defined(_WIN32)
            if (!::MoveFileExW(
                    temporary.c_str(),
                    path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                error = std::error_code(
                    static_cast<int>(::GetLastError()),
                    std::system_category());
            }
#else
            std::filesystem::rename(temporary, path, error);
#endif
            if (error)
            {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return lux::cxx::unexpected(
                    std::string{"cannot atomically replace Descriptor Index: "} +
                    error.message());
            }
            return {};
        }

        [[nodiscard]] std::filesystem::path objectPath(
            const std::filesystem::path& cache_file,
            const lux::cxx::algorithm::Sha256Digest& digest)
        {
            return cache_file.parent_path() / "objects" /
                (lux::cxx::algorithm::toHex(digest) + ".lxdo");
        }

        [[nodiscard]] std::string lower(std::string_view value)
        {
            std::string result(value);
            std::ranges::transform(
                result,
                result.begin(),
                [](unsigned char character)
                {
                    return static_cast<char>(std::tolower(character));
                });
            return result;
        }

        [[nodiscard]] std::uint64_t searchTokenHash(
            std::string_view token) noexcept
        {
            std::uint64_t value = 14695981039346656037ull;
            value ^= static_cast<std::uint8_t>(token.size());
            value *= 1099511628211ull;
            for (const auto character : token)
            {
                value ^= static_cast<std::uint8_t>(character);
                value *= 1099511628211ull;
            }
            return value;
        }

        [[nodiscard]] std::uint16_t searchBucket(
            std::uint64_t token) noexcept
        {
            return static_cast<std::uint16_t>(
                token & (kSearchBucketCount - 1u));
        }

        void appendSearchTokens(
            std::string_view value,
            std::vector<std::uint64_t>& tokens)
        {
            const auto normalized = lower(value);
            if (normalized.empty())
            {
                return;
            }
            const auto width = std::min<std::size_t>(3u, normalized.size());
            for (std::size_t offset = 0u;
                 offset + width <= normalized.size(); ++offset)
            {
                tokens.push_back(searchTokenHash(
                    std::string_view{normalized}.substr(offset, width)));
            }
        }

        [[nodiscard]] std::vector<std::uint64_t> searchTokens(
            const WorldDescriptorIndexActor& actor)
        {
            std::vector<std::uint64_t> result;
            result.reserve(
                actor.display_name.size() + actor.actor_class.size());
            appendSearchTokens(actor.display_name, result);
            appendSearchTokens(actor.actor_class, result);
            std::ranges::sort(result);
            result.erase(
                std::unique(result.begin(), result.end()), result.end());
            return result;
        }

        [[nodiscard]] bool searchRowLess(
            const SearchRow& left,
            const SearchRow& right) noexcept
        {
            if (left.token != right.token)
            {
                return left.token < right.token;
            }
            return std::ranges::lexicographical_compare(
                left.actor.value().as_bytes(),
                right.actor.value().as_bytes());
        }

        void writePosition(
            ArchiveWriter& writer,
            const WorldActorSourcePosition& position)
        {
            if (const auto* planar = std::get_if<
                    lux::math::Position2d>(&position))
            {
                writer.writePod(std::uint8_t{0u});
                writer.writePod(planar->x);
                writer.writePod(planar->y);
                return;
            }
            const auto& volume = std::get<lux::math::Position3d>(
                position);
            writer.writePod(std::uint8_t{1u});
            writer.writePod(volume.x);
            writer.writePod(volume.y);
            writer.writePod(volume.z);
        }

        [[nodiscard]] bool readPosition(
            ArchiveReader& reader,
            WorldActorSourcePosition& position)
        {
            const auto kind = reader.readPod<std::uint8_t>();
            if (kind == 0u)
            {
                const lux::math::Position2d value{
                    reader.readPod<double>(), reader.readPod<double>()};
                if (!reader.ok() || !lux::math::isFinite(value))
                {
                    return false;
                }
                position = value;
                return true;
            }
            if (kind == 1u)
            {
                const lux::math::Position3d value{
                    reader.readPod<double>(),
                    reader.readPod<double>(),
                    reader.readPod<double>()};
                if (!reader.ok() || !lux::math::isFinite(value))
                {
                    return false;
                }
                position = value;
                return true;
            }
            return false;
        }

        void writeActor(
            ArchiveWriter& writer,
            const WorldDescriptorIndexActor& actor)
        {
            writer.writeUuid(actor.actor.value());
            writer.writeUuid(actor.descriptor_page);
            writer.writeString(actor.display_name);
            writer.writeString(actor.actor_class);
            writer.writeUuid(actor.space.value());
            writePosition(writer, actor.position);
            for (const auto value : actor.bounds_half_extent)
            {
                writer.writePod(value);
            }
            writer.writePod(
                static_cast<std::uint32_t>(actor.data_layers.size()));
            for (const auto& layer : actor.data_layers)
            {
                writer.writeString(layer.name());
            }
        }

        [[nodiscard]] bool readActor(
            ArchiveReader& reader,
            WorldDescriptorIndexActor& actor)
        {
            actor.actor = lux::authoring::WorldActorId{
                reader.readUuid()};
            actor.descriptor_page = reader.readUuid();
            actor.display_name = reader.readString();
            actor.actor_class = reader.readString();
            actor.space = lux::authoring::PartitionSpaceId{reader.readUuid()};
            if (actor.display_name.size() > 1024u * 1024u ||
                actor.actor_class.size() > 1024u * 1024u ||
                !readPosition(reader, actor.position))
            {
                return false;
            }
            actor.bounds_half_extent = {
                reader.readPod<float>(),
                reader.readPod<float>(),
                reader.readPod<float>()};
            const auto layer_count = reader.readPod<std::uint32_t>();
            if (!reader.ok() || layer_count > kMaximumLayersPerActor)
            {
                return false;
            }
            actor.data_layers.reserve(layer_count);
            for (std::uint32_t layer = 0u; layer < layer_count; ++layer)
            {
                lux::authoring::DataLayerId id{reader.readString()};
                if (!id.valid())
                {
                    return false;
                }
                actor.data_layers.push_back(std::move(id));
            }
            return reader.ok();
        }

        [[nodiscard]] WorldDescriptorIndexActor indexActor(
            const WorldActorSourceDescriptor& actor,
            uuids::uuid page)
        {
            return {
                actor.id,
                page,
                actor.display_name,
                actor.actor_class,
                actor.space,
                actor.position,
                actor.bounds_half_extent,
                actor.data_layers};
        }

        [[nodiscard]] std::optional<lux::authoring::WorldMacroCoord> actorMacro(
            const WorldSourceDocument& source,
            const WorldDescriptorIndexActor& actor)
        {
            const auto space = std::ranges::find(
                source.spaces,
                actor.space,
                &lux::authoring::PartitionSpaceDescriptor::id);
            if (space == source.spaces.end())
            {
                return std::nullopt;
            }
            lux::authoring::WorldCellKey cell;
            cell.topology = space->topology;
            const auto coordinate = [&](double value)
                -> std::optional<std::int64_t>
            {
                if (!std::isfinite(value) ||
                    !lux::authoring::isValidCellEdge(space->cell_edge))
                {
                    return std::nullopt;
                }
                const auto result = std::floor(
                    value / static_cast<double>(space->cell_edge));
                if (result < static_cast<double>(
                        std::numeric_limits<std::int64_t>::min()) ||
                    result > static_cast<double>(
                        std::numeric_limits<std::int64_t>::max()))
                {
                    return std::nullopt;
                }
                return static_cast<std::int64_t>(result);
            };
            if (space->topology ==
                lux::authoring::EPartitionTopology::PLANAR_XY)
            {
                const auto* position = std::get_if<
                    lux::math::Position2d>(&actor.position);
                if (!position)
                {
                    return std::nullopt;
                }
                const auto x = coordinate(position->x);
                const auto y = coordinate(position->y);
                if (!x || !y)
                {
                    return std::nullopt;
                }
                cell.coordinate = lux::authoring::PlanarCellCoord{*x, *y};
            }
            else if (space->topology ==
                lux::authoring::EPartitionTopology::PLANAR_XZ)
            {
                const auto* position = std::get_if<
                    lux::math::Position3d>(&actor.position);
                if (!position)
                {
                    return std::nullopt;
                }
                const auto x = coordinate(position->x);
                const auto z = coordinate(position->z);
                if (!x || !z)
                {
                    return std::nullopt;
                }
                cell.coordinate = lux::authoring::PlanarCellCoord{*x, *z};
            }
            else
            {
                const auto* position = std::get_if<
                    lux::math::Position3d>(&actor.position);
                if (!position)
                {
                    return std::nullopt;
                }
                const auto x = coordinate(position->x);
                const auto y = coordinate(position->y);
                const auto z = coordinate(position->z);
                if (!x || !y || !z)
                {
                    return std::nullopt;
                }
                cell.coordinate = lux::authoring::VolumeCellCoord{*x, *y, *z};
            }
            return lux::authoring::macroCoordOf(
                cell, space->macro_edge_cells);
        }

        [[nodiscard]] bool validActor(
            const WorldSourceDocument& source,
            const WorldDescriptorIndexActor& actor,
            const WorldDescriptorPageReference& page)
        {
            const auto macro = actorMacro(source, actor);
            if (actor.actor.empty() || actor.descriptor_page != page.id ||
                actor.space != page.space || actor.display_name.empty() ||
                actor.actor_class.empty() ||
                actor.display_name.size() > kMaximumIndexedTextBytes ||
                actor.actor_class.size() > kMaximumIndexedTextBytes ||
                !macro || *macro != page.macro ||
                !std::isfinite(actor.bounds_half_extent[0]) ||
                !std::isfinite(actor.bounds_half_extent[1]) ||
                !std::isfinite(actor.bounds_half_extent[2]) ||
                actor.bounds_half_extent[0] < 0.0f ||
                actor.bounds_half_extent[1] < 0.0f ||
                actor.bounds_half_extent[2] < 0.0f)
            {
                return false;
            }
            std::unordered_set<std::string> memberships;
            for (const auto& layer : actor.data_layers)
            {
                const auto known = std::ranges::find(
                    source.data_layers,
                    layer);
                if (!layer.valid() || known == source.data_layers.end() ||
                    !memberships.insert(layer.name()).second)
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] lux::cxx::expected<std::vector<std::byte>, std::string>
        encodeObject(
            EObjectKind kind,
            std::uint16_t bucket,
            uuids::uuid page,
            std::vector<WorldDescriptorIndexActor> actors)
        {
            std::ranges::sort(actors, actorLess);
            if (actors.size() > kMaximumIndexActors ||
                std::adjacent_find(
                    actors.begin(), actors.end(), [](const auto& left, const auto& right)
                    {
                        return left.actor == right.actor;
                    }) != actors.end())
            {
                return lux::cxx::unexpected(
                    std::string{"Descriptor Index object has duplicate Actors"});
            }
            std::vector<std::byte> bytes;
            ArchiveWriter writer(bytes);
            writer.writePod(kObjectMagic);
            writer.writePod(kObjectVersion);
            writer.writePod(static_cast<std::uint8_t>(kind));
            writer.writePod(bucket);
            writer.writeUuid(page);
            writer.writePod(static_cast<std::uint32_t>(actors.size()));
            for (const auto& actor : actors)
            {
                writeActor(writer, actor);
            }
            if (bytes.size() > kMaximumObjectBytes)
            {
                return lux::cxx::unexpected(
                    std::string{"Descriptor Index object is too large"});
            }
            return bytes;
        }

        [[nodiscard]] lux::cxx::expected<
            std::vector<WorldDescriptorIndexActor>,
            std::string>
        decodeObject(
            std::span<const std::byte> bytes,
            EObjectKind expected_kind,
            std::uint16_t expected_bucket,
            uuids::uuid expected_page,
            std::uint32_t expected_count)
        {
            ArchiveReader reader(bytes.data(), bytes.size());
            if (reader.readPod<std::uint32_t>() != kObjectMagic ||
                reader.readPod<std::uint32_t>() != kObjectVersion ||
                reader.readPod<std::uint8_t>() !=
                    static_cast<std::uint8_t>(expected_kind) ||
                reader.readPod<std::uint16_t>() != expected_bucket ||
                reader.readUuid() != expected_page)
            {
                return lux::cxx::unexpected(
                    std::string{"invalid Descriptor Index object header"});
            }
            const auto count = reader.readPod<std::uint32_t>();
            if (!reader.ok() || count != expected_count ||
                count > kMaximumIndexActors)
            {
                return lux::cxx::unexpected(
                    std::string{"invalid Descriptor Index object count"});
            }
            std::vector<WorldDescriptorIndexActor> actors;
            actors.reserve(count);
            for (std::uint32_t index = 0u; index < count; ++index)
            {
                WorldDescriptorIndexActor actor;
                if (!readActor(reader, actor))
                {
                    break;
                }
                actors.push_back(std::move(actor));
            }
            if (!reader.ok() || !reader.eof() || actors.size() != count ||
                !std::ranges::is_sorted(actors, actorLess))
            {
                return lux::cxx::unexpected(
                    std::string{"malformed Descriptor Index object"});
            }
            auto canonical = encodeObject(
                expected_kind,
                expected_bucket,
                expected_page,
                actors);
            if (!canonical || canonical->size() != bytes.size() ||
                !std::ranges::equal(*canonical, bytes))
            {
                return lux::cxx::unexpected(
                    std::string{"non-canonical Descriptor Index object"});
            }
            return actors;
        }

        [[nodiscard]] lux::cxx::expected<ObjectReference, std::string>
        writeObject(
            const std::filesystem::path& cache_file,
            EObjectKind kind,
            std::uint16_t bucket,
            uuids::uuid page,
            std::vector<WorldDescriptorIndexActor> actors)
        {
            auto encoded = encodeObject(kind, bucket, page, std::move(actors));
            if (!encoded)
            {
                return lux::cxx::unexpected(std::move(encoded.error()));
            }
            const auto digest = lux::cxx::algorithm::Sha256::hash(*encoded);
            auto written = atomicWrite(objectPath(cache_file, digest), *encoded);
            if (!written)
            {
                return lux::cxx::unexpected(std::move(written.error()));
            }
            ArchiveReader reader(encoded->data(), encoded->size());
            (void)reader.readPod<std::uint32_t>();
            (void)reader.readPod<std::uint32_t>();
            (void)reader.readPod<std::uint8_t>();
            (void)reader.readPod<std::uint16_t>();
            (void)reader.readUuid();
            return ObjectReference{digest, reader.readPod<std::uint32_t>()};
        }

        [[nodiscard]] lux::cxx::expected<std::vector<std::byte>, std::string>
        encodeSearchObject(
            std::uint16_t bucket,
            std::vector<SearchRow> rows)
        {
            std::ranges::sort(rows, searchRowLess);
            if (rows.size() > (std::numeric_limits<std::uint32_t>::max)() ||
                std::adjacent_find(rows.begin(), rows.end()) != rows.end())
            {
                return lux::cxx::unexpected(
                    std::string{"Descriptor search object has duplicate rows"});
            }
            std::vector<std::byte> bytes;
            ArchiveWriter writer(bytes);
            writer.writePod(kObjectMagic);
            writer.writePod(kObjectVersion);
            writer.writePod(static_cast<std::uint8_t>(
                EObjectKind::SEARCH_BUCKET));
            writer.writePod(bucket);
            writer.writeUuid(uuids::uuid{});
            writer.writePod(static_cast<std::uint32_t>(rows.size()));
            for (const auto& row : rows)
            {
                writer.writePod(row.token);
                writer.writeUuid(row.actor.value());
            }
            if (bytes.size() > kMaximumObjectBytes)
            {
                return lux::cxx::unexpected(
                    std::string{"Descriptor search object is too large"});
            }
            return bytes;
        }

        [[nodiscard]] lux::cxx::expected<
            std::vector<SearchRow>,
            std::string>
        decodeSearchObject(
            std::span<const std::byte> bytes,
            std::uint16_t expected_bucket,
            std::uint32_t expected_count)
        {
            ArchiveReader reader(bytes.data(), bytes.size());
            if (reader.readPod<std::uint32_t>() != kObjectMagic ||
                reader.readPod<std::uint32_t>() != kObjectVersion ||
                reader.readPod<std::uint8_t>() !=
                    static_cast<std::uint8_t>(EObjectKind::SEARCH_BUCKET) ||
                reader.readPod<std::uint16_t>() != expected_bucket ||
                !reader.readUuid().is_nil())
            {
                return lux::cxx::unexpected(
                    std::string{"invalid Descriptor search object header"});
            }
            const auto count = reader.readPod<std::uint32_t>();
            if (!reader.ok() || count != expected_count)
            {
                return lux::cxx::unexpected(
                    std::string{"invalid Descriptor search object count"});
            }
            std::vector<SearchRow> rows;
            rows.reserve(count);
            for (std::uint32_t index = 0u; index < count; ++index)
            {
                SearchRow row{
                    reader.readPod<std::uint64_t>(),
                    lux::authoring::WorldActorId{
                        reader.readUuid()}};
                if (!reader.ok() || row.actor.empty() ||
                    searchBucket(row.token) != expected_bucket)
                {
                    return lux::cxx::unexpected(
                        std::string{"invalid Descriptor search row"});
                }
                rows.push_back(std::move(row));
            }
            if (!reader.ok() || !reader.eof() ||
                !std::ranges::is_sorted(rows, searchRowLess) ||
                std::adjacent_find(rows.begin(), rows.end()) != rows.end())
            {
                return lux::cxx::unexpected(
                    std::string{"malformed Descriptor search object"});
            }
            auto canonical = encodeSearchObject(expected_bucket, rows);
            if (!canonical || canonical->size() != bytes.size() ||
                !std::ranges::equal(*canonical, bytes))
            {
                return lux::cxx::unexpected(
                    std::string{"non-canonical Descriptor search object"});
            }
            return rows;
        }

        [[nodiscard]] lux::cxx::expected<ObjectReference, std::string>
        writeSearchObject(
            const std::filesystem::path& cache_file,
            std::uint16_t bucket,
            std::vector<SearchRow> rows)
        {
            auto encoded = encodeSearchObject(bucket, std::move(rows));
            if (!encoded)
            {
                return lux::cxx::unexpected(std::move(encoded.error()));
            }
            const auto digest = lux::cxx::algorithm::Sha256::hash(*encoded);
            auto written = atomicWrite(objectPath(cache_file, digest), *encoded);
            if (!written)
            {
                return lux::cxx::unexpected(std::move(written.error()));
            }
            ArchiveReader reader(encoded->data(), encoded->size());
            (void)reader.readPod<std::uint32_t>();
            (void)reader.readPod<std::uint32_t>();
            (void)reader.readPod<std::uint8_t>();
            (void)reader.readPod<std::uint16_t>();
            (void)reader.readUuid();
            return ObjectReference{digest, reader.readPod<std::uint32_t>()};
        }
    } // namespace

    struct WorldDescriptorIndex::Data final
    {
        struct CachedObject final
        {
            std::shared_ptr<const std::vector<WorldDescriptorIndexActor>> actors;
            std::size_t bytes{0u};
            std::list<std::string>::iterator lru;
        };
        struct CachedSearchObject final
        {
            std::shared_ptr<const std::vector<SearchRow>> rows;
            std::size_t bytes{0u};
            std::list<std::uint16_t>::iterator lru;
        };

        lux::authoring::WorldId world;
        lux::cxx::algorithm::Sha256Digest source_digest;
        WorldSourceDocument source;
        std::filesystem::path cache_file;
        std::size_t actor_count{0u};
        std::unordered_map<std::string, ObjectReference> page_objects;
        std::array<std::optional<ObjectReference>, kActorBucketCount>
            actor_buckets;
        std::array<std::optional<ObjectReference>, kSearchBucketCount>
            search_buckets;
        std::unordered_map<std::string, std::size_t> page_by_id;
        std::unordered_map<std::string, std::size_t> page_by_macro;
        mutable std::list<std::string> object_lru;
        mutable std::unordered_map<std::string, CachedObject> object_cache;
        mutable std::size_t object_cache_bytes{0u};
        mutable std::list<std::uint16_t> search_lru;
        mutable std::unordered_map<std::uint16_t, CachedSearchObject>
            search_cache;
        mutable std::size_t search_cache_bytes{0u};

        void rebuildPageMaps()
        {
            page_by_id.clear();
            page_by_macro.clear();
            page_by_id.reserve(source.descriptor_pages.size());
            page_by_macro.reserve(source.descriptor_pages.size());
            for (std::size_t index = 0u;
                 index < source.descriptor_pages.size(); ++index)
            {
                const auto& page = source.descriptor_pages[index];
                page_by_id.emplace(idKey(page.id), index);
                page_by_macro.emplace(
                    macroKey(page.space, page.macro), index);
            }
        }

        [[nodiscard]] lux::cxx::expected<
            std::shared_ptr<const std::vector<WorldDescriptorIndexActor>>,
            std::string>
        loadObject(
            EObjectKind kind,
            std::uint16_t bucket,
            uuids::uuid page,
            const ObjectReference& reference) const
        {
            const auto key = std::to_string(static_cast<std::uint8_t>(kind)) +
                "/" + std::to_string(bucket) + "/" + idKey(page);
            const auto cached = object_cache.find(key);
            if (cached != object_cache.end())
            {
                object_lru.splice(
                    object_lru.begin(), object_lru, cached->second.lru);
                return cached->second.actors;
            }
            auto bytes = readFile(
                objectPath(cache_file, reference.digest), kMaximumObjectBytes);
            if (!bytes)
            {
                return lux::cxx::unexpected(std::move(bytes.error()));
            }
            if (lux::cxx::algorithm::Sha256::hash(*bytes) != reference.digest)
            {
                return lux::cxx::unexpected(
                    std::string{"Descriptor Index object digest mismatch"});
            }
            auto decoded = decodeObject(
                *bytes, kind, bucket, page, reference.actor_count);
            if (!decoded)
            {
                return lux::cxx::unexpected(std::move(decoded.error()));
            }
            const WorldDescriptorPageReference* expected_page = nullptr;
            if (kind == EObjectKind::PAGE)
            {
                const auto found = page_by_id.find(idKey(page));
                if (found == page_by_id.end())
                {
                    return lux::cxx::unexpected(
                        std::string{"Descriptor Index object names an absent Page"});
                }
                expected_page = &source.descriptor_pages[found->second];
            }
            std::unordered_set<std::string> ids;
            ids.reserve(decoded->size());
            for (const auto& actor : *decoded)
            {
                const auto page_found = page_by_id.find(
                    idKey(actor.descriptor_page));
                if (page_found == page_by_id.end() ||
                    !validActor(
                        source,
                        actor,
                        source.descriptor_pages[page_found->second]) ||
                    (expected_page && actor.descriptor_page != expected_page->id) ||
                    (kind == EObjectKind::ACTOR_BUCKET &&
                     actorBucket(actor.actor) != bucket) ||
                    !ids.insert(idKey(actor.actor.value())).second)
                {
                    return lux::cxx::unexpected(
                        std::string{"Descriptor Index object has invalid Actor metadata"});
                }
            }
            auto owned = std::make_shared<const
                std::vector<WorldDescriptorIndexActor>>(std::move(*decoded));
            if (bytes->size() > kActorObjectCacheBytes)
            {
                return owned;
            }
            object_lru.push_front(key);
            object_cache.emplace(
                key, CachedObject{owned, bytes->size(), object_lru.begin()});
            object_cache_bytes += bytes->size();
            while (object_cache_bytes > kActorObjectCacheBytes &&
                   object_cache.size() > 1u)
            {
                const auto evicted = object_lru.back();
                object_lru.pop_back();
                const auto found = object_cache.find(evicted);
                object_cache_bytes -= found->second.bytes;
                object_cache.erase(found);
            }
            return owned;
        }

        [[nodiscard]] lux::cxx::expected<
            std::shared_ptr<const std::vector<SearchRow>>,
            std::string>
        loadSearchObject(
            std::uint16_t bucket,
            const ObjectReference& reference) const
        {
            const auto cached = search_cache.find(bucket);
            if (cached != search_cache.end())
            {
                search_lru.splice(
                    search_lru.begin(), search_lru, cached->second.lru);
                return cached->second.rows;
            }
            auto bytes = readFile(
                objectPath(cache_file, reference.digest), kMaximumObjectBytes);
            if (!bytes)
            {
                return lux::cxx::unexpected(std::move(bytes.error()));
            }
            if (lux::cxx::algorithm::Sha256::hash(*bytes) != reference.digest)
            {
                return lux::cxx::unexpected(
                    std::string{"Descriptor search object digest mismatch"});
            }
            auto decoded = decodeSearchObject(
                *bytes, bucket, reference.actor_count);
            if (!decoded)
            {
                return lux::cxx::unexpected(std::move(decoded.error()));
            }
            auto owned = std::make_shared<const std::vector<SearchRow>>(
                std::move(*decoded));
            if (bytes->size() > kSearchObjectCacheBytes)
            {
                return owned;
            }
            search_lru.push_front(bucket);
            search_cache.emplace(
                bucket,
                CachedSearchObject{
                    owned, bytes->size(), search_lru.begin()});
            search_cache_bytes += bytes->size();
            while (search_cache_bytes > kSearchObjectCacheBytes &&
                   search_cache.size() > 1u)
            {
                const auto evicted = search_lru.back();
                search_lru.pop_back();
                const auto found = search_cache.find(evicted);
                search_cache_bytes -= found->second.bytes;
                search_cache.erase(found);
            }
            return owned;
        }
    };

    struct WorldDescriptorIndexAccess final
    {
        [[nodiscard]] static std::shared_ptr<WorldDescriptorIndex::Data>
        makeData(
            const WorldSourceDocument& source,
            const std::filesystem::path& cache_file,
            const lux::cxx::algorithm::Sha256Digest& digest)
        {
            auto data = std::make_shared<WorldDescriptorIndex::Data>();
            data->world = source.world;
            data->source_digest = digest;
            data->source = source;
            data->cache_file = cache_file;
            for (const auto& page : source.descriptor_pages)
            {
                data->actor_count += page.actor_count;
            }
            data->rebuildPageMaps();
            return data;
        }

        [[nodiscard]] static WorldDescriptorIndex adopt(
            std::shared_ptr<WorldDescriptorIndex::Data> data)
        {
            WorldDescriptorIndex result;
            result.data_ = std::move(data);
            return result;
        }
    };

    namespace
    {
        template<class Data>
        [[nodiscard]] lux::cxx::expected<std::vector<std::byte>, std::string>
        encodeRoot(const Data& data)
        {
            if (data.world.empty() || data.source_digest ==
                    lux::cxx::algorithm::Sha256Digest{} ||
                data.actor_count > kMaximumIndexActors)
            {
                return lux::cxx::unexpected(
                    std::string{"invalid World Descriptor Index root"});
            }
            std::vector<std::byte> bytes;
            ArchiveWriter writer(bytes);
            writer.writePod(kWorldDescriptorIndexMagic);
            writer.writePod(kWorldDescriptorIndexVersion);
            writer.writeUuid(data.world.value());
            writer.writeBytes(
                data.source_digest.data(), data.source_digest.size());
            writer.writePod(static_cast<std::uint32_t>(data.actor_count));
            writer.writePod(static_cast<std::uint32_t>(
                data.source.descriptor_pages.size()));
            for (const auto& page : data.source.descriptor_pages)
            {
                const auto found = data.page_objects.find(idKey(page.id));
                if (found == data.page_objects.end() ||
                    found->second.actor_count != page.actor_count)
                {
                    return lux::cxx::unexpected(
                        std::string{"Descriptor Index root is missing a Page object"});
                }
                writer.writeUuid(page.id);
                writer.writeBytes(
                    found->second.digest.data(),
                    found->second.digest.size());
                writer.writePod(found->second.actor_count);
            }
            std::uint32_t bucket_count = 0u;
            for (const auto& bucket : data.actor_buckets)
            {
                bucket_count += bucket.has_value() ? 1u : 0u;
            }
            writer.writePod(bucket_count);
            for (std::uint16_t bucket = 0u;
                 bucket < data.actor_buckets.size(); ++bucket)
            {
                if (!data.actor_buckets[bucket])
                {
                    continue;
                }
                writer.writePod(bucket);
                writer.writeBytes(
                    data.actor_buckets[bucket]->digest.data(),
                    data.actor_buckets[bucket]->digest.size());
                writer.writePod(data.actor_buckets[bucket]->actor_count);
            }
            std::uint32_t search_bucket_count = 0u;
            for (const auto& bucket : data.search_buckets)
            {
                search_bucket_count += bucket.has_value() ? 1u : 0u;
            }
            writer.writePod(search_bucket_count);
            for (std::uint16_t bucket = 0u;
                 bucket < data.search_buckets.size(); ++bucket)
            {
                if (!data.search_buckets[bucket])
                {
                    continue;
                }
                writer.writePod(bucket);
                writer.writeBytes(
                    data.search_buckets[bucket]->digest.data(),
                    data.search_buckets[bucket]->digest.size());
                writer.writePod(
                    data.search_buckets[bucket]->actor_count);
            }
            return bytes;
        }

        template<class LoadPage>
        [[nodiscard]] lux::cxx::expected<WorldDescriptorIndex, std::string>
        buildIndex(
            const WorldSourceDocument& source,
            const std::filesystem::path& cache_file,
            LoadPage&& load_page)
        {
            auto digest = computeSourceDigest(source);
            if (!digest)
            {
                return lux::cxx::unexpected(std::move(digest.error()));
            }
            auto data = WorldDescriptorIndexAccess::makeData(
                source, cache_file, *digest);
            if (data->actor_count > kMaximumIndexActors)
            {
                return lux::cxx::unexpected(
                    std::string{"World has too many Actor descriptors"});
            }

            const auto nonce = std::chrono::steady_clock::now()
                .time_since_epoch().count();
            const auto staging = cache_file.parent_path() /
                ("lxdi-rebuild-" + std::to_string(nonce));
            std::error_code error;
            std::filesystem::create_directories(staging, error);
            if (error)
            {
                return lux::cxx::unexpected(
                    std::string{"cannot create Descriptor Index staging directory"});
            }

            std::array<bool, kActorBucketCount> used_buckets{};
            std::array<bool, kSearchBucketCount> used_search_buckets{};
            std::array<
                std::unique_ptr<std::ofstream>,
                kStagingShardCount> actor_shards;
            std::array<
                std::unique_ptr<std::ofstream>,
                kStagingShardCount> search_shards;

            const auto ensureShard = [&staging](
                                         auto& shards,
                                         std::size_t shard,
                                         std::string_view prefix)
                -> std::ofstream*
            {
                auto& stream = shards[shard];
                if (!stream)
                {
                    const auto path = staging /
                        (std::string{prefix} + std::to_string(shard) +
                         ".rows");
                    stream = std::make_unique<std::ofstream>(
                        path, std::ios::binary | std::ios::trunc);
                }
                if (!*stream)
                {
                    return nullptr;
                }
                return stream.get();
            };

            for (const auto& reference : source.descriptor_pages)
            {
                auto page = load_page(reference);
                if (!page)
                {
                    return lux::cxx::unexpected(std::move(page.error()));
                }
                if (page->id != reference.id ||
                    page->actors.size() != reference.actor_count)
                {
                    return lux::cxx::unexpected(
                        std::string{"Descriptor Page does not match LXWA"});
                }
                std::vector<WorldDescriptorIndexActor> page_actors;
                page_actors.reserve(page->actors.size());
                for (const auto& actor : page->actors)
                {
                    auto indexed = indexActor(actor, page->id);
                    if (!validActor(source, indexed, reference))
                    {
                        return lux::cxx::unexpected(
                            std::string{"Descriptor Page has invalid spatial metadata"});
                    }
                    page_actors.push_back(indexed);
                    for (const auto token : searchTokens(indexed))
                    {
                        const auto bucket = searchBucket(token);
                        const auto shard = bucket /
                            kBucketsPerStagingShard;
                        auto* stream = ensureShard(
                            search_shards, shard, "search-shard-");
                        if (stream == nullptr)
                        {
                            return lux::cxx::unexpected(
                                std::string{
                                    "cannot open Descriptor search staging shard"});
                        }
                        const auto actor_bytes = indexed.actor.value().as_bytes();
                        stream->write(
                            reinterpret_cast<const char*>(&bucket),
                            sizeof(bucket));
                        stream->write(
                            reinterpret_cast<const char*>(&token),
                            sizeof(token));
                        stream->write(
                            reinterpret_cast<const char*>(actor_bytes.data()),
                            static_cast<std::streamsize>(actor_bytes.size()));
                        if (!*stream)
                        {
                            return lux::cxx::unexpected(
                                std::string{
                                    "cannot write Descriptor search staging shard"});
                        }
                        used_search_buckets[bucket] = true;
                    }

                    const auto bucket = actorBucket(actor.id);
                    const auto shard = bucket / kBucketsPerStagingShard;
                    auto* stream = ensureShard(
                        actor_shards, shard, "actor-shard-");
                    if (stream == nullptr)
                    {
                        return lux::cxx::unexpected(
                            std::string{
                                "cannot open Descriptor Index staging shard"});
                    }
                    std::vector<std::byte> row;
                    ArchiveWriter writer(row);
                    writeActor(writer, indexed);
                    const auto size = static_cast<std::uint32_t>(row.size());
                    stream->write(
                        reinterpret_cast<const char*>(&bucket),
                        sizeof(bucket));
                    stream->write(
                        reinterpret_cast<const char*>(&size), sizeof(size));
                    stream->write(
                        reinterpret_cast<const char*>(row.data()),
                        static_cast<std::streamsize>(row.size()));
                    if (!*stream)
                    {
                        return lux::cxx::unexpected(
                            std::string{
                                "cannot write Descriptor Index staging shard"});
                    }
                    used_buckets[bucket] = true;
                }
                auto page_object = writeObject(
                    cache_file,
                    EObjectKind::PAGE,
                    0u,
                    page->id,
                    std::move(page_actors));
                if (!page_object)
                {
                    return lux::cxx::unexpected(std::move(page_object.error()));
                }
                data->page_objects.emplace(
                    idKey(page->id), std::move(*page_object));
            }

            for (auto& stream : actor_shards)
            {
                stream.reset();
            }
            for (auto& stream : search_shards)
            {
                stream.reset();
            }

            for (std::size_t shard = 0u;
                 shard < kStagingShardCount; ++shard)
            {
                const auto path = staging /
                    ("actor-shard-" + std::to_string(shard) + ".rows");
                if (!std::filesystem::exists(path))
                {
                    continue;
                }
                std::ifstream stream(path, std::ios::binary);
                std::array<
                    std::vector<WorldDescriptorIndexActor>,
                    kBucketsPerStagingShard> shard_actors;
                while (stream.peek() != std::char_traits<char>::eof())
                {
                    std::uint16_t bucket = 0u;
                    std::uint32_t size = 0u;
                    stream.read(
                        reinterpret_cast<char*>(&bucket), sizeof(bucket));
                    stream.read(reinterpret_cast<char*>(&size), sizeof(size));
                    if (!stream || size == 0u || size > kMaximumObjectBytes ||
                        bucket >= kActorBucketCount ||
                        bucket / kBucketsPerStagingShard != shard)
                        return lux::cxx::unexpected(
                            std::string{"malformed Descriptor Index staging row"});
                    std::vector<std::byte> row(size);
                    stream.read(
                        reinterpret_cast<char*>(row.data()),
                        static_cast<std::streamsize>(row.size()));
                    ArchiveReader reader(row.data(), row.size());
                    WorldDescriptorIndexActor actor;
                    if (!stream || !readActor(reader, actor) ||
                        !reader.eof())
                    {
                        return lux::cxx::unexpected(
                            std::string{"malformed Descriptor Index staging row"});
                    }
                    shard_actors[bucket % kBucketsPerStagingShard].push_back(
                        std::move(actor));
                }
                for (std::size_t local = 0u;
                     local < kBucketsPerStagingShard; ++local)
                {
                    const auto bucket = static_cast<std::uint16_t>(
                        shard * kBucketsPerStagingShard + local);
                    if (!used_buckets[bucket])
                    {
                        continue;
                    }
                    auto object = writeObject(
                        cache_file,
                        EObjectKind::ACTOR_BUCKET,
                        bucket,
                        {},
                        std::move(shard_actors[local]));
                    if (!object)
                    {
                        return lux::cxx::unexpected(
                            std::move(object.error()));
                    }
                    data->actor_buckets[bucket] = std::move(*object);
                }
            }

            for (std::size_t shard = 0u;
                 shard < kStagingShardCount; ++shard)
            {
                const auto path = staging /
                    ("search-shard-" + std::to_string(shard) + ".rows");
                if (!std::filesystem::exists(path))
                {
                    continue;
                }
                std::ifstream stream(path, std::ios::binary);
                std::array<
                    std::vector<SearchRow>,
                    kBucketsPerStagingShard> shard_rows;
                while (stream.peek() != std::char_traits<char>::eof())
                {
                    std::uint16_t bucket = 0u;
                    std::uint64_t token = 0u;
                    std::array<uuids::uuid::value_type, 16u> actor_bytes{};
                    stream.read(
                        reinterpret_cast<char*>(&bucket), sizeof(bucket));
                    stream.read(
                        reinterpret_cast<char*>(&token), sizeof(token));
                    stream.read(
                        reinterpret_cast<char*>(actor_bytes.data()),
                        static_cast<std::streamsize>(actor_bytes.size()));
                    SearchRow row{
                        token,
                        lux::authoring::WorldActorId{
                            uuids::uuid(actor_bytes)}};
                    if (!stream || bucket >= kSearchBucketCount ||
                        bucket / kBucketsPerStagingShard != shard ||
                        row.actor.empty() ||
                        searchBucket(row.token) != bucket)
                    {
                        return lux::cxx::unexpected(
                            std::string{"malformed Descriptor search staging row"});
                    }
                    shard_rows[bucket % kBucketsPerStagingShard].push_back(
                        std::move(row));
                }
                for (std::size_t local = 0u;
                     local < kBucketsPerStagingShard; ++local)
                {
                    const auto bucket = static_cast<std::uint16_t>(
                        shard * kBucketsPerStagingShard + local);
                    if (!used_search_buckets[bucket])
                    {
                        continue;
                    }
                    auto object = writeSearchObject(
                        cache_file, bucket, std::move(shard_rows[local]));
                    if (!object)
                    {
                        return lux::cxx::unexpected(
                            std::move(object.error()));
                    }
                    data->search_buckets[bucket] = std::move(*object);
                }
            }

            auto root = encodeRoot(*data);
            if (!root)
            {
                return lux::cxx::unexpected(std::move(root.error()));
            }
            auto committed = atomicWrite(cache_file, *root);
            if (!committed)
            {
                return lux::cxx::unexpected(std::move(committed.error()));
            }
            std::filesystem::remove_all(staging, error);

            return WorldDescriptorIndexAccess::adopt(std::move(data));
        }
    } // namespace

    WorldDescriptorIndex::WorldDescriptorIndex()
        : data_(std::make_shared<Data>())
    {
    }

    WorldDescriptorIndex::~WorldDescriptorIndex() = default;
    WorldDescriptorIndex::WorldDescriptorIndex(
        const WorldDescriptorIndex&) noexcept = default;
    WorldDescriptorIndex& WorldDescriptorIndex::operator=(
        const WorldDescriptorIndex&) noexcept = default;
    WorldDescriptorIndex::WorldDescriptorIndex(
        WorldDescriptorIndex&&) noexcept = default;
    WorldDescriptorIndex& WorldDescriptorIndex::operator=(
        WorldDescriptorIndex&&) noexcept = default;

    lux::cxx::expected<WorldDescriptorIndex, std::string>
    WorldDescriptorIndex::rebuild(
        const std::filesystem::path& world_file,
        const WorldSourceDocument& source,
        const std::filesystem::path& cache_file)
    {
        return buildIndex(
            source,
            cache_file,
            [&](const WorldDescriptorPageReference& reference)
            {
                return loadWorldDescriptorPage(world_file, source, reference);
            });
    }

    lux::cxx::expected<WorldDescriptorIndex, std::string>
    WorldDescriptorIndex::fromPages(
        const WorldSourceDocument& source,
        std::span<const WorldDescriptorPageDocument> pages,
        const std::filesystem::path& cache_file)
    {
        std::unordered_map<std::string, const WorldDescriptorPageDocument*>
            by_id;
        by_id.reserve(pages.size());
        for (const auto& page : pages)
        {
            by_id.emplace(idKey(page.id), &page);
        }
        return buildIndex(
            source,
            cache_file,
            [&](const WorldDescriptorPageReference& reference)
                -> lux::cxx::expected<WorldDescriptorPageDocument, std::string>
            {
                const auto found = by_id.find(idKey(reference.id));
                if (found == by_id.end())
                {
                    return lux::cxx::unexpected(
                        std::string{"Descriptor Index input is incomplete"});
                }
                return *found->second;
            });
    }

    lux::cxx::expected<WorldDescriptorIndex, std::string>
    WorldDescriptorIndex::load(
        const std::filesystem::path& cache_file,
        const WorldSourceDocument& source)
    {
        auto expected_digest = computeSourceDigest(source);
        if (!expected_digest)
        {
            return lux::cxx::unexpected(std::move(expected_digest.error()));
        }
        auto bytes = readFile(cache_file, kMaximumRootBytes);
        if (!bytes)
        {
            return lux::cxx::unexpected(std::move(bytes.error()));
        }
        ArchiveReader reader(bytes->data(), bytes->size());
        if (reader.readPod<std::uint32_t>() != kWorldDescriptorIndexMagic ||
            reader.readPod<std::uint32_t>() != kWorldDescriptorIndexVersion)
        {
            return lux::cxx::unexpected(
                std::string{"invalid LXDI v5 header"});
        }
        const auto world = lux::authoring::WorldId{reader.readUuid()};
        lux::cxx::algorithm::Sha256Digest source_digest;
        reader.readBytes(
            source_digest.data(), source_digest.size());
        const auto actor_count = reader.readPod<std::uint32_t>();
        const auto page_count = reader.readPod<std::uint32_t>();
        if (!reader.ok() || world != source.world ||
            source_digest != *expected_digest ||
            actor_count > kMaximumIndexActors ||
            page_count != source.descriptor_pages.size())
        {
            return lux::cxx::unexpected(
                std::string{"stale or malformed World Descriptor Index"});
        }
        auto data = WorldDescriptorIndexAccess::makeData(
            source, cache_file, source_digest);
        if (data->actor_count != actor_count)
        {
            return lux::cxx::unexpected(
                std::string{"World Descriptor Index Actor count mismatch"});
        }
        for (std::uint32_t index = 0u; index < page_count; ++index)
        {
            const auto page = reader.readUuid();
            ObjectReference reference;
            reader.readBytes(
                reference.digest.data(), reference.digest.size());
            reference.actor_count = reader.readPod<std::uint32_t>();
            const auto found = data->page_by_id.find(idKey(page));
            if (!reader.ok() || found == data->page_by_id.end() ||
                reference.digest == lux::cxx::algorithm::Sha256Digest{} ||
                reference.actor_count !=
                    source.descriptor_pages[found->second].actor_count ||
                !data->page_objects.emplace(
                     idKey(page), std::move(reference)).second)
            {
                return lux::cxx::unexpected(
                    std::string{"malformed Descriptor Index Page table"});
            }
        }
        const auto bucket_count = reader.readPod<std::uint32_t>();
        if (!reader.ok() || bucket_count > kActorBucketCount)
        {
            return lux::cxx::unexpected(
                std::string{"malformed Descriptor Index bucket table"});
        }
        std::uint64_t bucket_actors = 0u;
        for (std::uint32_t index = 0u; index < bucket_count; ++index)
        {
            const auto bucket = reader.readPod<std::uint16_t>();
            ObjectReference reference;
            reader.readBytes(
                reference.digest.data(), reference.digest.size());
            reference.actor_count = reader.readPod<std::uint32_t>();
            if (!reader.ok() || bucket >= kActorBucketCount ||
                reference.digest == lux::cxx::algorithm::Sha256Digest{} ||
                reference.actor_count == 0u ||
                data->actor_buckets[bucket])
            {
                return lux::cxx::unexpected(
                    std::string{"malformed Descriptor Index bucket table"});
            }
            bucket_actors += reference.actor_count;
            data->actor_buckets[bucket] = std::move(reference);
        }
        const auto search_bucket_count = reader.readPod<std::uint32_t>();
        if (!reader.ok() || search_bucket_count > kSearchBucketCount)
        {
            return lux::cxx::unexpected(
                std::string{"malformed Descriptor Index search table"});
        }
        for (std::uint32_t index = 0u;
             index < search_bucket_count; ++index)
        {
            const auto bucket = reader.readPod<std::uint16_t>();
            ObjectReference reference;
            reader.readBytes(
                reference.digest.data(), reference.digest.size());
            reference.actor_count = reader.readPod<std::uint32_t>();
            if (!reader.ok() || bucket >= kSearchBucketCount ||
                reference.digest == lux::cxx::algorithm::Sha256Digest{} ||
                reference.actor_count == 0u ||
                data->search_buckets[bucket])
            {
                return lux::cxx::unexpected(
                    std::string{"malformed Descriptor Index search table"});
            }
            data->search_buckets[bucket] = std::move(reference);
        }
        if (!reader.ok() || !reader.eof() || bucket_actors != actor_count)
        {
            return lux::cxx::unexpected(
                std::string{"malformed World Descriptor Index root"});
        }
        return WorldDescriptorIndexAccess::adopt(std::move(data));
    }

    lux::cxx::expected<void, std::string>
    WorldDescriptorIndex::updatePages(
        const WorldSourceDocument& source,
        std::span<const WorldDescriptorPageDocument> changed_pages)
    {
        if (!data_ || data_->cache_file.empty() || source.world != data_->world)
        {
            return lux::cxx::unexpected(
                std::string{"Descriptor Index update has no durable root"});
        }
        auto digest = computeSourceDigest(source);
        if (!digest)
        {
            return lux::cxx::unexpected(std::move(digest.error()));
        }
        auto next = WorldDescriptorIndexAccess::makeData(
            source, data_->cache_file, *digest);
        next->page_objects = data_->page_objects;
        next->actor_buckets = data_->actor_buckets;
        next->search_buckets = data_->search_buckets;

        std::unordered_map<std::string, const WorldDescriptorPageDocument*>
            replacements;
        replacements.reserve(changed_pages.size());
        for (const auto& page : changed_pages)
        {
            const auto found = next->page_by_id.find(idKey(page.id));
            if (found == next->page_by_id.end() || page.world != source.world ||
                page.space != source.descriptor_pages[found->second].space ||
                page.macro != source.descriptor_pages[found->second].macro ||
                page.actors.size() !=
                    source.descriptor_pages[found->second].actor_count ||
                !replacements.emplace(idKey(page.id), &page).second)
            {
                return lux::cxx::unexpected(
                    std::string{"changed Descriptor Page does not match LXWA"});
            }
        }
        for (const auto& [page, _] : data_->page_objects)
        {
            if (!next->page_by_id.contains(page))
            {
                replacements.emplace(page, nullptr);
            }
        }
        for (const auto& page : source.descriptor_pages)
        {
            const auto previous = data_->page_by_id.find(idKey(page.id));
            if (previous == data_->page_by_id.end())
            {
                if (!replacements.contains(idKey(page.id)))
                {
                    return lux::cxx::unexpected(
                        std::string{"new Descriptor Page payload is missing"});
                }
                continue;
            }
            if (data_->source.descriptor_pages[previous->second] != page &&
                !replacements.contains(idKey(page.id)))
            {
                return lux::cxx::unexpected(
                    std::string{"changed Descriptor Page payload is missing"});
            }
        }

        std::unordered_set<std::uint16_t> affected_buckets;
        std::unordered_set<std::uint16_t> affected_search_buckets;
        std::unordered_set<std::string> replaced_page_ids;
        std::unordered_set<std::string> removed_actor_ids;
        for (const auto& [page_key, replacement] : replacements)
        {
            replaced_page_ids.insert(page_key);
            const auto old_reference = data_->page_objects.find(page_key);
            if (old_reference != data_->page_objects.end())
            {
                const auto old_page = data_->page_by_id.find(page_key);
                const auto page_id = old_page == data_->page_by_id.end()
                    ? uuids::uuid::from_string(page_key).value()
                    : data_->source.descriptor_pages[old_page->second].id;
                auto actors = data_->loadObject(
                    EObjectKind::PAGE, 0u, page_id, old_reference->second);
                if (!actors)
                {
                    return lux::cxx::unexpected(std::move(actors.error()));
                }
                for (const auto& actor : **actors)
                {
                    affected_buckets.insert(actorBucket(actor.actor));
                    removed_actor_ids.insert(idKey(actor.actor.value()));
                    for (const auto token : searchTokens(actor))
                    {
                        affected_search_buckets.insert(searchBucket(token));
                    }
                }
            }
            if (replacement)
            {
                for (const auto& actor : replacement->actors)
                {
                    affected_buckets.insert(actorBucket(actor.id));
                    const auto indexed = indexActor(actor, replacement->id);
                    for (const auto token : searchTokens(indexed))
                    {
                        affected_search_buckets.insert(searchBucket(token));
                    }
                }
            }
        }

        for (const auto bucket : affected_buckets)
        {
            std::vector<WorldDescriptorIndexActor> actors;
            if (data_->actor_buckets[bucket])
            {
                auto old = data_->loadObject(
                    EObjectKind::ACTOR_BUCKET,
                    bucket,
                    {},
                    *data_->actor_buckets[bucket]);
                if (!old)
                {
                    return lux::cxx::unexpected(std::move(old.error()));
                }
                for (const auto& actor : **old)
                {
                    if (!replaced_page_ids.contains(
                            idKey(actor.descriptor_page)))
                    {
                        actors.push_back(actor);
                    }
                }
            }
            for (const auto& [_, replacement] : replacements)
            {
                if (!replacement)
                {
                    continue;
                }
                for (const auto& actor : replacement->actors)
                {
                    if (actorBucket(actor.id) == bucket)
                    {
                        actors.push_back(indexActor(actor, replacement->id));
                    }
                }
            }
            if (actors.empty())
            {
                next->actor_buckets[bucket].reset();
                continue;
            }
            auto object = writeObject(
                next->cache_file,
                EObjectKind::ACTOR_BUCKET,
                bucket,
                {},
                std::move(actors));
            if (!object)
            {
                return lux::cxx::unexpected(std::move(object.error()));
            }
            next->actor_buckets[bucket] = std::move(*object);
        }

        for (const auto bucket : affected_search_buckets)
        {
            std::vector<SearchRow> rows;
            if (data_->search_buckets[bucket])
            {
                auto old = data_->loadSearchObject(
                    bucket, *data_->search_buckets[bucket]);
                if (!old)
                {
                    return lux::cxx::unexpected(std::move(old.error()));
                }
                rows.reserve((*old)->size());
                for (const auto& row : **old)
                {
                    if (!removed_actor_ids.contains(
                            idKey(row.actor.value())))
                    {
                        rows.push_back(row);
                    }
                }
            }
            for (const auto& [_, replacement] : replacements)
            {
                if (!replacement)
                {
                    continue;
                }
                for (const auto& actor : replacement->actors)
                {
                    const auto indexed = indexActor(actor, replacement->id);
                    for (const auto token : searchTokens(indexed))
                    {
                        if (searchBucket(token) == bucket)
                            rows.push_back(SearchRow{token, actor.id});
                    }
                }
            }
            if (rows.empty())
            {
                next->search_buckets[bucket].reset();
                continue;
            }
            auto object = writeSearchObject(
                next->cache_file, bucket, std::move(rows));
            if (!object)
            {
                return lux::cxx::unexpected(std::move(object.error()));
            }
            next->search_buckets[bucket] = std::move(*object);
        }

        for (const auto& [page_key, replacement] : replacements)
        {
            if (!replacement)
            {
                next->page_objects.erase(page_key);
                continue;
            }
            std::vector<WorldDescriptorIndexActor> actors;
            actors.reserve(replacement->actors.size());
            for (const auto& actor : replacement->actors)
            {
                auto indexed = indexActor(actor, replacement->id);
                const auto reference = next->page_by_id.find(page_key);
                if (reference == next->page_by_id.end() ||
                    !validActor(
                        source,
                        indexed,
                        source.descriptor_pages[reference->second]))
                {
                    return lux::cxx::unexpected(
                        std::string{"changed Descriptor Page has invalid Actor"});
                }
                actors.push_back(std::move(indexed));
            }
            auto object = writeObject(
                next->cache_file,
                EObjectKind::PAGE,
                0u,
                replacement->id,
                std::move(actors));
            if (!object)
            {
                return lux::cxx::unexpected(std::move(object.error()));
            }
            next->page_objects[page_key] = std::move(*object);
        }

        auto root = encodeRoot(*next);
        if (!root)
        {
            return lux::cxx::unexpected(std::move(root.error()));
        }
        auto committed = atomicWrite(next->cache_file, *root);
        if (!committed)
        {
            return lux::cxx::unexpected(std::move(committed.error()));
        }
        data_ = std::move(next);
        return {};
    }

    std::optional<WorldDescriptorIndexActor> WorldDescriptorIndex::find(
        lux::authoring::WorldActorId actor) const
    {
        if (!data_ || actor.empty())
        {
            return std::nullopt;
        }
        const auto bucket = actorBucket(actor);
        if (!data_->actor_buckets[bucket])
        {
            return std::nullopt;
        }
        const auto loaded = data_->loadObject(
            EObjectKind::ACTOR_BUCKET,
            bucket,
            {},
            *data_->actor_buckets[bucket]);
        if (!loaded)
        {
            return std::nullopt;
        }
        const auto found = std::lower_bound(
            (*loaded)->begin(),
            (*loaded)->end(),
            actor,
            [](const WorldDescriptorIndexActor& row,
               lux::authoring::WorldActorId key)
            {
                return std::ranges::lexicographical_compare(
                    row.actor.value().as_bytes(), key.value().as_bytes());
            });
        return found == (*loaded)->end() || found->actor != actor
            ? std::nullopt
            : std::optional<WorldDescriptorIndexActor>{*found};
    }

    const WorldDescriptorPageReference* WorldDescriptorIndex::page(
        uuids::uuid page_id) const noexcept
    {
        if (!data_)
        {
            return nullptr;
        }
        const auto found = data_->page_by_id.find(idKey(page_id));
        return found == data_->page_by_id.end()
            ? nullptr
            : &data_->source.descriptor_pages[found->second];
    }

    const WorldDescriptorPageReference* WorldDescriptorIndex::page(
        lux::authoring::PartitionSpaceId space,
        const lux::authoring::WorldMacroCoord& macro) const noexcept
    {
        if (!data_)
        {
            return nullptr;
        }
        const auto found = data_->page_by_macro.find(macroKey(space, macro));
        return found == data_->page_by_macro.end()
            ? nullptr
            : &data_->source.descriptor_pages[found->second];
    }

    std::vector<lux::authoring::WorldActorId>
    WorldDescriptorIndex::actorsInPage(uuids::uuid page_id) const
    {
        std::vector<lux::authoring::WorldActorId> result;
        if (!data_)
        {
            return result;
        }
        const auto reference = data_->page_objects.find(idKey(page_id));
        if (reference == data_->page_objects.end())
        {
            return result;
        }
        const auto loaded = data_->loadObject(
            EObjectKind::PAGE, 0u, page_id, reference->second);
        if (!loaded)
        {
            return result;
        }
        result.reserve((*loaded)->size());
        for (const auto& actor : **loaded)
        {
            result.push_back(actor.actor);
        }
        return result;
    }

    std::vector<WorldDescriptorIndexActor> WorldDescriptorIndex::search(
        std::string_view text,
        std::size_t offset,
        std::size_t maximum) const
    {
        std::vector<WorldDescriptorIndexActor> result;
        if (!data_ || maximum == 0u)
        {
            return result;
        }
        result.reserve(maximum);
        const auto needle = lower(text);
        if (!needle.empty())
        {
            if (needle.size() > kMaximumIndexedTextBytes)
            {
                return result;
            }
            std::vector<std::uint64_t> tokens;
            appendSearchTokens(needle, tokens);
            std::ranges::sort(tokens);
            tokens.erase(
                std::unique(tokens.begin(), tokens.end()), tokens.end());
            if (tokens.empty())
            {
                return result;
            }
            std::optional<std::uint64_t> selected_token;
            std::uint32_t selected_cost =
                (std::numeric_limits<std::uint32_t>::max)();
            for (const auto token : tokens)
            {
                const auto& reference =
                    data_->search_buckets[searchBucket(token)];
                if (!reference)
                {
                    return result;
                }
                if (reference->actor_count < selected_cost)
                {
                    selected_token = token;
                    selected_cost = reference->actor_count;
                }
            }
            const auto bucket = searchBucket(*selected_token);
            const auto loaded = data_->loadSearchObject(
                bucket, *data_->search_buckets[bucket]);
            if (!loaded)
            {
                return result;
            }
            auto row = std::lower_bound(
                (*loaded)->begin(),
                (*loaded)->end(),
                *selected_token,
                [](const SearchRow& candidate, std::uint64_t token)
                {
                    return candidate.token < token;
                });
            std::size_t skipped = 0u;
            for (; row != (*loaded)->end() &&
                   row->token == *selected_token; ++row)
            {
                const auto actor = find(row->actor);
                if (!actor ||
                    (lower(actor->display_name).find(needle) ==
                         std::string::npos &&
                     lower(actor->actor_class).find(needle) ==
                         std::string::npos))
                {
                    continue;
                }
                if (skipped++ < offset)
                {
                    continue;
                }
                result.push_back(std::move(*actor));
                if (result.size() == maximum)
                {
                    break;
                }
            }
            return result;
        }
        std::size_t skipped = 0u;
        for (const auto& page : data_->source.descriptor_pages)
        {
            if (skipped + page.actor_count <= offset)
            {
                skipped += page.actor_count;
                continue;
            }
            const auto reference = data_->page_objects.find(idKey(page.id));
            if (reference == data_->page_objects.end())
            {
                continue;
            }
            const auto loaded = data_->loadObject(
                EObjectKind::PAGE, 0u, page.id, reference->second);
            if (!loaded)
            {
                continue;
            }
            for (const auto& actor : **loaded)
            {
                if (skipped++ < offset)
                {
                    continue;
                }
                result.push_back(actor);
                if (result.size() == maximum)
                {
                    return result;
                }
            }
        }
        return result;
    }

    std::size_t WorldDescriptorIndex::actorCount() const noexcept
    {
        return data_ ? data_->actor_count : 0u;
    }

    std::size_t WorldDescriptorIndex::pageCount() const noexcept
    {
        return data_ ? data_->source.descriptor_pages.size() : 0u;
    }

    lux::authoring::WorldId WorldDescriptorIndex::world() const noexcept
    {
        return data_ ? data_->world : lux::authoring::WorldId{};
    }

    lux::cxx::algorithm::Sha256Digest
    WorldDescriptorIndex::sourceDigest() const noexcept
    {
        return data_ ? data_->source_digest
                     : lux::cxx::algorithm::Sha256Digest{};
    }

    WorldDescriptorIndexStats WorldDescriptorIndex::stats() const noexcept
    {
        if (!data_)
        {
            return {};
        }
        return {
            data_->actor_count,
            data_->source.descriptor_pages.size(),
            data_->object_cache.size(),
            data_->search_cache.size(),
            data_->object_cache_bytes + data_->search_cache_bytes,
            kObjectCacheBytes};
    }

    std::filesystem::path worldDescriptorIndexCachePath(
        const std::filesystem::path& cache_root,
        lux::authoring::WorldId world)
    {
        return cache_root / "descriptor-index" /
            (uuids::to_string(world.value()) + ".lxdi");
    }
} // namespace lux::authoring
