    lux::cxx::expected<std::vector<std::byte>, std::string>
    encodeWorldTerrainPage(
        const WorldSourceDocument& root,
        const WorldTerrainPageDocument& page) noexcept
    {
        constexpr std::size_t kSamples =
            static_cast<std::size_t>(kWorldTerrainSampleEdge)
            * kWorldTerrainSampleEdge;
        constexpr std::size_t kWeightBytes = kSamples * 4u;
        constexpr std::size_t kHoleBytes = (kSamples + 7u) / 8u;
        if (page.world != root.world || page.terrain_set.empty()
            || !pageSpaceMatches(
                root, page.space, page.cell,
                lux::authoring::EPartitionTopology::PLANAR_XZ)
            || !std::isfinite(page.height_min)
            || !std::isfinite(page.height_max)
            || !(page.height_max > page.height_min)
            || !std::isfinite(page.sample_spacing)
            || !(page.sample_spacing > 0.0f)
            || page.heights.size() != kSamples
            || page.weight_layer_count > 8u
            || page.weight_planes[0].size() != kWeightBytes
            || page.weight_planes[1].size() != kWeightBytes
            || page.holes.size() != kHoleBytes
            || std::ranges::any_of(page.heights, [](float value)
                { return !std::isfinite(value); }))
        {
            return lux::cxx::unexpected(
                std::string{"LXTP has invalid dimensions or fields"});
        }
        std::vector<std::byte> bytes;
        ArchiveWriter writer{bytes};
        writer.writePod(kWorldTerrainPageMagic);
        writer.writePod(kWorldTerrainPageVersion);
        writeId(writer, page.world);
        writeId(writer, page.terrain_set);
        writeId(writer, page.space);
        writeCell(writer, page.cell);
        writer.writePod(page.height_min);
        writer.writePod(page.height_max);
        writer.writePod(page.sample_spacing);
        writer.writePod(page.weight_layer_count);
        writer.writeBytes(page.heights.data(), page.heights.size() * sizeof(float));
        writer.writeBytes(
            page.weight_planes[0].data(), page.weight_planes[0].size());
        writer.writeBytes(
            page.weight_planes[1].data(), page.weight_planes[1].size());
        writer.writeBytes(page.holes.data(), page.holes.size());
        if (bytes.size() > WorldSourceCodecLimits{}.maximum_bytes)
            return lux::cxx::unexpected(std::string{"LXTP exceeds byte limit"});
        return bytes;
    }

    lux::cxx::expected<WorldTerrainPageDocument, std::string>
    decodeWorldTerrainPage(
        const WorldSourceDocument& root,
        std::span<const std::byte> bytes,
        const WorldSourceCodecLimits& limits) noexcept
    {
        constexpr std::size_t kSamples =
            static_cast<std::size_t>(kWorldTerrainSampleEdge)
            * kWorldTerrainSampleEdge;
        constexpr std::size_t kWeightBytes = kSamples * 4u;
        constexpr std::size_t kHoleBytes = (kSamples + 7u) / 8u;
        if (bytes.size() > limits.maximum_bytes)
            return lux::cxx::unexpected(std::string{"LXTP exceeds byte limit"});
        ArchiveReader reader{bytes.data(), bytes.size()};
        if (reader.readPod<std::uint32_t>() != kWorldTerrainPageMagic
            || reader.readPod<std::uint32_t>() != kWorldTerrainPageVersion)
            return lux::cxx::unexpected(std::string{"invalid LXTP v1 header"});
        WorldTerrainPageDocument page;
        page.world = readId<lux::authoring::WorldId>(reader);
        page.terrain_set = readId<lux::authoring::TerrainSetId>(reader);
        page.space = readId<lux::authoring::PartitionSpaceId>(reader);
        if (!readCell(reader, page.cell))
            return lux::cxx::unexpected(std::string{"invalid LXTP Cell"});
        page.height_min = reader.readPod<float>();
        page.height_max = reader.readPod<float>();
        page.sample_spacing = reader.readPod<float>();
        page.weight_layer_count = reader.readPod<std::uint8_t>();
        page.heights.resize(kSamples);
        page.weight_planes[0].resize(kWeightBytes);
        page.weight_planes[1].resize(kWeightBytes);
        page.holes.resize(kHoleBytes);
        reader.readBytes(page.heights.data(), page.heights.size() * sizeof(float));
        reader.readBytes(page.weight_planes[0].data(), kWeightBytes);
        reader.readBytes(page.weight_planes[1].data(), kWeightBytes);
        reader.readBytes(page.holes.data(), kHoleBytes);
        if (!reader.ok() || !reader.eof())
            return lux::cxx::unexpected(
                std::string{"LXTP is truncated or has trailing bytes"});
        if (!encodeWorldTerrainPage(root, page))
            return lux::cxx::unexpected(std::string{"invalid LXTP payload"});
        return page;
    }

    lux::cxx::expected<WorldTerrainPageDocument, std::string>
    loadWorldTerrainPage(
        const std::filesystem::path& root_document,
        std::string_view relative_path,
        const WorldSourceDocument& root,
        const WorldSourceCodecLimits& limits) noexcept
    {
        auto resolved = resolveWorldSourceDocument(
            root_document, relative_path);
        if (!resolved)
            return lux::cxx::unexpected(std::move(resolved.error()));
        auto bytes = readFile(*resolved, limits.maximum_bytes);
        if (!bytes)
            return lux::cxx::unexpected(std::move(bytes.error()));
        return decodeWorldTerrainPage(root, *bytes, limits);
    }

    lux::cxx::expected<std::vector<std::byte>, std::string>
    encodeWorldTilePage(
        const WorldSourceDocument& root,
        const WorldTilePageDocument& page) noexcept
    {
        constexpr std::size_t kTiles =
            static_cast<std::size_t>(kWorldLogicalChunkEdge)
            * kWorldLogicalChunkEdge;
        if (page.world != root.world || page.tilemap.empty()
            || !pageSpaceMatches(
                root, page.space, page.cell,
                lux::authoring::EPartitionTopology::PLANAR_XY)
            || page.tileset.is_nil()
            || page.tileset_columns == 0u || page.tileset_rows == 0u
            || page.tileset_columns > 0xffffu
            || page.tileset_rows > 0xffffu
            || static_cast<std::uint64_t>(page.tileset_columns) *
                page.tileset_rows >= 0xffffu
            || !std::isfinite(page.tile_size[0])
            || !std::isfinite(page.tile_size[1])
            || !(page.tile_size[0] > 0.0f) || !(page.tile_size[1] > 0.0f)
            || page.tile_ordinals.size() != kTiles
            || page.collision_boxes.size() > kTiles ||
            std::ranges::any_of(
                page.collision_boxes,
                [](const WorldTileCollisionBox& box)
                {
                    return box.width == 0u || box.height == 0u ||
                        static_cast<std::uint32_t>(box.x) + box.width >
                            kWorldLogicalChunkEdge ||
                        static_cast<std::uint32_t>(box.y) + box.height >
                            kWorldLogicalChunkEdge;
                }))
        {
            return lux::cxx::unexpected(
                std::string{"LXTL has invalid dimensions or fields"});
        }
        std::vector<std::byte> bytes;
        ArchiveWriter writer{bytes};
        writer.writePod(kWorldTilePageMagic);
        writer.writePod(kWorldTilePageVersion);
        writeId(writer, page.world);
        writeId(writer, page.tilemap);
        writeId(writer, page.space);
        writeCell(writer, page.cell);
        writer.writeUuid(page.tileset);
        writer.writePod(page.tileset_columns);
        writer.writePod(page.tileset_rows);
        writer.writePod(page.tile_size[0]);
        writer.writePod(page.tile_size[1]);
        writer.writeBytes(
            page.tile_ordinals.data(),
            page.tile_ordinals.size() * sizeof(std::uint32_t));
        writer.writePod(static_cast<std::uint32_t>(
            page.collision_boxes.size()));
        for (const auto& box : page.collision_boxes)
        {
            writer.writePod(box.x);
            writer.writePod(box.y);
            writer.writePod(box.width);
            writer.writePod(box.height);
        }
        if (bytes.size() > WorldSourceCodecLimits{}.maximum_bytes)
            return lux::cxx::unexpected(std::string{"LXTL exceeds byte limit"});
        return bytes;
    }

    lux::cxx::expected<WorldTilePageDocument, std::string>
    decodeWorldTilePage(
        const WorldSourceDocument& root,
        std::span<const std::byte> bytes,
        const WorldSourceCodecLimits& limits) noexcept
    {
        constexpr std::size_t kTiles =
            static_cast<std::size_t>(kWorldLogicalChunkEdge)
            * kWorldLogicalChunkEdge;
        if (bytes.size() > limits.maximum_bytes)
            return lux::cxx::unexpected(std::string{"LXTL exceeds byte limit"});
        ArchiveReader reader{bytes.data(), bytes.size()};
        if (reader.readPod<std::uint32_t>() != kWorldTilePageMagic
            || reader.readPod<std::uint32_t>() != kWorldTilePageVersion)
            return lux::cxx::unexpected(std::string{"invalid LXTL v1 header"});
        WorldTilePageDocument page;
        page.world = readId<lux::authoring::WorldId>(reader);
        page.tilemap = readId<lux::authoring::TilemapId>(reader);
        page.space = readId<lux::authoring::PartitionSpaceId>(reader);
        if (!readCell(reader, page.cell))
            return lux::cxx::unexpected(std::string{"invalid LXTL Cell"});
        page.tileset = reader.readUuid();
        page.tileset_columns = reader.readPod<std::uint32_t>();
        page.tileset_rows = reader.readPod<std::uint32_t>();
        page.tile_size = {reader.readPod<float>(), reader.readPod<float>()};
        page.tile_ordinals.resize(kTiles);
        reader.readBytes(
            page.tile_ordinals.data(),
            page.tile_ordinals.size() * sizeof(std::uint32_t));
        const auto collision_count = reader.readPod<std::uint32_t>();
        if (!reader.ok() || collision_count > kTiles ||
            collision_count > reader.remaining() /
                (sizeof(std::uint16_t) * 4u))
            return lux::cxx::unexpected(
                std::string{"invalid LXTL collision metadata"});
        page.collision_boxes.resize(collision_count);
        for (auto& box : page.collision_boxes)
        {
            box.x = reader.readPod<std::uint16_t>();
            box.y = reader.readPod<std::uint16_t>();
            box.width = reader.readPod<std::uint16_t>();
            box.height = reader.readPod<std::uint16_t>();
        }
        if (!reader.ok() || !reader.eof() || !encodeWorldTilePage(root, page))
            return lux::cxx::unexpected(std::string{"invalid LXTL payload"});
        return page;
    }

    lux::cxx::expected<WorldTilePageDocument, std::string>
    loadWorldTilePage(
        const std::filesystem::path& root_document,
        std::string_view relative_path,
        const WorldSourceDocument& root,
        const WorldSourceCodecLimits& limits) noexcept
    {
        auto resolved = resolveWorldSourceDocument(
            root_document, relative_path);
        if (!resolved)
            return lux::cxx::unexpected(std::move(resolved.error()));
        auto bytes = readFile(*resolved, limits.maximum_bytes);
        if (!bytes)
            return lux::cxx::unexpected(std::move(bytes.error()));
        return decodeWorldTilePage(root, *bytes, limits);
    }

    lux::cxx::expected<std::vector<std::byte>, std::string>
    encodeWorldPixelPage(
        const WorldSourceDocument& root,
        const WorldPixelPageDocument& page) noexcept
    {
        constexpr std::size_t kPixels =
            static_cast<std::size_t>(kWorldLogicalChunkEdge)
            * kWorldLogicalChunkEdge;
        if (page.world != root.world || page.field.empty()
            || !pageSpaceMatches(
                root, page.space, page.cell,
                lux::authoring::EPartitionTopology::PLANAR_XY)
            || (page.material_base.size() != kPixels &&
                !(page.material_base.empty() && page.generator))
            || (page.generator &&
                (!page.generator->id.isValid()
                 || !lux::authoring::isCanonicalWorldName(
                     page.generator->id.name())
                 || page.generator->schema_version == 0u
                 || page.generator->config_schema_version == 0u
                 || page.generator->config.size() >
                     WorldSourceCodecLimits{}.maximum_bytes)))
        {
            return lux::cxx::unexpected(
                std::string{"LXPP has invalid dimensions or fields"});
        }
        std::vector<std::byte> bytes;
        ArchiveWriter writer{bytes};
        writer.writePod(kWorldPixelPageMagic);
        writer.writePod(kWorldPixelPageVersion);
        writeId(writer, page.world);
        writeId(writer, page.field);
        writeId(writer, page.space);
        writeCell(writer, page.cell);
        writer.writePod(static_cast<std::uint32_t>(
            page.material_base.size()));
        writer.writeBytes(
            page.material_base.data(),
            page.material_base.size() * sizeof(std::uint16_t));
        writer.writePod(static_cast<std::uint8_t>(page.generator.has_value()));
        if (page.generator)
        {
            writer.writePod(page.generator->id.hash());
            writer.writeString(page.generator->id.name());
            writer.writePod(page.generator->schema_version);
            writer.writePod(page.generator->config_schema_version);
            writer.writePod(page.generator->seed);
            writer.writePod(static_cast<std::uint64_t>(
                page.generator->config.size()));
            writer.writeBytes(
                page.generator->config.data(), page.generator->config.size());
        }
        if (bytes.size() > WorldSourceCodecLimits{}.maximum_bytes)
            return lux::cxx::unexpected(std::string{"LXPP exceeds byte limit"});
        return bytes;
    }

    lux::cxx::expected<WorldPixelPageDocument, std::string>
    decodeWorldPixelPage(
        const WorldSourceDocument& root,
        std::span<const std::byte> bytes,
        const WorldSourceCodecLimits& limits) noexcept
    {
        constexpr std::size_t kPixels =
            static_cast<std::size_t>(kWorldLogicalChunkEdge)
            * kWorldLogicalChunkEdge;
        if (bytes.size() > limits.maximum_bytes)
            return lux::cxx::unexpected(std::string{"LXPP exceeds byte limit"});
        ArchiveReader reader{bytes.data(), bytes.size()};
        if (reader.readPod<std::uint32_t>() != kWorldPixelPageMagic
            || reader.readPod<std::uint32_t>() != kWorldPixelPageVersion)
            return lux::cxx::unexpected(std::string{"invalid LXPP v1 header"});
        WorldPixelPageDocument page;
        page.world = readId<lux::authoring::WorldId>(reader);
        page.field = readId<lux::authoring::PixelFieldId>(reader);
        page.space = readId<lux::authoring::PartitionSpaceId>(reader);
        if (!readCell(reader, page.cell))
            return lux::cxx::unexpected(std::string{"invalid LXPP Cell"});
        const auto material_count = reader.readPod<std::uint32_t>();
        if (material_count != 0u && material_count != kPixels)
            return lux::cxx::unexpected(
                std::string{"invalid LXPP material count"});
        page.material_base.resize(material_count);
        reader.readBytes(
            page.material_base.data(),
            page.material_base.size() * sizeof(std::uint16_t));
        const auto has_generator = reader.readPod<std::uint8_t>();
        if (has_generator > 1u)
            return lux::cxx::unexpected(std::string{"invalid LXPP generator tag"});
        if (has_generator != 0u)
        {
            WorldPixelGeneratorSource generator;
            const auto hash = reader.readPod<std::uint64_t>();
            std::string name;
            if (!readString(reader, limits.maximum_string_bytes, name))
                return lux::cxx::unexpected(
                    std::string{"invalid LXPP generator id"});
            generator.id = lux::authoring::ChunkGeneratorId{name};
            if (generator.id.hash() != hash)
            {
                return lux::cxx::unexpected(
                    std::string{"LXPP generator id hash mismatch"});
            }
            generator.schema_version = reader.readPod<std::uint32_t>();
            generator.config_schema_version =
                reader.readPod<std::uint32_t>();
            generator.seed = reader.readPod<std::uint64_t>();
            const auto config_size = reader.readPod<std::uint64_t>();
            if (!reader.ok() || config_size > limits.maximum_bytes
                || config_size > reader.remaining())
                return lux::cxx::unexpected(
                    std::string{"invalid LXPP generator config"});
            generator.config.resize(static_cast<std::size_t>(config_size));
            reader.readBytes(generator.config.data(), generator.config.size());
            page.generator = std::move(generator);
        }
        if (!reader.ok() || !reader.eof() || !encodeWorldPixelPage(root, page))
            return lux::cxx::unexpected(std::string{"invalid LXPP payload"});
        return page;
    }

    lux::cxx::expected<WorldPixelPageDocument, std::string>
    loadWorldPixelPage(
        const std::filesystem::path& root_document,
        std::string_view relative_path,
        const WorldSourceDocument& root,
        const WorldSourceCodecLimits& limits) noexcept
    {
        auto resolved = resolveWorldSourceDocument(
            root_document, relative_path);
        if (!resolved)
            return lux::cxx::unexpected(std::move(resolved.error()));
        auto bytes = readFile(*resolved, limits.maximum_bytes);
        if (!bytes)
            return lux::cxx::unexpected(std::move(bytes.error()));
        return decodeWorldPixelPage(root, *bytes, limits);
    }
