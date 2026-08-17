    lux::cxx::expected<std::vector<std::byte>, std::string>
    encodeWorldActorDocument(const WorldActorDocument& source) noexcept
    {
        const WorldSourceCodecLimits limits;
        auto document = source;
        if (document.world.empty() || document.actor.empty()
            || document.actor_class.empty() || document.space.empty()
            || document.actor_class.size() > limits.maximum_string_bytes
            || document.data_layers.size() > limits.maximum_data_layers
            || document.references.size() >
                limits.maximum_actor_references
            || document.name_table.size() > limits.maximum_bytes
            || document.components.size()
                > limits.maximum_components_per_actor)
        {
            return lux::cxx::unexpected(
                std::string{"LXAD has invalid identity or bounds"});
        }
        const bool finite_position = std::visit(
            [](const auto& value) { return lux::spatial::isFinite(value); },
            document.position);
        if (!finite_position)
            return lux::cxx::unexpected(
                std::string{"LXAD position is not finite"});
        if (document.transform_parent &&
            (document.transform_parent->empty() ||
             *document.transform_parent == document.actor))
        {
            return lux::cxx::unexpected(
                std::string{"LXAD has an invalid Transform parent"});
        }
        std::ranges::sort(
            document.data_layers,
            {},
            [](const auto& layer) { return layer.name(); });
        std::ranges::sort(
            document.references,
            [](const auto& left, const auto& right)
            {
                if (left.target != right.target)
                    return uuidLess(left.target.value(), right.target.value());
                return left.kind < right.kind;
            });
        for (std::size_t index = 0u;
             index < document.data_layers.size(); ++index)
        {
            if (!document.data_layers[index].valid() ||
                (index != 0u && document.data_layers[index - 1u] ==
                    document.data_layers[index]))
            {
                return lux::cxx::unexpected(
                    std::string{"LXAD has invalid Data Layers"});
            }
        }
        for (std::size_t index = 0u;
             index < document.references.size(); ++index)
        {
            const auto& reference = document.references[index];
            if (reference.target.empty() ||
                reference.target == document.actor ||
                static_cast<std::uint8_t>(reference.kind) >
                    static_cast<std::uint8_t>(
                        EWorldActorReferenceKind::OPTIONAL_REFERENCE) ||
                (index != 0u && document.references[index - 1u].target ==
                    reference.target))
            {
                return lux::cxx::unexpected(
                    std::string{"LXAD has invalid Actor references"});
            }
        }
        if (document.transform_parent &&
            !std::ranges::any_of(
                document.references,
                [&](const auto& reference)
                {
                    return reference.target == *document.transform_parent &&
                        reference.kind == EWorldActorReferenceKind::LOCAL;
                }))
        {
            return lux::cxx::unexpected(
                std::string{"LXAD Transform parent is not a LOCAL reference"});
        }
        {
            ArchiveReader reader{
                document.name_table.data(), document.name_table.size()};
            static_cast<void>(lux::serialize::NameTable::deserialize(reader));
            if (!reader.ok() || !reader.eof())
                return lux::cxx::unexpected(
                    std::string{"LXAD has an invalid NameTable"});
        }
        std::ranges::sort(document.components, {}, [](const auto& component)
        {
            return component.schema_name;
        });
        for (std::size_t index = 0u;
             index < document.components.size();
             ++index)
        {
            const auto& component = document.components[index];
            if (component.schema_name.empty()
                || component.schema_name.size() > limits.maximum_string_bytes
                || component.schema_version == 0u
                || component.tagged_payload.size() > limits.maximum_bytes
                || (index != 0u
                    && document.components[index - 1u].schema_name
                        == component.schema_name))
            {
                return lux::cxx::unexpected(
                    std::string{"LXAD has an invalid component record"});
            }
        }

        std::vector<std::byte> bytes;
        ArchiveWriter writer{bytes};
        writer.writePod(kWorldActorDocumentMagic);
        writer.writePod(kWorldActorDocumentVersion);
        writeId(writer, document.world);
        writeId(writer, document.actor);
        writer.writeString(document.actor_class);
        writeId(writer, document.space);
        writePosition(writer, document.position);
        writer.writePod(static_cast<std::uint8_t>(
            document.transform_parent.has_value()));
        if (document.transform_parent)
            writeId(writer, *document.transform_parent);
        writer.writePod(static_cast<std::uint32_t>(
            document.data_layers.size()));
        for (const auto& layer : document.data_layers)
            writeLayer(writer, layer);
        writer.writePod(static_cast<std::uint32_t>(
            document.references.size()));
        for (const auto& reference : document.references)
        {
            writeId(writer, reference.target);
            writer.writePod(static_cast<std::uint8_t>(reference.kind));
        }
        writer.writePod(static_cast<std::uint64_t>(document.name_table.size()));
        writer.writeBytes(
            document.name_table.data(), document.name_table.size());
        writer.writePod(static_cast<std::uint32_t>(document.components.size()));
        for (const auto& component : document.components)
        {
            writer.writeString(component.schema_name);
            writer.writePod(component.schema_version);
            writer.writePod(static_cast<std::uint64_t>(
                component.tagged_payload.size()));
            writer.writeBytes(
                component.tagged_payload.data(),
                component.tagged_payload.size());
        }
        if (bytes.size() > limits.maximum_bytes)
            return lux::cxx::unexpected(
                std::string{"LXAD exceeds the byte limit"});
        return bytes;
    }

    lux::cxx::expected<WorldActorDocument, std::string>
    decodeWorldActorDocument(
        std::span<const std::byte> bytes,
        const WorldSourceCodecLimits& limits) noexcept
    {
        if (bytes.size() > limits.maximum_bytes)
            return lux::cxx::unexpected(
                std::string{"LXAD exceeds the byte limit"});
        ArchiveReader reader{bytes.data(), bytes.size()};
        if (reader.readPod<std::uint32_t>() != kWorldActorDocumentMagic
            || reader.readPod<std::uint32_t>()
                != kWorldActorDocumentVersion)
        {
            return lux::cxx::unexpected(
                std::string{"LXAD has an invalid v2 header"});
        }
        WorldActorDocument document;
        document.world = readId<lux::entity_scene::EntitySceneId>(reader);
        document.actor = readId<
            lux::entity_scene::PersistentEntityId>(reader);
        if (!readString(
                reader,
                limits.maximum_string_bytes,
                document.actor_class))
        {
            return lux::cxx::unexpected(
                std::string{"LXAD has malformed Actor Class"});
        }
        document.space = readId<lux::authoring::PartitionSpaceId>(reader);
        if (!readPosition(reader, document.position))
            return lux::cxx::unexpected(
                std::string{"LXAD has malformed Actor position"});
        const auto has_parent = reader.readPod<std::uint8_t>();
        if (has_parent > 1u)
            return lux::cxx::unexpected(
                std::string{"LXAD has malformed Transform parent"});
        if (has_parent != 0u)
            document.transform_parent =
                readId<lux::entity_scene::PersistentEntityId>(reader);
        std::uint32_t source_count = 0u;
        if (!readCount(reader, limits.maximum_data_layers, source_count))
            return lux::cxx::unexpected(
                std::string{"LXAD has too many Data Layers"});
        document.data_layers.reserve(source_count);
        for (std::uint32_t index = 0u; index < source_count; ++index)
        {
            lux::authoring::DataLayerId layer;
            if (!readLayer(reader, limits, layer))
                return lux::cxx::unexpected(
                    std::string{"LXAD has a malformed Data Layer"});
            document.data_layers.push_back(std::move(layer));
        }
        if (!readCount(
                reader,
                limits.maximum_actor_references,
                source_count))
        {
            return lux::cxx::unexpected(
                std::string{"LXAD has too many Actor references"});
        }
        document.references.reserve(source_count);
        for (std::uint32_t index = 0u; index < source_count; ++index)
        {
            WorldActorSourceReference reference;
            reference.target = readId<
                lux::entity_scene::PersistentEntityId>(reader);
            const auto kind = reader.readPod<std::uint8_t>();
            if (kind > static_cast<std::uint8_t>(
                    EWorldActorReferenceKind::OPTIONAL_REFERENCE))
            {
                return lux::cxx::unexpected(
                    std::string{"LXAD has a malformed Actor reference"});
            }
            reference.kind = static_cast<EWorldActorReferenceKind>(kind);
            document.references.push_back(std::move(reference));
        }
        const auto name_size = reader.readPod<std::uint64_t>();
        if (!reader.ok()
            || name_size > limits.maximum_bytes
            || name_size > reader.remaining())
        {
            return lux::cxx::unexpected(
                std::string{"LXAD has malformed root fields"});
        }
        document.name_table.resize(static_cast<std::size_t>(name_size));
        reader.readBytes(
            document.name_table.data(), document.name_table.size());
        std::uint32_t component_count = 0u;
        if (!readCount(
                reader,
                limits.maximum_components_per_actor,
                component_count))
        {
            return lux::cxx::unexpected(
                std::string{"LXAD has too many components"});
        }
        document.components.reserve(component_count);
        for (std::uint32_t index = 0u; index < component_count; ++index)
        {
            WorldActorComponentRecord component;
            if (!readString(
                    reader,
                    limits.maximum_string_bytes,
                    component.schema_name))
                return lux::cxx::unexpected(
                    std::string{"LXAD has a malformed component name"});
            component.schema_version = reader.readPod<std::uint32_t>();
            const auto payload_size = reader.readPod<std::uint64_t>();
            if (!reader.ok() || payload_size > limits.maximum_bytes
                || payload_size > reader.remaining())
                return lux::cxx::unexpected(
                    std::string{"LXAD has a malformed component payload"});
            component.tagged_payload.resize(
                static_cast<std::size_t>(payload_size));
            reader.readBytes(
                component.tagged_payload.data(),
                component.tagged_payload.size());
            document.components.push_back(std::move(component));
        }
        if (!reader.ok() || !reader.eof())
            return lux::cxx::unexpected(
                std::string{"LXAD is truncated or has trailing bytes"});
        auto canonical = encodeWorldActorDocument(document);
        if (!canonical)
            return lux::cxx::unexpected(std::move(canonical.error()));
        std::ranges::sort(
            document.data_layers,
            {},
            [](const auto& layer) { return layer.name(); });
        std::ranges::sort(
            document.references,
            [](const auto& left, const auto& right)
            {
                if (left.target != right.target)
                    return uuidLess(left.target.value(), right.target.value());
                return left.kind < right.kind;
            });
        std::ranges::sort(document.components, {}, [](const auto& component)
        {
            return component.schema_name;
        });
        return document;
    }

    lux::cxx::expected<std::vector<std::byte>, std::string>
    encodeWorldInstancePage(
        const WorldSourceDocument& root,
        const WorldInstancePageDocument& source) noexcept
    {
        const WorldSourceCodecLimits limits;
        if (auto valid_root = validateRoot(root, limits); !valid_root)
            return lux::cxx::unexpected(std::move(valid_root.error()));
        auto page = source;
        const auto* space = findSpace(root, page.space);
        const auto* instance_set = findInstanceSet(root, page.instance_set);
        if (page.world != root.world || page.instance_set.empty() || !space
            || !page.cell.valid() || page.cell.topology != space->topology
            || !instance_set
            || page.instances.size() > limits.maximum_instances_per_page
            || page.tombstones.size() > limits.maximum_instances_per_page)
        {
            return lux::cxx::unexpected(
                std::string{"LXIP has invalid root fields"});
        }
        std::unordered_set<std::uint64_t> live;
        std::unordered_set<std::uint64_t> dead;
        live.reserve(page.instances.size());
        dead.reserve(page.tombstones.size());
        for (const auto id : page.tombstones)
        {
            if (id == 0u || id >= instance_set->next_local_id ||
                !dead.insert(id).second)
                return lux::cxx::unexpected(
                    std::string{"LXIP has an invalid tombstone"});
        }
        for (auto& instance : page.instances)
        {
            if (instance.id.set != page.instance_set
                || instance.id.local_id == 0u
                || instance.id.local_id >= instance_set->next_local_id
                || !live.insert(instance.id.local_id).second
                || dead.contains(instance.id.local_id)
                || instance.mesh.is_nil()
                || instance.data_layers.size() > limits.maximum_data_layers)
            {
                return lux::cxx::unexpected(
                    std::string{"LXIP has an invalid Instance identity"});
            }
            const auto cell = positionCell(*space, instance.position);
            if (!cell || *cell != page.cell)
            {
                return lux::cxx::unexpected(
                    std::string{"LXIP Instance is outside its Cell"});
            }
            float quaternion_norm = 0.0f;
            for (const auto value : instance.rotation)
            {
                if (!std::isfinite(value))
                    return lux::cxx::unexpected(
                        std::string{"LXIP has a non-finite rotation"});
                quaternion_norm += value * value;
            }
            if (!(quaternion_norm > 1.0e-12f)
                || !std::isfinite(instance.scale[0])
                || !std::isfinite(instance.scale[1])
                || !std::isfinite(instance.scale[2]))
            {
                return lux::cxx::unexpected(
                    std::string{"LXIP has an invalid transform"});
            }
            // Canonical encoding must be idempotent. A quaternion normalized
            // into float storage is generally not bit-exactly unit length;
            // renormalizing those decoded bits on every encode changes the
            // image (and therefore its SHA-256) by one or more ULPs. Normalize
            // authored values once, but preserve an already-unit float image.
            constexpr float kUnitQuaternionTolerance = 1.0e-5f;
            if (std::abs(quaternion_norm - 1.0f) >
                    kUnitQuaternionTolerance)
            {
                const auto inverse_norm =
                    1.0f / std::sqrt(quaternion_norm);
                for (auto& value : instance.rotation)
                    value *= inverse_norm;
            }
            std::ranges::sort(instance.data_layers, {}, [](const auto& layer)
            {
                return layer.name();
            });
            for (const auto& row : instance.custom_values)
            for (const auto value : row)
            {
                if (!std::isfinite(value))
                    return lux::cxx::unexpected(
                        std::string{"LXIP has a non-finite custom value"});
            }
        }
        std::ranges::sort(page.instances, {}, [](const auto& instance)
        {
            return instance.id.local_id;
        });
        std::ranges::sort(page.tombstones);

        std::vector<std::byte> bytes;
        ArchiveWriter writer{bytes};
        writer.writePod(kWorldInstancePageMagic);
        writer.writePod(kWorldInstancePageVersion);
        writeId(writer, page.world);
        writeId(writer, page.instance_set);
        writeId(writer, page.space);
        writeCell(writer, page.cell);
        writer.writePod(static_cast<std::uint32_t>(page.instances.size()));
        for (const auto& instance : page.instances)
        {
            writer.writePod(instance.id.local_id);
            writePosition(writer, instance.position);
            for (const auto value : instance.rotation)
                writer.writePod(value);
            for (const auto value : instance.scale)
                writer.writePod(value);
            writer.writeUuid(instance.mesh);
            writer.writeUuid(instance.material_instance);
            writer.writePod(instance.rgba8);
            for (const auto& row : instance.custom_values)
            for (const auto value : row)
                writer.writePod(value);
            writer.writePod(static_cast<std::uint32_t>(
                instance.data_layers.size()));
            for (const auto& layer : instance.data_layers)
                writeLayer(writer, layer);
            writer.writePod(instance.editor_flags);
        }
        writer.writePod(static_cast<std::uint32_t>(page.tombstones.size()));
        for (const auto id : page.tombstones)
            writer.writePod(id);
        if (bytes.size() > limits.maximum_bytes)
            return lux::cxx::unexpected(
                std::string{"LXIP exceeds the byte limit"});
        return bytes;
    }

    lux::cxx::expected<WorldInstancePageDocument, std::string>
    decodeWorldInstancePage(
        const WorldSourceDocument& root,
        std::span<const std::byte> bytes,
        const WorldSourceCodecLimits& limits) noexcept
    {
        if (bytes.size() > limits.maximum_bytes)
            return lux::cxx::unexpected(
                std::string{"LXIP exceeds the byte limit"});
        ArchiveReader reader{bytes.data(), bytes.size()};
        if (reader.readPod<std::uint32_t>() != kWorldInstancePageMagic
            || reader.readPod<std::uint32_t>() != kWorldInstancePageVersion)
        {
            return lux::cxx::unexpected(
                std::string{"invalid LXIP v2 header"});
        }
        WorldInstancePageDocument page;
        page.world = readId<lux::entity_scene::EntitySceneId>(reader);
        page.instance_set = readId<lux::authoring::InstanceSetId>(reader);
        page.space = readId<lux::authoring::PartitionSpaceId>(reader);
        if (!readCell(reader, page.cell))
            return lux::cxx::unexpected(std::string{"invalid LXIP Cell"});
        std::uint32_t count = 0u;
        if (!readCount(reader, limits.maximum_instances_per_page, count))
            return lux::cxx::unexpected(std::string{"too many LXIP Instances"});
        page.instances.reserve(count);
        for (std::uint32_t index = 0u; index < count; ++index)
        {
            EditableWorldInstance instance;
            instance.id.set = page.instance_set;
            instance.id.local_id = reader.readPod<std::uint64_t>();
            if (!readPosition(reader, instance.position))
                return lux::cxx::unexpected(
                    std::string{"malformed LXIP position"});
            for (auto& value : instance.rotation)
                value = reader.readPod<float>();
            instance.scale = {
                reader.readPod<float>(),
                reader.readPod<float>(),
                reader.readPod<float>()};
            instance.mesh = reader.readUuid();
            instance.material_instance = reader.readUuid();
            instance.rgba8 = reader.readPod<std::uint32_t>();
            for (auto& row : instance.custom_values)
            for (auto& value : row)
                value = reader.readPod<float>();
            std::uint32_t layer_count = 0u;
            if (!readCount(reader, limits.maximum_data_layers, layer_count))
                return lux::cxx::unexpected(
                    std::string{"too many LXIP Data Layers"});
            instance.data_layers.reserve(layer_count);
            for (std::uint32_t layer = 0u; layer < layer_count; ++layer)
            {
                lux::authoring::DataLayerId id;
                if (!readLayer(reader, limits, id))
                    return lux::cxx::unexpected(
                        std::string{"malformed LXIP Data Layer"});
                instance.data_layers.push_back(std::move(id));
            }
            instance.editor_flags = reader.readPod<std::uint32_t>();
            page.instances.push_back(std::move(instance));
        }
        if (!readCount(reader, limits.maximum_instances_per_page, count))
            return lux::cxx::unexpected(std::string{"too many LXIP tombstones"});
        page.tombstones.reserve(count);
        for (std::uint32_t index = 0u; index < count; ++index)
            page.tombstones.push_back(reader.readPod<std::uint64_t>());
        if (!reader.ok() || !reader.eof())
            return lux::cxx::unexpected(
                std::string{"LXIP is truncated or has trailing bytes"});
        auto canonical = encodeWorldInstancePage(root, page);
        if (!canonical)
            return lux::cxx::unexpected(std::move(canonical.error()));
        std::ranges::sort(page.instances, {}, [](const auto& instance)
        {
            return instance.id.local_id;
        });
        std::ranges::sort(page.tombstones);
        return page;
    }

    lux::cxx::expected<WorldInstancePageDocument, std::string>
    loadWorldInstancePage(
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
        return decodeWorldInstancePage(root, *bytes, limits);
    }
