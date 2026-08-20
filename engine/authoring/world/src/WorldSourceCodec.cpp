#include <lux/engine/authoring/world/WorldSourceCodec.hpp>

#include <lux/engine/core/serialization/Archive.hpp>
#include <lux/engine/core/serialization/NameTable.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <functional>
#include <limits>
#include <ranges>
#include <random>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef OPTIONAL
#undef OPTIONAL
#endif
#endif

namespace lux::authoring
{
    namespace
    {
        using lux::serialize::ArchiveReader;
        using lux::serialize::ArchiveWriter;

        template <class Id>
        std::string idKey(const Id& id)
        {
            return uuids::to_string(id.value());
        }

        bool uuidLess(const uuids::uuid& left, const uuids::uuid& right)
        {
            return std::ranges::lexicographical_compare(
                left.as_bytes(), right.as_bytes());
        }

        uuids::uuid randomUuid()
        {
            std::random_device device;
            std::array<int, std::mt19937::state_size> seed{};
            std::ranges::generate(seed, std::ref(device));
            std::seed_seq sequence(seed.begin(), seed.end());
            std::mt19937 generator(sequence);
            return uuids::uuid_random_generator{generator}();
        }

        bool validPath(std::string_view path)
        {
            if (path.empty() || path.front() == '/' || path.front() == '\\'
                || path.find('\\') != std::string_view::npos
                || path.find(':') != std::string_view::npos)
            {
                return false;
            }
            std::size_t begin = 0u;
            while (begin <= path.size())
            {
                const auto end = path.find('/', begin);
                const auto part = path.substr(
                    begin,
                    end == std::string_view::npos
                        ? path.size() - begin
                        : end - begin);
                if (part.empty() || part == "." || part == "..")
                    return false;
                if (end == std::string_view::npos)
                    break;
                begin = end + 1u;
            }
            return true;
        }

        bool confinedTo(
            const std::filesystem::path& root,
            const std::filesystem::path& candidate)
        {
            const auto relative = candidate.lexically_relative(root);
            if (relative.empty() || relative.is_absolute())
                return false;
            for (const auto& part : relative)
            {
                if (part == "..")
                    return false;
            }
            return true;
        }

        // `weakly_canonical` may probe every ancestor from the volume root
        // when `input` does not exist. Resolve only the nearest existing
        // prefix so child-first LXWA commits also work in confined roots.
        std::filesystem::path canonicalizeExistingPrefix(
            const std::filesystem::path& input,
            std::error_code& error)
        {
            error.clear();
            auto existing = input.empty()
                ? std::filesystem::path{"."}
                : input.lexically_normal();
            std::filesystem::path unresolved;
            while (!existing.empty())
            {
                const auto status = std::filesystem::symlink_status(
                    existing,
                    error);
                if (error &&
                    status.type() != std::filesystem::file_type::not_found)
                    return {};
                error.clear();
                if (std::filesystem::exists(status))
                {
                    auto canonical = std::filesystem::canonical(
                        existing,
                        error);
                    if (error)
                        return {};
                    // Appending an empty path adds a trailing directory
                    // separator on MSVC. That turns an already-existing
                    // regular file into `file.lxad\\` and makes the
                    // subsequent open fail. Only append the suffix when we
                    // actually walked past a missing child.
                    return unresolved.empty()
                        ? canonical.lexically_normal()
                        : (canonical / unresolved).lexically_normal();
                }

                const auto filename = existing.filename();
                if (!filename.empty())
                {
                    unresolved = unresolved.empty()
                        ? filename
                        : filename / unresolved;
                }
                const auto parent = existing.parent_path();
                if (parent == existing)
                    break;
                existing = parent;
            }
            error = std::make_error_code(
                std::errc::no_such_file_or_directory);
            return {};
        }

        lux::cxx::expected<void, std::string> atomicWrite(
            const std::filesystem::path& path,
            std::span<const std::byte> bytes)
        {
            std::error_code error;
            if (const auto parent = path.parent_path(); !parent.empty())
                std::filesystem::create_directories(parent, error);
            if (error)
            {
                return lux::cxx::unexpected(
                    std::string{"cannot create World source directory: "}
                    + error.message());
            }
            auto temporary = path;
            temporary += ".tmp";
            {
                std::ofstream stream(
                    temporary, std::ios::binary | std::ios::trunc);
                if (!stream
                    || !stream.write(
                        reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size())))
                {
                    return lux::cxx::unexpected(
                        std::string{"cannot write temporary World source"});
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
                    std::string{"cannot atomically replace World source: "}
                    + error.message());
            }
            return {};
        }

