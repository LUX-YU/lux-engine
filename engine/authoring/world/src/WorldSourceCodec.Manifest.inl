    WorldSourceDocument makeWorldSourceDocument(
        lux::authoring::EPartitionTopology topology)
    {
        WorldSourceDocument root;
        root.world = lux::authoring::WorldId{randomUuid()};
        lux::authoring::PartitionSpaceDescriptor space;
        space.id = lux::authoring::PartitionSpaceId{randomUuid()};
        space.topology = topology;
        space.cell_edge = 128.0f;
        space.macro_edge_cells = 32u;
        root.spaces.push_back(space);
        return root;
    }

    std::string makeWorldActorDocumentPath(
        lux::authoring::WorldActorId actor,
        const lux::cxx::algorithm::Sha256Digest& content_digest)
    {
        const auto identity = uuids::to_string(actor.value());
        return "Actors/" + identity.substr(0u, 2u) + "/" + identity
            + "/" + lux::cxx::algorithm::toHex(content_digest) + ".lxad";
    }

    uuids::uuid makeWorldDescriptorPageId(
        lux::authoring::WorldId world,
        lux::authoring::PartitionSpaceId space,
        const lux::authoring::WorldMacroCoord& macro)
    {
        if (world.empty() || space.empty() || !macro.valid())
            return {};
        std::ostringstream name;
        name << "descriptor|" << uuids::to_string(space.value()) << '|'
             << static_cast<unsigned>(macro.topology) << '|';
        if (const auto* planar = std::get_if<lux::authoring::PlanarMacroCoord>(
                &macro.coordinate))
            name << planar->a << '|' << planar->b;
        else
        {
            const auto& volume = std::get<lux::authoring::VolumeMacroCoord>(
                macro.coordinate);
            name << volume.x << '|' << volume.y << '|' << volume.z;
        }
        return uuids::uuid_name_generator{world.value()}(name.str());
    }

    std::string makeWorldDescriptorPagePath(
        const uuids::uuid& page,
        const lux::cxx::algorithm::Sha256Digest& content_digest)
    {
        const auto identity = uuids::to_string(page);
        return "Descriptors/" + identity.substr(0u, 2u) + "/" + identity
            + "/" + lux::cxx::algorithm::toHex(content_digest) + ".lxai";
    }

    std::string makeWorldInstancePagePath(
        lux::authoring::InstanceSetId instance_set,
        const lux::authoring::WorldCellKey& cell,
        const lux::cxx::algorithm::Sha256Digest& content_digest)
    {
        if (instance_set.empty() || !cell.valid() || content_digest ==
            lux::cxx::algorithm::Sha256Digest{})
            return {};
        const auto identity = uuids::to_string(instance_set.value());
        std::ostringstream coordinate;
        coordinate << static_cast<unsigned>(cell.topology) << '-';
        if (const auto* planar = std::get_if<
                lux::authoring::PlanarCellCoord>(&cell.coordinate))
        {
            coordinate << planar->a << '-' << planar->b;
        }
        else
        {
            const auto& volume = std::get<lux::authoring::VolumeCellCoord>(
                cell.coordinate);
            coordinate << volume.x << '-' << volume.y << '-' << volume.z;
        }
        return "Instances/" + identity.substr(0u, 2u) + "/" + identity
            + "/" + coordinate.str() + "/" +
            lux::cxx::algorithm::toHex(content_digest)
            + ".lxip";
    }

    namespace
    {
        template <class Id>
        std::string makeDomainPagePath(
            std::string_view directory,
            std::string_view extension,
            Id id,
            const lux::authoring::WorldCellKey& cell,
            const lux::cxx::algorithm::Sha256Digest& digest)
        {
            if (id.empty() || !cell.valid() ||
                digest == lux::cxx::algorithm::Sha256Digest{})
                return {};
            const auto identity = uuids::to_string(id.value());
            std::ostringstream coordinate;
            coordinate << static_cast<unsigned>(cell.topology) << '-';
            if (const auto* planar = std::get_if<
                    lux::authoring::PlanarCellCoord>(&cell.coordinate))
            {
                coordinate << planar->a << '-' << planar->b;
            }
            else
            {
                const auto& volume = std::get<lux::authoring::VolumeCellCoord>(
                    cell.coordinate);
                coordinate << volume.x << '-' << volume.y << '-' << volume.z;
            }
            return std::string{directory} + "/" + identity.substr(0u, 2u)
                + "/" + identity + "/" + coordinate.str() + "/"
                + lux::cxx::algorithm::toHex(digest) +
                std::string{extension};
        }

        [[nodiscard]] bool pageSpaceMatches(
            const WorldSourceDocument& root,
            lux::authoring::PartitionSpaceId space_id,
            const lux::authoring::WorldCellKey& cell,
            lux::authoring::EPartitionTopology required_topology)
        {
            const auto* space = findSpace(root, space_id);
            return space && space->topology == required_topology
                && cell.valid() && cell.topology == required_topology;
        }
    } // namespace

    std::string makeWorldTerrainPagePath(
        lux::authoring::TerrainSetId terrain,
        const lux::authoring::WorldCellKey& cell,
        const lux::cxx::algorithm::Sha256Digest& digest)
    {
        return makeDomainPagePath(
            "Terrain", ".lxtp", terrain, cell, digest);
    }

    std::string makeWorldTilePagePath(
        lux::authoring::TilemapId tilemap,
        const lux::authoring::WorldCellKey& cell,
        const lux::cxx::algorithm::Sha256Digest& digest)
    {
        return makeDomainPagePath(
            "Tiles", ".lxtl", tilemap, cell, digest);
    }

    std::string makeWorldPixelPagePath(
        lux::authoring::PixelFieldId field,
        const lux::authoring::WorldCellKey& cell,
        const lux::cxx::algorithm::Sha256Digest& digest)
    {
        return makeDomainPagePath(
            "Pixels", ".lxpp", field, cell, digest);
    }

    lux::cxx::expected<std::vector<std::byte>, std::string>
    encodeWorldSource(const WorldSourceDocument& source) noexcept
    {
        auto root = source;
        canonicalizeRoot(root);
        const WorldSourceCodecLimits limits;
        if (auto valid = validateRoot(root, limits); !valid)
            return lux::cxx::unexpected(std::move(valid.error()));
        std::vector<std::byte> bytes;
        ArchiveWriter writer{bytes};
        writer.writePod(kWorldSourceMagic);
        writer.writePod(kWorldSourceVersion);
        writeId(writer, root.world);
        writer.writePod(static_cast<std::uint32_t>(root.spaces.size()));
        for (const auto& space : root.spaces)
        {
            writeId(writer, space.id);
            writer.writePod(static_cast<std::uint8_t>(space.topology));
            writer.writePod(space.cell_edge);
            writer.writePod(space.macro_edge_cells);
        }
        writer.writePod(static_cast<std::uint32_t>(root.data_layers.size()));
        for (const auto& layer : root.data_layers)
            writeLayer(writer, layer);
        writer.writePod(static_cast<std::uint32_t>(root.required_extensions.size()));
        for (const auto& extension : root.required_extensions)
        {
            writer.writePod(extension.id.hash());
            writer.writeString(extension.id.name());
            writer.writePod(extension.required_major);
            writer.writePod(extension.minimum_minor);
        }
        writer.writePod(static_cast<std::uint32_t>(root.instance_sets.size()));
        for (const auto& instance_set : root.instance_sets)
        {
            writeId(writer, instance_set.id);
            writer.writePod(instance_set.next_local_id);
        }
        writer.writePod(static_cast<std::uint32_t>(root.descriptor_pages.size()));
        for (const auto& reference : root.descriptor_pages)
        {
            writer.writeUuid(reference.id);
            writeId(writer, reference.space);
            writeMacro(writer, reference.macro);
            writer.writeString(reference.document_path);
            writer.writeBytes(
                reference.content_digest.data(),
                reference.content_digest.size());
            writer.writePod(reference.actor_count);
            writer.writePod(reference.page_count);
        }
        if (bytes.size() > limits.maximum_bytes)
            return lux::cxx::unexpected(
                std::string{"World source exceeds byte limit"});
        return bytes;
    }

    lux::cxx::expected<WorldSourceDocument, WorldSourceCodecFailure>
    decodeWorldSource(
        std::span<const std::byte> bytes,
        const WorldSourceCodecLimits& limits) noexcept
    {
        if (bytes.size() > limits.maximum_bytes)
            return lux::cxx::unexpected(WorldSourceCodecFailure{
                EWorldSourceCodecError::LIMIT_EXCEEDED,
                "World source exceeds byte limit"});
        ArchiveReader reader{bytes.data(), bytes.size()};
        if (reader.readPod<std::uint32_t>() != kWorldSourceMagic)
            return lux::cxx::unexpected(WorldSourceCodecFailure{
                EWorldSourceCodecError::INVALID_DATA,
                "invalid LXWA magic"});
        if (reader.readPod<std::uint32_t>() != kWorldSourceVersion)
            return lux::cxx::unexpected(WorldSourceCodecFailure{
                EWorldSourceCodecError::UNSUPPORTED_VERSION,
                "unsupported LXWA version"});
        WorldSourceDocument root;
        root.world = readId<lux::authoring::WorldId>(reader);
        std::uint32_t count = 0u;
        if (!readCount(reader, limits.maximum_spaces, count))
            return lux::cxx::unexpected(WorldSourceCodecFailure{
                EWorldSourceCodecError::LIMIT_EXCEEDED,
                "too many Partition Spaces"});
        root.spaces.reserve(count);
        for (std::uint32_t i = 0u; i < count; ++i)
        {
            lux::authoring::PartitionSpaceDescriptor space;
            space.id = readId<lux::authoring::PartitionSpaceId>(reader);
            const auto topology = reader.readPod<std::uint8_t>();
            space.cell_edge = reader.readPod<float>();
            space.macro_edge_cells = reader.readPod<std::uint16_t>();
            if (topology > static_cast<std::uint8_t>(
                    lux::authoring::EPartitionTopology::VOLUMETRIC_XYZ))
                return lux::cxx::unexpected(WorldSourceCodecFailure{
                    EWorldSourceCodecError::INVALID_DATA,
                    "invalid Space topology"});
            space.topology = static_cast<lux::authoring::EPartitionTopology>(topology);
            root.spaces.push_back(space);
        }
        if (!readCount(reader, limits.maximum_data_layers, count))
            return lux::cxx::unexpected(WorldSourceCodecFailure{
                EWorldSourceCodecError::LIMIT_EXCEEDED,
                "too many Data Layers"});
        root.data_layers.reserve(count);
        for (std::uint32_t i = 0u; i < count; ++i)
        {
            lux::authoring::DataLayerId layer;
            if (!readLayer(reader, limits, layer))
                return lux::cxx::unexpected(WorldSourceCodecFailure{
                    EWorldSourceCodecError::INVALID_DATA,
                    "malformed Data Layer"});
            root.data_layers.push_back(std::move(layer));
        }
        if (!readCount(reader, limits.maximum_requirements, count))
            return lux::cxx::unexpected(WorldSourceCodecFailure{
                EWorldSourceCodecError::LIMIT_EXCEEDED,
                "too many Extension requirements"});
        root.required_extensions.reserve(count);
        for (std::uint32_t i = 0u; i < count; ++i)
        {
            const auto hash = reader.readPod<std::uint64_t>();
            std::string name;
            if (!readString(reader, limits.maximum_string_bytes, name))
                return lux::cxx::unexpected(WorldSourceCodecFailure{
                    EWorldSourceCodecError::INVALID_DATA,
                    "malformed Extension id"});
            lux::authoring::WorldRequiredExtension extension;
            extension.id = lux::extensions::ExtensionId{name};
            if (extension.id.hash() != hash)
            {
                return lux::cxx::unexpected(WorldSourceCodecFailure{
                    EWorldSourceCodecError::INVALID_DATA,
                    "Extension id hash mismatch"});
            }
            extension.required_major = reader.readPod<std::uint16_t>();
            extension.minimum_minor = reader.readPod<std::uint16_t>();
            root.required_extensions.push_back(std::move(extension));
        }
        if (!readCount(reader, limits.maximum_instance_sets, count))
            return lux::cxx::unexpected(WorldSourceCodecFailure{
                EWorldSourceCodecError::LIMIT_EXCEEDED,
                "too many Instance Sets"});
        root.instance_sets.reserve(count);
        for (std::uint32_t i = 0u; i < count; ++i)
        {
            root.instance_sets.push_back(WorldInstanceSetSourceDescriptor{
                readId<lux::authoring::InstanceSetId>(reader),
                reader.readPod<std::uint64_t>()});
        }
        if (!readCount(reader, limits.maximum_descriptor_pages, count))
            return lux::cxx::unexpected(WorldSourceCodecFailure{
                EWorldSourceCodecError::LIMIT_EXCEEDED,
                "too many Descriptor Pages"});
        root.descriptor_pages.reserve(count);
        for (std::uint32_t i = 0u; i < count; ++i)
        {
            WorldDescriptorPageReference reference;
            reference.id = reader.readUuid();
            reference.space = readId<lux::authoring::PartitionSpaceId>(reader);
            if (!readMacro(reader, reference.macro)
                || !readString(
                    reader, limits.maximum_string_bytes, reference.document_path))
                return lux::cxx::unexpected(WorldSourceCodecFailure{
                    EWorldSourceCodecError::INVALID_DATA,
                    "malformed Descriptor Page"});
            reader.readBytes(
                reference.content_digest.data(),
                reference.content_digest.size());
            reference.actor_count = reader.readPod<std::uint32_t>();
            reference.page_count = reader.readPod<std::uint32_t>();
            root.descriptor_pages.push_back(std::move(reference));
        }
        if (!reader.ok() || !reader.eof())
            return lux::cxx::unexpected(WorldSourceCodecFailure{
                EWorldSourceCodecError::INVALID_DATA,
                "World source is truncated or has trailing bytes"});
        canonicalizeRoot(root);
        if (auto valid = validateRoot(root, limits); !valid)
            return lux::cxx::unexpected(WorldSourceCodecFailure{
                EWorldSourceCodecError::INVALID_DATA,
                std::move(valid.error())});
        return root;
    }

    lux::cxx::expected<std::vector<std::byte>, std::string>
    encodeWorldDescriptorPage(
        const WorldSourceDocument& root,
        const WorldDescriptorPageDocument& source) noexcept
    {
        auto page = source;
        canonicalizePage(page);
        const WorldSourceCodecLimits limits;
        if (auto valid_root = validateRoot(root, limits); !valid_root)
            return lux::cxx::unexpected(std::move(valid_root.error()));
        if (auto valid = validatePage(root, page, limits); !valid)
            return lux::cxx::unexpected(std::move(valid.error()));
        std::vector<std::byte> bytes;
        ArchiveWriter writer{bytes};
        writer.writePod(kWorldDescriptorPageMagic);
        writer.writePod(kWorldDescriptorPageVersion);
        writeId(writer, page.world);
        writer.writeUuid(page.id);
        writeId(writer, page.space);
        writeMacro(writer, page.macro);
        writer.writePod(static_cast<std::uint32_t>(page.actors.size()));
        for (const auto& actor : page.actors)
            writeActor(writer, actor);
        writer.writePod(static_cast<std::uint32_t>(page.pages.size()));
        for (const auto& content : page.pages)
            writeContentPage(writer, content);
        if (bytes.size() > limits.maximum_descriptor_page_bytes)
            return lux::cxx::unexpected(
                std::string{"Descriptor Page exceeds byte limit"});
        return bytes;
    }

    lux::cxx::expected<WorldDescriptorPageDocument, std::string>
    decodeWorldDescriptorPage(
        const WorldSourceDocument& root,
        std::span<const std::byte> bytes,
        const WorldSourceCodecLimits& limits) noexcept
    {
        if (bytes.size() > limits.maximum_descriptor_page_bytes)
            return lux::cxx::unexpected(
                std::string{"Descriptor Page exceeds byte limit"});
        ArchiveReader reader{bytes.data(), bytes.size()};
        if (reader.readPod<std::uint32_t>() != kWorldDescriptorPageMagic
            || reader.readPod<std::uint32_t>() != kWorldDescriptorPageVersion)
            return lux::cxx::unexpected(
                std::string{"invalid LXAI v2 header"});
        WorldDescriptorPageDocument page;
        page.world = readId<lux::authoring::WorldId>(reader);
        page.id = reader.readUuid();
        page.space = readId<lux::authoring::PartitionSpaceId>(reader);
        if (!readMacro(reader, page.macro))
            return lux::cxx::unexpected(std::string{"invalid LXAI macro"});
        std::uint32_t count = 0u;
        if (!readCount(reader, limits.maximum_actors, count))
            return lux::cxx::unexpected(std::string{"too many LXAI Actors"});
        page.actors.reserve(count);
        for (std::uint32_t i = 0u; i < count; ++i)
        {
            WorldActorSourceDescriptor actor;
            if (!readActor(reader, limits, actor))
                return lux::cxx::unexpected(std::string{"malformed LXAI Actor"});
            page.actors.push_back(std::move(actor));
        }
        if (!readCount(reader, limits.maximum_pages, count))
            return lux::cxx::unexpected(std::string{"too many LXAI content Pages"});
        page.pages.reserve(count);
        for (std::uint32_t i = 0u; i < count; ++i)
        {
            WorldPageSourceDescriptor content;
            if (!readContentPage(reader, limits, content))
                return lux::cxx::unexpected(std::string{"malformed LXAI content Page"});
            page.pages.push_back(std::move(content));
        }
        if (!reader.ok() || !reader.eof())
            return lux::cxx::unexpected(
                std::string{"LXAI is truncated or has trailing bytes"});
        canonicalizePage(page);
        if (auto valid = validatePage(root, page, limits); !valid)
            return lux::cxx::unexpected(std::move(valid.error()));
        return page;
    }