        lux::cxx::expected<std::vector<std::byte>, std::string> readFile(
            const std::filesystem::path& path,
            std::uint64_t maximum_bytes)
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream)
                return lux::cxx::unexpected(
                    std::string{"cannot open World source document"});
            const auto end = stream.tellg();
            if (end < 0 || static_cast<std::uint64_t>(end) > maximum_bytes)
            {
                return lux::cxx::unexpected(
                    std::string{"World source document exceeds byte limit"});
            }
            std::vector<std::byte> bytes(static_cast<std::size_t>(end));
            stream.seekg(0, std::ios::beg);
            if (!bytes.empty()
                && !stream.read(
                    reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size())))
            {
                return lux::cxx::unexpected(
                    std::string{"cannot read World source document"});
            }
            return bytes;
        }

        template <class Id>
        void writeId(ArchiveWriter& writer, const Id& id)
        {
            writer.writeUuid(id.value());
        }

        template <class Id>
        Id readId(ArchiveReader& reader)
        {
            return Id{reader.readUuid()};
        }

        bool readCount(
            ArchiveReader& reader,
            std::uint32_t maximum,
            std::uint32_t& value)
        {
            value = reader.readPod<std::uint32_t>();
            return reader.ok() && value <= maximum;
        }

        bool readString(
            ArchiveReader& reader,
            std::uint32_t maximum,
            std::string& value)
        {
            const auto size = reader.readPod<std::uint32_t>();
            if (!reader.ok() || size > maximum || size > reader.remaining())
                return false;
            value.resize(size);
            reader.readBytes(value.data(), value.size());
            return reader.ok();
        }

        void writeLayer(
            ArchiveWriter& writer,
            const lux::authoring::DataLayerId& id)
        {
            writer.writePod(id.hash());
            writer.writeString(id.name());
        }

        bool readLayer(
            ArchiveReader& reader,
            const WorldSourceCodecLimits& limits,
            lux::authoring::DataLayerId& id)
        {
            const auto hash = reader.readPod<std::uint64_t>();
            std::string name;
            if (!readString(reader, limits.maximum_string_bytes, name))
                return false;
            id = lux::authoring::DataLayerId{hash, std::move(name)};
            return reader.ok();
        }

        void writeMacro(
            ArchiveWriter& writer,
            const lux::authoring::WorldMacroCoord& macro)
        {
            writer.writePod(static_cast<std::uint8_t>(macro.topology));
            if (const auto* planar = std::get_if<
                    lux::authoring::PlanarMacroCoord>(&macro.coordinate))
            {
                writer.writePod(planar->a);
                writer.writePod(planar->b);
            }
            else
            {
                const auto& volume = std::get<
                    lux::authoring::VolumeMacroCoord>(macro.coordinate);
                writer.writePod(volume.x);
                writer.writePod(volume.y);
                writer.writePod(volume.z);
            }
        }

        bool readMacro(
            ArchiveReader& reader,
            lux::authoring::WorldMacroCoord& macro)
        {
            const auto topology = reader.readPod<std::uint8_t>();
            if (topology > static_cast<std::uint8_t>(
                    lux::authoring::EPartitionTopology::VOLUMETRIC_XYZ))
                return false;
            macro.topology = static_cast<lux::authoring::EPartitionTopology>(
                topology);
            if (macro.topology ==
                lux::authoring::EPartitionTopology::VOLUMETRIC_XYZ)
            {
                macro.coordinate = lux::authoring::VolumeMacroCoord{
                    reader.readPod<std::int64_t>(),
                    reader.readPod<std::int64_t>(),
                    reader.readPod<std::int64_t>()};
            }
            else
            {
                macro.coordinate = lux::authoring::PlanarMacroCoord{
                    reader.readPod<std::int64_t>(),
                    reader.readPod<std::int64_t>()};
            }
            return reader.ok() && macro.valid();
        }

        void writeCell(
            ArchiveWriter& writer,
            const lux::authoring::WorldCellKey& cell)
        {
            writer.writePod(static_cast<std::uint8_t>(cell.topology));
            if (const auto* planar = std::get_if<
                    lux::authoring::PlanarCellCoord>(&cell.coordinate))
            {
                writer.writePod(planar->a);
                writer.writePod(planar->b);
            }
            else
            {
                const auto& volume = std::get<
                    lux::authoring::VolumeCellCoord>(cell.coordinate);
                writer.writePod(volume.x);
                writer.writePod(volume.y);
                writer.writePod(volume.z);
            }
        }

        bool readCell(
            ArchiveReader& reader,
            lux::authoring::WorldCellKey& cell)
        {
            const auto topology = reader.readPod<std::uint8_t>();
            if (topology > static_cast<std::uint8_t>(
                    lux::authoring::EPartitionTopology::VOLUMETRIC_XYZ))
                return false;
            cell.topology = static_cast<lux::authoring::EPartitionTopology>(
                topology);
            if (cell.topology ==
                lux::authoring::EPartitionTopology::VOLUMETRIC_XYZ)
            {
                cell.coordinate = lux::authoring::VolumeCellCoord{
                    reader.readPod<std::int64_t>(),
                    reader.readPod<std::int64_t>(),
                    reader.readPod<std::int64_t>()};
            }
            else
            {
                cell.coordinate = lux::authoring::PlanarCellCoord{
                    reader.readPod<std::int64_t>(),
                    reader.readPod<std::int64_t>()};
            }
            return reader.ok() && cell.valid();
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
            }
            else
            {
                const auto& volume = std::get<
                    lux::math::Position3d>(position);
                writer.writePod(std::uint8_t{1u});
                writer.writePod(volume.x);
                writer.writePod(volume.y);
                writer.writePod(volume.z);
            }
        }

        bool readPosition(
            ArchiveReader& reader,
            WorldActorSourcePosition& position)
        {
            const auto kind = reader.readPod<std::uint8_t>();
            if (kind == 0u)
            {
                const lux::math::Position2d value{
                    reader.readPod<double>(), reader.readPod<double>()};
                if (!reader.ok() || !lux::math::isFinite(value))
                    return false;
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
                    return false;
                position = value;
                return true;
            }
            return false;
        }

        void writeActor(
            ArchiveWriter& writer,
            const WorldActorSourceDescriptor& actor)
        {
            writeId(writer, actor.id);
            writer.writeString(actor.display_name);
            writer.writeString(actor.actor_class);
            writer.writeString(actor.document_path);
            writer.writeBytes(
                actor.content_digest.data(),
                actor.content_digest.size());
            writeId(writer, actor.space);
            writePosition(writer, actor.position);
            writer.writePod(static_cast<std::uint8_t>(
                actor.transform_parent.has_value()));
            if (actor.transform_parent)
                writeId(writer, *actor.transform_parent);
            for (const auto value : actor.bounds_half_extent)
                writer.writePod(value);
            writer.writePod(static_cast<std::uint32_t>(actor.data_layers.size()));
            for (const auto& layer : actor.data_layers)
                writeLayer(writer, layer);
            writer.writePod(static_cast<std::uint32_t>(actor.references.size()));
            for (const auto& reference : actor.references)
            {
                writeId(writer, reference.target);
                writer.writePod(static_cast<std::uint8_t>(reference.kind));
            }
        }

        bool readActor(
            ArchiveReader& reader,
            const WorldSourceCodecLimits& limits,
            WorldActorSourceDescriptor& actor)
        {
            actor.id = readId<lux::authoring::WorldActorId>(reader);
            if (!readString(reader, limits.maximum_string_bytes, actor.display_name)
                || !readString(reader, limits.maximum_string_bytes, actor.actor_class)
                || !readString(reader, limits.maximum_string_bytes, actor.document_path))
                return false;
            reader.readBytes(
                actor.content_digest.data(),
                actor.content_digest.size());
            actor.space = readId<lux::authoring::PartitionSpaceId>(reader);
            if (!readPosition(reader, actor.position))
                return false;
            const auto has_parent = reader.readPod<std::uint8_t>();
            if (has_parent > 1u)
                return false;
            if (has_parent != 0u)
                actor.transform_parent =
                    readId<lux::authoring::WorldActorId>(reader);
            actor.bounds_half_extent = {
                reader.readPod<float>(),
                reader.readPod<float>(),
                reader.readPod<float>()};
            std::uint32_t count = 0u;
            if (!readCount(reader, limits.maximum_data_layers, count))
                return false;
            actor.data_layers.reserve(count);
            for (std::uint32_t i = 0u; i < count; ++i)
            {
                lux::authoring::DataLayerId layer;
                if (!readLayer(reader, limits, layer))
                    return false;
                actor.data_layers.push_back(std::move(layer));
            }
            if (!readCount(reader, limits.maximum_actor_references, count))
                return false;
            actor.references.reserve(count);
            for (std::uint32_t i = 0u; i < count; ++i)
            {
                WorldActorSourceReference reference;
                reference.target = readId<
                    lux::authoring::WorldActorId>(reader);
                const auto kind = reader.readPod<std::uint8_t>();
                if (kind > static_cast<std::uint8_t>(
                        EWorldActorReferenceKind::OPTIONAL_REFERENCE))
                    return false;
                reference.kind = static_cast<EWorldActorReferenceKind>(kind);
                actor.references.push_back(reference);
            }
            return reader.ok();
        }

        void writeContentPage(
            ArchiveWriter& writer,
            const WorldPageSourceDescriptor& page)
        {
            writer.writeUuid(page.id);
            writer.writePod(static_cast<std::uint8_t>(page.kind));
            std::visit(
                [&](const auto& owner)
                {
                    writeId(writer, owner);
                },
                page.owner);
            writer.writeString(page.document_path);
            writeId(writer, page.space);
            writeCell(writer, page.cell);
            writer.writeBytes(
                page.content_digest.data(),
                page.content_digest.size());
        }

        bool readContentPage(
            ArchiveReader& reader,
            const WorldSourceCodecLimits& limits,
            WorldPageSourceDescriptor& page)
        {
            page.id = reader.readUuid();
            const auto kind = reader.readPod<std::uint8_t>();
            if (kind > static_cast<std::uint8_t>(EWorldPageSourceKind::PIXEL))
                return false;
            page.kind = static_cast<EWorldPageSourceKind>(kind);
            switch (page.kind)
            {
            case EWorldPageSourceKind::INSTANCE:
                page.owner = readId<lux::authoring::InstanceSetId>(reader);
                break;
            case EWorldPageSourceKind::TERRAIN:
                page.owner = readId<lux::authoring::TerrainSetId>(reader);
                break;
            case EWorldPageSourceKind::TILE:
                page.owner = readId<lux::authoring::TilemapId>(reader);
                break;
            case EWorldPageSourceKind::PIXEL:
                page.owner = readId<lux::authoring::PixelFieldId>(reader);
                break;
            }
            if (!readString(
                    reader, limits.maximum_string_bytes, page.document_path))
                return false;
            page.space = readId<lux::authoring::PartitionSpaceId>(reader);
            if (!readCell(reader, page.cell))
                return false;
            reader.readBytes(
                page.content_digest.data(),
                page.content_digest.size());
            return reader.ok();
        }

        const lux::authoring::PartitionSpaceDescriptor* findSpace(
            const WorldSourceDocument& root,
            lux::authoring::PartitionSpaceId id)
        {
            const auto found = std::ranges::find(root.spaces, id, [](const auto& space)
            {
                return space.id;
            });
            return found == root.spaces.end() ? nullptr : &*found;
        }

        const WorldInstanceSetSourceDescriptor* findInstanceSet(
            const WorldSourceDocument& root,
            lux::authoring::InstanceSetId id)
        {
            const auto found = std::ranges::find(
                root.instance_sets,
                id,
                &WorldInstanceSetSourceDescriptor::id);
            return found == root.instance_sets.end() ? nullptr : &*found;
        }

        std::optional<lux::authoring::WorldCellKey> positionCell(
            const lux::authoring::PartitionSpaceDescriptor& space,
            const WorldActorSourcePosition& position)
        {
            const auto cellCoordinate = [&](double value)
                -> std::optional<std::int64_t>
            {
                if (!std::isfinite(value) ||
                    !lux::authoring::isValidCellEdge(space.cell_edge))
                {
                    return std::nullopt;
                }
                const double coordinate = std::floor(
                    value / static_cast<double>(space.cell_edge));
                if (coordinate < static_cast<double>(
                        std::numeric_limits<std::int64_t>::min()) ||
                    coordinate > static_cast<double>(
                        std::numeric_limits<std::int64_t>::max()))
                {
                    return std::nullopt;
                }
                return static_cast<std::int64_t>(coordinate);
            };
            lux::authoring::WorldCellKey cell;
            cell.topology = space.topology;
            if (space.topology == lux::authoring::EPartitionTopology::PLANAR_XY)
            {
                const auto* point = std::get_if<lux::math::Position2d>(
                    &position);
                if (!point)
                    return std::nullopt;
                const auto x = cellCoordinate(point->x);
                const auto y = cellCoordinate(point->y);
                if (!x || !y)
                    return std::nullopt;
                cell.coordinate = lux::authoring::PlanarCellCoord{*x, *y};
            }
            else
            {
                const auto* point = std::get_if<lux::math::Position3d>(
                    &position);
                if (!point)
                    return std::nullopt;
                if (space.topology == lux::authoring::EPartitionTopology::PLANAR_XZ)
                {
                    const auto x = cellCoordinate(point->x);
                    const auto z = cellCoordinate(point->z);
                    if (!x || !z)
                        return std::nullopt;
                    cell.coordinate = lux::authoring::PlanarCellCoord{*x, *z};
                }
                else
                {
                    const auto x = cellCoordinate(point->x);
                    const auto y = cellCoordinate(point->y);
                    const auto z = cellCoordinate(point->z);
                    if (!x || !y || !z)
                        return std::nullopt;
                    cell.coordinate = lux::authoring::VolumeCellCoord{
                        *x, *y, *z};
                }
            }
            return cell;
        }

        std::optional<lux::authoring::WorldMacroCoord> actorMacro(
            const lux::authoring::PartitionSpaceDescriptor& space,
            const WorldActorSourcePosition& position)
        {
            const auto cell = positionCell(space, position);
            return cell
                ? lux::authoring::macroCoordOf(*cell, space.macro_edge_cells)
                : std::nullopt;
        }

        void canonicalizeRoot(WorldSourceDocument& root)
        {
            std::ranges::sort(root.contributions, {}, [](const auto& value)
            {
                return value.id.name();
            });
            std::ranges::sort(root.spaces, {}, [](const auto& value)
            {
                return uuids::to_string(value.id.value());
            });
            std::ranges::sort(root.data_layers, {}, [](const auto& value)
            {
                return value.name();
            });
            std::ranges::sort(root.required_extensions, {}, [](const auto& value)
            {
                return value.id.name();
            });
            std::ranges::sort(root.descriptor_pages, [](const auto& a, const auto& b)
            {
                return uuidLess(a.id, b.id);
            });
            std::ranges::sort(root.instance_sets, {}, [](const auto& value)
            {
                return uuids::to_string(value.id.value());
            });
        }

        void canonicalizePage(WorldDescriptorPageDocument& page)
        {
            for (auto& actor : page.actors)
            {
                std::ranges::sort(actor.data_layers, {}, [](const auto& layer)
                {
                    return layer.name();
                });
                std::ranges::sort(actor.references, [](const auto& a, const auto& b)
                {
                    if (a.target != b.target)
                        return uuidLess(a.target.value(), b.target.value());
                    return a.kind < b.kind;
                });
            }
            std::ranges::sort(page.actors, {}, [](const auto& actor)
            {
                return uuids::to_string(actor.id.value());
            });
            std::ranges::sort(page.pages, [](const auto& a, const auto& b)
            {
                return uuidLess(a.id, b.id);
            });
        }

        lux::cxx::expected<void, std::string> validateRoot(
            const WorldSourceDocument& root,
            const WorldSourceCodecLimits& limits)
        {
            if (root.world.empty()
                || root.spaces.empty()
                || root.spaces.size() > limits.maximum_spaces
                || root.contributions.size() > limits.maximum_contributions
                || root.data_layers.size() > limits.maximum_data_layers
                || root.required_extensions.size() > limits.maximum_requirements
                || root.instance_sets.size() > limits.maximum_instance_sets
                || root.descriptor_pages.size() > limits.maximum_descriptor_pages)
            {
                return lux::cxx::unexpected(
                    std::string{"World source has invalid root bounds"});
            }
            std::unordered_set<std::string> contribution_ids;
            for (const auto& contribution : root.contributions)
            {
                if (!contribution.id.valid() ||
                    contribution.id.name().size() >
                        limits.maximum_string_bytes ||
                    contribution.config.size() > limits.maximum_bytes ||
                    !contribution_ids.insert(
                        std::string{contribution.id.name()}).second)
                {
                    return lux::cxx::unexpected(std::string{
                        "World source has invalid scene contributions"});
                }
            }
            std::unordered_map<std::string, const lux::authoring::PartitionSpaceDescriptor*> spaces;
            for (const auto& space : root.spaces)
            {
                if (space.id.empty() ||
                    !lux::authoring::isValidCellEdge(space.cell_edge) ||
                    space.macro_edge_cells == 0u
                    || !spaces.emplace(idKey(space.id), &space).second)
                {
                    return lux::cxx::unexpected(
                    std::string{"World source has invalid Partition Spaces"});
                }
            }
            std::unordered_set<std::string> layers;
            for (const auto& layer : root.data_layers)
            {
                if (!layer.valid()
                    || layer.name().size() > limits.maximum_string_bytes
                    || !layers.insert(std::string{layer.name()}).second)
                {
                    return lux::cxx::unexpected(
                        std::string{"World source has invalid Data Layers"});
                }
            }
            std::unordered_set<std::string> extensions;
            for (const auto& extension : root.required_extensions)
            {
                if (!extension.id.valid() ||
                    extension.id.name().size() > limits.maximum_string_bytes ||
                    !extensions.insert(
                        std::string{extension.id.name()}).second)
                {
                    return lux::cxx::unexpected(
                        std::string{"World source has invalid Extension requirements"});
                }
            }
            std::unordered_set<std::string> instance_sets;
            for (const auto& instance_set : root.instance_sets)
            {
                if (instance_set.id.empty() ||
                    instance_set.next_local_id == 0u ||
                    !instance_sets.insert(idKey(instance_set.id)).second)
                {
                    return lux::cxx::unexpected(std::string{
                        "World source has invalid Instance Set allocators"});
                }
            }
            std::unordered_set<std::string> ids;
            std::unordered_set<std::string> paths;
            for (const auto& reference : root.descriptor_pages)
            {
                const auto space = spaces.find(idKey(reference.space));
                if (reference.id.is_nil()
                    || space == spaces.end()
                    || !reference.macro.valid()
                    || reference.macro.topology != space->second->topology
                    || reference.id != makeWorldDescriptorPageId(
                        root.world, reference.space, reference.macro)
                    || reference.document_path.size() > limits.maximum_string_bytes
                    || !validPath(reference.document_path)
                    || reference.content_digest ==
                        lux::cxx::algorithm::Sha256Digest{}
                    || reference.actor_count > limits.maximum_actors
                    || reference.page_count > limits.maximum_pages
                    || !ids.insert(uuids::to_string(reference.id)).second
                    || !paths.insert(reference.document_path).second)
                {
                    return lux::cxx::unexpected(
                        std::string{"World source has invalid Descriptor Page references"});
                }
            }
            return {};
        }

        lux::cxx::expected<void, std::string> validatePage(
            const WorldSourceDocument& root,
            const WorldDescriptorPageDocument& page,
            const WorldSourceCodecLimits& limits)
        {
            const auto* space = findSpace(root, page.space);
            if (page.world != root.world || page.id.is_nil() || !space
                || page.macro.topology != space->topology
                || page.id != makeWorldDescriptorPageId(
                    root.world, page.space, page.macro)
                || page.actors.size() > limits.maximum_actors
                || page.pages.size() > limits.maximum_pages)
            {
                return lux::cxx::unexpected(
                    std::string{"Descriptor Page identity is invalid"});
            }
            std::unordered_set<std::string> known_layers;
            for (const auto& layer : root.data_layers)
                known_layers.insert(std::string{layer.name()});
            std::unordered_set<std::string> ids;
            std::unordered_set<std::string> paths;
            for (const auto& actor : page.actors)
            {
                const auto macro = actorMacro(*space, actor.position);
                if (actor.id.empty() || actor.space != page.space
                    || actor.display_name.empty() || actor.actor_class.empty()
                    || actor.display_name.size() > limits.maximum_string_bytes
                    || actor.actor_class.size() > limits.maximum_string_bytes
                    || actor.document_path.size() > limits.maximum_string_bytes
                    || !validPath(actor.document_path)
                    || actor.content_digest ==
                        lux::cxx::algorithm::Sha256Digest{}
                    || actor.document_path != makeWorldActorDocumentPath(
                        actor.id, actor.content_digest)
                    || actor.references.size() > limits.maximum_actor_references
                    || !macro || *macro != page.macro
                    || !std::isfinite(actor.bounds_half_extent[0])
                    || !std::isfinite(actor.bounds_half_extent[1])
                    || !std::isfinite(actor.bounds_half_extent[2])
                    || actor.bounds_half_extent[0] < 0.0f
                    || actor.bounds_half_extent[1] < 0.0f
                    || actor.bounds_half_extent[2] < 0.0f
                    || !ids.insert(idKey(actor.id)).second
                    || !paths.insert(actor.document_path).second)
                {
                    return lux::cxx::unexpected(
                        std::string{"Descriptor Page has an invalid Actor"});
                }
                std::unordered_set<std::string> memberships;
                for (const auto& layer : actor.data_layers)
                {
                    if (!layer.valid() ||
                        !known_layers.contains(std::string{layer.name()}) ||
                        !memberships.insert(std::string{layer.name()}).second)
                    {
                        return lux::cxx::unexpected(
                            std::string{"Descriptor Actor has invalid Data Layers"});
                    }
                }
                std::unordered_set<std::string> references;
                for (const auto& reference : actor.references)
                {
                    if (reference.target.empty() || reference.target == actor.id
                        || !references.insert(idKey(reference.target)).second)
                    {
                        return lux::cxx::unexpected(
                            std::string{"Descriptor Actor has invalid references"});
                    }
                }
                if (actor.transform_parent &&
                    (actor.transform_parent->empty() ||
                     *actor.transform_parent == actor.id ||
                     !std::ranges::any_of(
                         actor.references,
                         [&](const auto& reference)
                         {
                             return reference.target ==
                                     *actor.transform_parent &&
                                 reference.kind ==
                                     EWorldActorReferenceKind::LOCAL;
                         })))
                {
                    return lux::cxx::unexpected(
                        std::string{"Descriptor Actor has an invalid Transform parent"});
                }
            }
            std::unordered_set<std::string> content_ids;
            for (const auto& content : page.pages)
            {
                const auto valid_owner = [&]()
                {
                    switch (content.kind)
                    {
                    case EWorldPageSourceKind::INSTANCE:
                        if (const auto* owner = std::get_if<
                                lux::authoring::InstanceSetId>(&content.owner))
                            return !owner->empty() &&
                                findInstanceSet(root, *owner) != nullptr;
                        break;
                    case EWorldPageSourceKind::TERRAIN:
                        if (const auto* owner = std::get_if<
                                lux::authoring::TerrainSetId>(&content.owner))
                            return !owner->empty();
                        break;
                    case EWorldPageSourceKind::TILE:
                        if (const auto* owner = std::get_if<
                                lux::authoring::TilemapId>(&content.owner))
                            return !owner->empty();
                        break;
                    case EWorldPageSourceKind::PIXEL:
                        if (const auto* owner = std::get_if<
                                lux::authoring::PixelFieldId>(&content.owner))
                            return !owner->empty();
                        break;
                    }
                    return false;
                }();
                const auto macro = lux::authoring::macroCoordOf(
                    content.cell, space->macro_edge_cells);
                if (content.id.is_nil() || !valid_owner ||
                    content.space != page.space
                    || content.cell.topology != space->topology
                    || !macro || *macro != page.macro
                    || content.content_digest ==
                        lux::cxx::algorithm::Sha256Digest{}
                    || content.document_path.size() > limits.maximum_string_bytes
                    || !validPath(content.document_path)
                    || !content_ids.insert(uuids::to_string(content.id)).second
                    || !paths.insert(content.document_path).second)
                {
                    return lux::cxx::unexpected(
                        std::string{"Descriptor Page has an invalid content Page"});
                }
            }
            return {};
        }
    }

#include "WorldSourceCodec.Manifest.inl"

#include "WorldSourceCodec.ActorInstance.inl"

#include "WorldSourceCodec.DomainPages.inl"

#include "WorldSourceCodec.Storage.inl"

} // namespace lux::authoring
