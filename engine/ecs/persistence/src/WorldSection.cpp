#include <lux/engine/ecs/WorldSection.hpp>
#include <lux/engine/ecs/core/detail/WorldAccess.hpp>
#include <lux/engine/ecs/persistence/detail/LittleEndian.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lux::ecs
{
    namespace
    {
        constexpr std::array<char, 4> kMagic{'L', 'X', 'W', 'S'};
        constexpr std::uint16_t kVersion = 1;
        constexpr std::uint32_t kNullOrdinal = std::numeric_limits<std::uint32_t>::max();

        [[nodiscard]] bool lessId(
            const PersistentEntityId& left,
            const PersistentEntityId& right
        ) noexcept
        {
            const auto lhs = left.value.as_bytes();
            const auto rhs = right.value.as_bytes();
            return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
        }

        struct SelectedEntity final
        {
            Entity runtime{NullEntity};
            PersistentEntityId persistent;
        };

        class EncodePort final : public ComponentEncodePort
        {
          public:
            EncodePort(
                TaggedPropertyWriter& writer,
                const std::unordered_map<Entity, std::uint32_t>& entity_ordinals,
                std::vector<EntityReferenceRelocation>& entity_relocations,
                std::vector<PersistentReferenceRelocation>& persistent_relocations,
                std::uint32_t column,
                std::uint32_t cell
            ) noexcept
                : writer_(&writer),
                  entity_ordinals_(&entity_ordinals),
                  entity_relocations_(&entity_relocations),
                  persistent_relocations_(&persistent_relocations),
                  column_(column),
                  cell_(cell)
            {
            }

            lux::cxx::expected<void, EComponentCodecError> write(
                std::string_view name,
                EComponentWireType type,
                std::span<const std::byte> bytes
            ) noexcept override
            {
                return writer_->write(name, type, bytes);
            }

            lux::cxx::expected<void, EComponentCodecError> writeEntity(
                std::string_view name,
                Entity entity
            ) noexcept override
            {
                std::uint32_t ordinal = kNullOrdinal;
                if (entity != NullEntity)
                {
                    const auto iterator = entity_ordinals_->find(entity);
                    if (iterator == entity_ordinals_->end())
                        return lux::cxx::unexpected(EComponentCodecError::UNKNOWN_REFERENCE);
                    ordinal = iterator->second;
                }
                std::array<std::byte, sizeof(ordinal)> encoded_ordinal{};
                for (std::size_t index{}; index < encoded_ordinal.size(); ++index)
                {
                    encoded_ordinal[index] = static_cast<std::byte>(
                        (ordinal >> (index * 8U)) & 0xffU
                    );
                }
                auto written = writer_->write(
                    name,
                    EComponentWireType::LOCAL_ENTITY,
                    encoded_ordinal
                );
                if (!written)
                    return written;
                if (entity != NullEntity)
                {
                    try
                    {
                        entity_relocations_->push_back(EntityReferenceRelocation{
                            column_, cell_, writer_->lastPayloadOffset(), ordinal});
                    }
                    catch (...)
                    {
                        return lux::cxx::unexpected(EComponentCodecError::ALLOCATION_FAILURE);
                    }
                }
                return {};
            }

            lux::cxx::expected<void, EComponentCodecError> writeStableReference(
                std::string_view name,
                std::span<const std::byte> stable_id
            ) noexcept override
            {
                if (stable_id.size() != 16)
                    return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                auto written = writer_->write(
                    name,
                    EComponentWireType::STABLE_REFERENCE,
                    stable_id
                );
                if (!written)
                    return written;
                try
                {
                    std::array<std::uint8_t, 16> bytes{};
                    std::memcpy(bytes.data(), stable_id.data(), bytes.size());
                    persistent_relocations_->push_back(PersistentReferenceRelocation{
                        column_,
                        cell_,
                        writer_->lastPayloadOffset(),
                        PersistentEntityId{uuids::uuid{bytes}}
                    });
                }
                catch (...)
                {
                    return lux::cxx::unexpected(EComponentCodecError::ALLOCATION_FAILURE);
                }
                return {};
            }

          private:
            TaggedPropertyWriter* writer_{};
            const std::unordered_map<Entity, std::uint32_t>* entity_ordinals_{};
            std::vector<EntityReferenceRelocation>* entity_relocations_{};
            std::vector<PersistentReferenceRelocation>* persistent_relocations_{};
            std::uint32_t column_{};
            std::uint32_t cell_{};
        };

        class DecodePort final : public ComponentDecodePort
        {
          public:
            DecodePort(
                std::span<const std::byte> bytes,
                std::span<const std::string> names,
                const std::vector<Entity>& entities
            ) noexcept
                : reader_(bytes, names), entities_(&entities)
            {
            }

            bool next(EncodedPropertyView& property) noexcept override
            {
                return reader_.next(property);
            }

            lux::cxx::expected<Entity, EComponentCodecError> resolveEntity(
                std::span<const std::byte> encoded
            ) const noexcept override
            {
                if (encoded.size() != sizeof(std::uint32_t))
                    return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                std::uint32_t ordinal{};
                std::size_t offset{};
                if (!persistence::detail::readLittle(encoded, offset, ordinal))
                    return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                if (ordinal == kNullOrdinal)
                    return NullEntity;
                if (ordinal >= entities_->size())
                    return lux::cxx::unexpected(EComponentCodecError::UNKNOWN_REFERENCE);
                return (*entities_)[ordinal];
            }

            lux::cxx::expected<std::array<std::byte, 16>, EComponentCodecError>
            resolveStableReference(
                std::span<const std::byte> encoded
            ) const noexcept override
            {
                if (encoded.size() != 16)
                    return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                std::array<std::byte, 16> result{};
                std::memcpy(result.data(), encoded.data(), result.size());
                return result;
            }

            [[nodiscard]] bool valid() const noexcept
            {
                return reader_.valid();
            }

          private:
            TaggedPropertyReader reader_;
            const std::vector<Entity>* entities_{};
        };

        template <class T>
        [[nodiscard]] lux::cxx::expected<void, PersistenceFailure> checkedCount(
            persistence::detail::Reader& reader,
            std::uint32_t limit,
            T& output
        ) noexcept
        {
            output = reader.readUnsigned<T>();
            if (!reader.ok())
            {
                return lux::cxx::unexpected(
                    PersistenceFailure{EPersistenceError::TRUNCATED}
                );
            }
            if (output > static_cast<T>(limit))
            {
                return lux::cxx::unexpected(
                    PersistenceFailure{EPersistenceError::LIMIT_EXCEEDED}
                );
            }
            return {};
        }

        [[nodiscard]] lux::cxx::expected<void, PersistenceFailure>
        readBoundedString(
            persistence::detail::Reader& reader,
            std::uint64_t& aggregate_remaining,
            std::string& output
        )
        {
            const std::uint32_t size = reader.readUnsigned<std::uint32_t>();
            if (!reader.ok())
            {
                return lux::cxx::unexpected(
                    PersistenceFailure{EPersistenceError::TRUNCATED}
                );
            }
            if (size > aggregate_remaining)
            {
                return lux::cxx::unexpected(
                    PersistenceFailure{EPersistenceError::LIMIT_EXCEEDED}
                );
            }
            const auto bytes = reader.readSpan(size);
            if (!reader.ok())
            {
                return lux::cxx::unexpected(
                    PersistenceFailure{EPersistenceError::TRUNCATED}
                );
            }
            output.assign(
                reinterpret_cast<const char*>(bytes.data()),
                bytes.size()
            );
            aggregate_remaining -= size;
            return {};
        }

        template <class T>
        [[nodiscard]] bool payloadPod(
            std::span<const std::byte> payload,
            std::size_t offset,
            T& value
        ) noexcept
        {
            return persistence::detail::readLittle(payload, offset, value);
        }

        struct PersistentRelocationKey final
        {
            std::uint32_t column{};
            std::uint32_t cell{};
            std::uint32_t offset{};
            std::array<std::byte, 16> target{};

            [[nodiscard]] auto operator<=>(
                const PersistentRelocationKey&
            ) const noexcept = default;
        };

        [[nodiscard]] lux::cxx::expected<void, PersistenceFailure>
        validateImage(
            const WorldSectionImage& image,
            const WorldSectionLimits& limits
        ) noexcept
        {
            try
            {
                const auto invalid = [](EPersistenceError code)
                    -> lux::cxx::expected<void, PersistenceFailure>
                {
                    return lux::cxx::unexpected(PersistenceFailure{code});
                };

                if (image.id.value.is_nil())
                    return invalid(EPersistenceError::INVALID_SECTION_ID);
                if (image.property_names.size() > limits.max_names ||
                    image.schemas.size() > limits.max_schemas ||
                    image.archetypes.size() > limits.max_archetypes ||
                    image.entities.size() > limits.max_entities ||
                    image.columns.size() > limits.max_columns ||
                    image.entity_relocations.size() > limits.max_relocations ||
                    image.persistent_relocations.size() > limits.max_relocations)
                {
                    return invalid(EPersistenceError::LIMIT_EXCEEDED);
                }

                std::uint64_t name_bytes{};
                std::unordered_set<std::string_view> property_names;
                property_names.reserve(image.property_names.size());
                for (const std::string& name : image.property_names)
                {
                    if (name.empty())
                        return invalid(EPersistenceError::INVALID_NAME_TABLE);
                    name_bytes += name.size();
                    if (name_bytes > limits.max_name_bytes)
                        return invalid(EPersistenceError::LIMIT_EXCEEDED);
                    if (!property_names.emplace(name).second)
                        return invalid(EPersistenceError::INVALID_NAME_TABLE);
                }

                std::unordered_set<std::uint64_t> schema_hashes;
                schema_hashes.reserve(image.schemas.size());
                for (std::size_t index{}; index < image.schemas.size(); ++index)
                {
                    const WorldSchemaEntry& schema = image.schemas[index];
                    name_bytes += schema.id.name.size();
                    if (name_bytes > limits.max_name_bytes)
                        return invalid(EPersistenceError::LIMIT_EXCEEDED);
                    if (!schema.id.valid())
                        return invalid(EPersistenceError::INVALID_HASH);
                    if (schema.version == 0)
                        return invalid(EPersistenceError::INVALID_SCHEMA_VERSION);
                    if (!schema_hashes.emplace(schema.id.hash).second)
                        return invalid(EPersistenceError::DUPLICATE_SCHEMA);
                }

                std::vector<std::uint8_t> entity_membership(
                    image.entities.size(),
                    std::uint8_t{}
                );
                std::set<std::vector<std::uint32_t>> archetype_signatures;
                for (std::size_t archetype_index{};
                     archetype_index < image.archetypes.size();
                     ++archetype_index)
                {
                    const WorldArchetype& archetype = image.archetypes[archetype_index];
                    if (!std::is_sorted(
                            archetype.schema_indices.begin(),
                            archetype.schema_indices.end()
                        ) || std::adjacent_find(
                            archetype.schema_indices.begin(),
                            archetype.schema_indices.end()
                        ) != archetype.schema_indices.end())
                    {
                        return invalid(EPersistenceError::INVALID_ARCHETYPE);
                    }
                    for (const std::uint32_t schema : archetype.schema_indices)
                        if (schema >= image.schemas.size())
                            return invalid(EPersistenceError::INVALID_INDEX);
                    if (!std::is_sorted(
                            archetype.entity_ordinals.begin(),
                            archetype.entity_ordinals.end()
                        ) || std::adjacent_find(
                            archetype.entity_ordinals.begin(),
                            archetype.entity_ordinals.end()
                        ) != archetype.entity_ordinals.end())
                    {
                        return invalid(EPersistenceError::INVALID_ARCHETYPE);
                    }
                    for (const std::uint32_t ordinal : archetype.entity_ordinals)
                    {
                        if (ordinal >= image.entities.size() ||
                            image.entities[ordinal].archetype != archetype_index ||
                            entity_membership[ordinal] != 0)
                        {
                            return invalid(EPersistenceError::INVALID_ARCHETYPE);
                        }
                        entity_membership[ordinal] = 1;
                    }
                    if (!archetype_signatures.emplace(archetype.schema_indices).second)
                        return invalid(EPersistenceError::INVALID_ARCHETYPE);
                }
                if (std::find(entity_membership.begin(), entity_membership.end(), 0) !=
                    entity_membership.end())
                {
                    return invalid(EPersistenceError::INVALID_ARCHETYPE);
                }

                for (std::size_t ordinal{}; ordinal < image.entities.size(); ++ordinal)
                {
                    const WorldEntityRecord& entity = image.entities[ordinal];
                    if (entity.id.value.is_nil() || entity.archetype >= image.archetypes.size())
                        return invalid(EPersistenceError::INVALID_INDEX);
                    if (ordinal != 0 && !lessId(image.entities[ordinal - 1].id, entity.id))
                        return invalid(EPersistenceError::DUPLICATE_PERSISTENT_ID);
                }

                if (image.columns.size() != image.schemas.size())
                    return invalid(EPersistenceError::INVALID_INDEX);
                std::uint64_t cell_total{};
                std::uint64_t payload_total{};
                std::vector<std::vector<std::uint8_t>> components;
                components.reserve(image.columns.size());
                for (std::size_t column_index{};
                     column_index < image.columns.size();
                     ++column_index)
                {
                    const WorldComponentColumn& column = image.columns[column_index];
                    if (column.schema_index != column_index)
                        return invalid(EPersistenceError::INVALID_INDEX);
                    cell_total += column.cells.size();
                    if (cell_total > limits.max_cells)
                        return invalid(EPersistenceError::LIMIT_EXCEEDED);
                    components.emplace_back(image.entities.size(), std::uint8_t{});
                    for (const WorldComponentCell& cell : column.cells)
                    {
                        if (cell.entity_ordinal >= image.entities.size() ||
                            components.back()[cell.entity_ordinal] != 0)
                        {
                            return invalid(EPersistenceError::DUPLICATE_COMPONENT);
                        }
                        const auto& signature = image.archetypes[
                            image.entities[cell.entity_ordinal].archetype].schema_indices;
                        if (!std::binary_search(
                                signature.begin(),
                                signature.end(),
                                column.schema_index
                            ))
                        {
                            return invalid(EPersistenceError::INVALID_ARCHETYPE);
                        }
                        components.back()[cell.entity_ordinal] = 1;
                        payload_total += cell.payload.size();
                        if (payload_total > limits.max_payload_bytes)
                            return invalid(EPersistenceError::LIMIT_EXCEEDED);
                    }
                }
                for (std::size_t ordinal{}; ordinal < image.entities.size(); ++ordinal)
                {
                    const auto& signature = image.archetypes[
                        image.entities[ordinal].archetype].schema_indices;
                    for (const std::uint32_t schema : signature)
                        if (components[schema][ordinal] == 0)
                            return invalid(EPersistenceError::INVALID_ARCHETYPE);
                }

                using EntityKey = std::tuple<
                    std::uint32_t,
                    std::uint32_t,
                    std::uint32_t,
                    std::uint32_t>;
                std::vector<EntityKey> entity_keys;
                entity_keys.reserve(image.entity_relocations.size());
                for (const EntityReferenceRelocation& relocation : image.entity_relocations)
                {
                    if (relocation.column >= image.columns.size() ||
                        relocation.cell >= image.columns[relocation.column].cells.size() ||
                        relocation.target_ordinal >= image.entities.size())
                    {
                        return invalid(EPersistenceError::INVALID_RELOCATION);
                    }
                    const auto& payload = image.columns[relocation.column]
                        .cells[relocation.cell].payload;
                    std::uint32_t encoded{};
                    if (!payloadPod(payload, relocation.payload_offset, encoded) ||
                        encoded != relocation.target_ordinal)
                    {
                        return invalid(EPersistenceError::INVALID_RELOCATION);
                    }
                    entity_keys.emplace_back(
                        relocation.column,
                        relocation.cell,
                        relocation.payload_offset,
                        relocation.target_ordinal
                    );
                }
                std::sort(entity_keys.begin(), entity_keys.end());
                if (std::adjacent_find(entity_keys.begin(), entity_keys.end()) != entity_keys.end())
                    return invalid(EPersistenceError::INVALID_RELOCATION);

                std::vector<PersistentRelocationKey> persistent_keys;
                persistent_keys.reserve(image.persistent_relocations.size());
                for (const PersistentReferenceRelocation& relocation : image.persistent_relocations)
                {
                    if (relocation.column >= image.columns.size() ||
                        relocation.cell >= image.columns[relocation.column].cells.size() ||
                        relocation.target.value.is_nil())
                    {
                        return invalid(EPersistenceError::INVALID_RELOCATION);
                    }
                    const auto& payload = image.columns[relocation.column]
                        .cells[relocation.cell].payload;
                    const auto target = relocation.target.value.as_bytes();
                    if (relocation.payload_offset > payload.size() ||
                        target.size() > payload.size() - relocation.payload_offset ||
                        std::memcmp(
                            payload.data() + relocation.payload_offset,
                            target.data(),
                            target.size()
                        ) != 0)
                    {
                        return invalid(EPersistenceError::INVALID_RELOCATION);
                    }
                    std::array<std::byte, 16> target_key{};
                    std::memcpy(target_key.data(), target.data(), target_key.size());
                    persistent_keys.push_back(PersistentRelocationKey{
                        relocation.column,
                        relocation.cell,
                        relocation.payload_offset,
                        target_key
                    });
                }
                std::sort(persistent_keys.begin(), persistent_keys.end());
                if (std::adjacent_find(persistent_keys.begin(), persistent_keys.end()) !=
                    persistent_keys.end())
                {
                    return invalid(EPersistenceError::INVALID_RELOCATION);
                }

                std::size_t seen_entity_references{};
                std::size_t seen_persistent_references{};
                for (std::uint32_t column{}; column < image.columns.size(); ++column)
                {
                    const auto& cells = image.columns[column].cells;
                    for (std::uint32_t cell{}; cell < cells.size(); ++cell)
                    {
                        const auto& payload = cells[cell].payload;
                        std::size_t offset{};
                        std::uint32_t property_count{};
                        if (!payloadPod(payload, offset, property_count))
                            return invalid(EPersistenceError::INVALID_PAYLOAD);
                        offset += sizeof(property_count);
                        if (property_count > 4096)
                            return invalid(EPersistenceError::LIMIT_EXCEEDED);
                        for (std::uint32_t property{}; property < property_count; ++property)
                        {
                            std::uint32_t name{};
                            std::uint8_t wire{};
                            std::uint32_t size{};
                            if (!payloadPod(payload, offset, name))
                                return invalid(EPersistenceError::INVALID_PAYLOAD);
                            offset += sizeof(name);
                            if (!payloadPod(payload, offset, wire))
                                return invalid(EPersistenceError::INVALID_PAYLOAD);
                            offset += sizeof(wire);
                            if (!payloadPod(payload, offset, size))
                                return invalid(EPersistenceError::INVALID_PAYLOAD);
                            offset += sizeof(size);
                            if (name >= image.property_names.size() ||
                                wire > static_cast<std::uint8_t>(EComponentWireType::STABLE_REFERENCE) ||
                                offset > payload.size() || size > payload.size() - offset)
                            {
                                return invalid(EPersistenceError::INVALID_PAYLOAD);
                            }
                            const auto type = static_cast<EComponentWireType>(wire);
                            if (type == EComponentWireType::LOCAL_ENTITY)
                            {
                                std::uint32_t ordinal{};
                                if (size != sizeof(ordinal) || !payloadPod(payload, offset, ordinal))
                                    return invalid(EPersistenceError::INVALID_PAYLOAD);
                                if (ordinal != kNullOrdinal)
                                {
                                    const EntityKey key{
                                        column,
                                        cell,
                                        static_cast<std::uint32_t>(offset),
                                        ordinal};
                                    if (!std::binary_search(entity_keys.begin(), entity_keys.end(), key))
                                        return invalid(EPersistenceError::INVALID_RELOCATION);
                                    ++seen_entity_references;
                                }
                            }
                            else if (type == EComponentWireType::STABLE_REFERENCE)
                            {
                                if (size != 16)
                                    return invalid(EPersistenceError::INVALID_PAYLOAD);
                                std::array<std::byte, 16> target{};
                                std::memcpy(target.data(), payload.data() + offset, target.size());
                                const PersistentRelocationKey key{
                                    column,
                                    cell,
                                    static_cast<std::uint32_t>(offset),
                                    target};
                                if (!std::binary_search(
                                        persistent_keys.begin(),
                                        persistent_keys.end(),
                                        key
                                    ))
                                {
                                    return invalid(EPersistenceError::INVALID_RELOCATION);
                                }
                                ++seen_persistent_references;
                            }
                            offset += size;
                        }
                        if (offset != payload.size())
                            return invalid(EPersistenceError::INVALID_PAYLOAD);
                    }
                }
                if (seen_entity_references != entity_keys.size() ||
                    seen_persistent_references != persistent_keys.size())
                {
                    return invalid(EPersistenceError::INVALID_RELOCATION);
                }
                return {};
            }
            catch (...)
            {
                return lux::cxx::unexpected(
                    PersistenceFailure{EPersistenceError::ALLOCATION_FAILURE}
                );
            }
        }
    } // namespace

    lux::cxx::expected<WorldSectionImage, PersistenceFailure>
    WorldSectionWriter::build(
        const World& world,
        const ComponentSchemaSet& schemas,
        WorldSectionId id,
        WorldSectionWriteSelection selection
    ) noexcept
    {
        if (!detail::WorldColdAccess::ownerIdle(world))
        {
            return lux::cxx::unexpected(
                PersistenceFailure{EPersistenceError::WORLD_BUSY}
            );
        }
        try
        {
            std::vector<SelectedEntity> entities;
            entities.reserve(selection.entities.size());
            std::unordered_set<Entity> selected_runtime_entities;
            selected_runtime_entities.reserve(selection.entities.size());
            for (const Entity entity : selection.entities)
            {
                if (!world.valid(entity))
                    return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::INVALID_ENTITY});
                if (!selected_runtime_entities.insert(entity).second)
                    return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::DUPLICATE_ENTITY});
                const auto* persistent = world.find<PersistentId>(entity);
                if (persistent == nullptr || persistent->value.value.is_nil())
                    return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::MISSING_PERSISTENT_ID});
                entities.push_back({entity, persistent->value});
            }
            std::sort(entities.begin(), entities.end(), [](const auto& left, const auto& right) { return lessId(left.persistent, right.persistent); });
            for (std::size_t index = 1; index < entities.size(); ++index)
            {
                if (entities[index - 1].persistent == entities[index].persistent)
                    return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::DUPLICATE_PERSISTENT_ID});
            }
            std::unordered_map<Entity, std::uint32_t> entity_ordinals;
            entity_ordinals.reserve(entities.size());
            for (std::uint32_t ordinal{}; ordinal < entities.size(); ++ordinal)
                entity_ordinals.emplace(entities[ordinal].runtime, ordinal);

            std::vector<const ComponentSchema*> selected_schemas;
            std::vector<ComponentSchemaId> seen_schema_ids;
            seen_schema_ids.reserve(selection.schemas.size());
            for (const ComponentSchemaId& schema_id : selection.schemas)
            {
                if (std::find_if(
                        seen_schema_ids.begin(),
                        seen_schema_ids.end(),
                        [&schema_id](const ComponentSchemaId& value)
                        {
                            return value == schema_id;
                        }
                    ) != seen_schema_ids.end())
                {
                    return lux::cxx::unexpected(PersistenceFailure{
                        EPersistenceError::DUPLICATE_SCHEMA,
                        0,
                        schema_id
                    });
                }
                seen_schema_ids.push_back(schema_id);
                const ComponentSchema* schema = schemas.find(schema_id);
                if (schema == nullptr)
                    return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::MISSING_SCHEMA, 0, schema_id});
                if (schema->cpp_type == lux::cxx::typeToken<PersistentId>())
                    continue;
                if (!schema->codec.present())
                    return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::MISSING_CODEC, 0, schema_id});
                selected_schemas.push_back(schema);
            }
            std::sort(selected_schemas.begin(), selected_schemas.end(), [](const auto* left, const auto* right)
            {
                if (left->id.hash != right->id.hash)
                    return left->id.hash < right->id.hash;
                return left->id.name < right->id.name;
            });

            WorldSectionImage image;
            image.id = id;
            for (const ComponentSchema* schema : selected_schemas)
                image.schemas.push_back(WorldSchemaEntry{schema->id, schema->version});
            image.columns.resize(image.schemas.size());
            for (std::uint32_t index{}; index < image.columns.size(); ++index)
                image.columns[index].schema_index = index;

            std::map<std::vector<std::uint32_t>, std::uint32_t> archetype_map;
            for (std::uint32_t ordinal{}; ordinal < entities.size(); ++ordinal)
            {
                const SelectedEntity& selected = entities[ordinal];
                std::vector<std::uint32_t> signature;
                for (std::uint32_t schema_index{}; schema_index < selected_schemas.size(); ++schema_index)
                {
                    if (selected_schemas[schema_index]->operations.has(world, selected.runtime))
                        signature.push_back(schema_index);
                }
                auto [iterator, inserted] = archetype_map.emplace(signature, static_cast<std::uint32_t>(archetype_map.size()));
                if (inserted)
                    image.archetypes.push_back(WorldArchetype{signature, {}});
                image.archetypes[iterator->second].entity_ordinals.push_back(ordinal);
                image.entities.push_back(WorldEntityRecord{selected.persistent, iterator->second});

                for (const std::uint32_t schema_index : signature)
                {
                    WorldComponentColumn& column = image.columns[schema_index];
                    const std::uint32_t cell_index = static_cast<std::uint32_t>(column.cells.size());
                    column.cells.push_back(WorldComponentCell{ordinal, {}});
                    WorldComponentCell& cell = column.cells.back();
                    TaggedPropertyWriter property_writer(cell.payload, image.property_names);
                    EncodePort port(
                        property_writer,
                        entity_ordinals,
                        image.entity_relocations,
                        image.persistent_relocations,
                        schema_index,
                        cell_index
                    );
                    auto encoded = selected_schemas[schema_index]->codec.encode(
                        *selected_schemas[schema_index],
                        world,
                        selected.runtime,
                        port
                    );
                    if (!encoded)
                    {
                        EPersistenceError error =
                            EPersistenceError::COMPONENT_ENCODE_FAILED;
                        if (encoded.error() ==
                            EComponentCodecError::UNKNOWN_REFERENCE)
                        {
                            error = EPersistenceError::ENTITY_REFERENCE_OUTSIDE_SECTION;
                        }
                        else if (encoded.error() ==
                            EComponentCodecError::ALLOCATION_FAILURE)
                        {
                            error = EPersistenceError::ALLOCATION_FAILURE;
                        }
                        else if (encoded.error() ==
                            EComponentCodecError::LIMIT_EXCEEDED)
                        {
                            error = EPersistenceError::LIMIT_EXCEEDED;
                        }
                        return lux::cxx::unexpected(PersistenceFailure{error, ordinal, selected_schemas[schema_index]->id});
                    }
                    if (auto finished = property_writer.finish(); !finished)
                    {
                        const auto error = finished.error() ==
                            EComponentCodecError::ALLOCATION_FAILURE
                            ? EPersistenceError::ALLOCATION_FAILURE
                            : EPersistenceError::COMPONENT_ENCODE_FAILED;
                        return lux::cxx::unexpected(PersistenceFailure{
                            error, ordinal, selected_schemas[schema_index]->id});
                    }
                }
            }
            if (auto valid = validateImage(image, WorldSectionLimits{}); !valid)
                return lux::cxx::unexpected(valid.error());
            return image;
        }
        catch (...)
        {
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::ALLOCATION_FAILURE});
        }
    }

    lux::cxx::expected<std::unique_ptr<World>, PersistenceFailure>
    WorldSectionReader::materialize(
        const WorldSectionImage& image,
        const ComponentSchemaSet& schemas,
        WorldConfig config
    ) noexcept
    {
        try
        {
            if (auto valid = validateImage(image, WorldSectionLimits{}); !valid)
                return lux::cxx::unexpected(valid.error());
            std::vector<const ComponentSchema*> resolved;
            resolved.reserve(image.schemas.size());
            for (const WorldSchemaEntry& entry : image.schemas)
            {
                const ComponentSchema* schema = schemas.find(entry.id);
                if (schema == nullptr)
                    return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::MISSING_SCHEMA, 0, entry.id});
                if (!schema->codec.present())
                    return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::MISSING_CODEC, 0, entry.id});
                resolved.push_back(schema);
            }

            auto world = std::make_unique<World>(config);
            auto edit_result = world->edit();
            if (!edit_result)
                return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::WORLD_BUSY});
            auto edit = std::move(*edit_result);
            std::vector<Entity> entities;
            entities.reserve(image.entities.size());
            for (const WorldEntityRecord& record : image.entities)
            {
                Entity entity = edit.create();
                edit.emplace<PersistentId>(entity, record.id);
                entities.push_back(entity);
            }

            for (const WorldComponentColumn& column : image.columns)
            {
                if (column.schema_index >= resolved.size())
                    return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::INVALID_INDEX});
                const ComponentSchema& schema = *resolved[column.schema_index];
                for (const WorldComponentCell& cell : column.cells)
                {
                    if (cell.entity_ordinal >= entities.size())
                        return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::INVALID_INDEX});
                    DecodePort port(cell.payload, image.property_names, entities);
                    auto decoded = schema.codec.decode(
                        schema,
                        edit,
                        entities[cell.entity_ordinal],
                        image.schemas[column.schema_index].version,
                        port
                    );
                    if (!decoded || !port.valid())
                    {
                        EPersistenceError error =
                            EPersistenceError::COMPONENT_DECODE_FAILED;
                        if (!decoded && decoded.error() ==
                            EComponentCodecError::ALLOCATION_FAILURE)
                        {
                            error = EPersistenceError::ALLOCATION_FAILURE;
                        }
                        else if (!decoded && decoded.error() ==
                            EComponentCodecError::LIMIT_EXCEEDED)
                        {
                            error = EPersistenceError::LIMIT_EXCEEDED;
                        }
                        return lux::cxx::unexpected(PersistenceFailure{
                            error, cell.entity_ordinal, schema.id});
                    }
                }
            }
            edit = {};
            detail::establishWorldChangeBaseline(*world);
            return world;
        }
        catch (...)
        {
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::ALLOCATION_FAILURE});
        }
    }

    lux::cxx::expected<std::vector<std::byte>, PersistenceFailure>
    encodeWorldSection(const WorldSectionImage& image) noexcept
    {
        try
        {
            if (auto valid = validateImage(image, WorldSectionLimits{}); !valid)
                return lux::cxx::unexpected(valid.error());
            std::vector<std::byte> bytes;
            persistence::detail::Writer writer(bytes);
            writer.writeBytes(kMagic.data(), kMagic.size());
            writer.writeUnsigned(kVersion);
            writer.writeUnsigned(std::uint16_t{});
            writer.writeUuid(image.id.value);

            writer.writeUnsigned(static_cast<std::uint32_t>(image.property_names.size()));
            for (const std::string& name : image.property_names)
                writer.writeString(name);
            writer.writeUnsigned(static_cast<std::uint32_t>(image.schemas.size()));
            for (const WorldSchemaEntry& schema : image.schemas)
            {
                writer.writeUnsigned(schema.id.hash);
                writer.writeString(schema.id.name);
                writer.writeUnsigned(schema.version);
            }
            writer.writeUnsigned(static_cast<std::uint32_t>(image.archetypes.size()));
            for (const WorldArchetype& archetype : image.archetypes)
            {
                writer.writeUnsigned(static_cast<std::uint32_t>(archetype.schema_indices.size()));
                for (const auto value : archetype.schema_indices) writer.writeUnsigned(value);
                writer.writeUnsigned(static_cast<std::uint32_t>(archetype.entity_ordinals.size()));
                for (const auto value : archetype.entity_ordinals) writer.writeUnsigned(value);
            }
            writer.writeUnsigned(static_cast<std::uint32_t>(image.entities.size()));
            for (const WorldEntityRecord& entity : image.entities)
            {
                writer.writeUuid(entity.id.value);
                writer.writeUnsigned(entity.archetype);
            }
            writer.writeUnsigned(static_cast<std::uint32_t>(image.columns.size()));
            for (const WorldComponentColumn& column : image.columns)
            {
                writer.writeUnsigned(column.schema_index);
                writer.writeUnsigned(static_cast<std::uint32_t>(column.cells.size()));
                for (const WorldComponentCell& cell : column.cells)
                {
                    writer.writeUnsigned(cell.entity_ordinal);
                    writer.writeUnsigned(static_cast<std::uint32_t>(cell.payload.size()));
                    writer.writeBytes(cell.payload.data(), cell.payload.size());
                }
            }
            writer.writeUnsigned(static_cast<std::uint32_t>(image.entity_relocations.size()));
            for (const EntityReferenceRelocation& relocation : image.entity_relocations)
            {
                writer.writeUnsigned(relocation.column);
                writer.writeUnsigned(relocation.cell);
                writer.writeUnsigned(relocation.payload_offset);
                writer.writeUnsigned(relocation.target_ordinal);
            }
            writer.writeUnsigned(static_cast<std::uint32_t>(image.persistent_relocations.size()));
            for (const PersistentReferenceRelocation& relocation : image.persistent_relocations)
            {
                writer.writeUnsigned(relocation.column);
                writer.writeUnsigned(relocation.cell);
                writer.writeUnsigned(relocation.payload_offset);
                writer.writeUuid(relocation.target.value);
            }
            return bytes;
        }
        catch (...)
        {
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::ALLOCATION_FAILURE});
        }
    }

    lux::cxx::expected<WorldSectionImage, PersistenceFailure>
    decodeWorldSection(
        std::span<const std::byte> bytes,
        WorldSectionLimits limits
    ) noexcept
    {
        try
        {
            if (bytes.size() > limits.max_image_bytes)
                return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::LIMIT_EXCEEDED});
            persistence::detail::Reader reader(bytes);
            std::array<char, 4> magic{};
            reader.readBytes(magic.data(), magic.size());
            if (!reader.ok())
                return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::TRUNCATED});
            if (magic != kMagic)
                return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::INVALID_MAGIC});
            const auto version = reader.readUnsigned<std::uint16_t>();
            if (!reader.ok())
                return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::TRUNCATED});
            if (version != kVersion)
                return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::UNSUPPORTED_VERSION});
            (void)reader.readUnsigned<std::uint16_t>();
            if (!reader.ok())
                return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::TRUNCATED});

            WorldSectionImage image;
            image.id.value = reader.readUuid();
            if (!reader.ok())
                return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::TRUNCATED});
            std::uint32_t count{};
            std::uint64_t remaining_name_bytes = limits.max_name_bytes;
            if (auto checked = checkedCount(reader, limits.max_names, count);
                !checked)
            {
                return lux::cxx::unexpected(checked.error());
            }
            image.property_names.reserve(count);
            for (std::uint32_t index{}; index < count; ++index)
            {
                std::string name;
                if (auto read = readBoundedString(
                        reader, remaining_name_bytes, name
                    ); !read)
                {
                    return lux::cxx::unexpected(read.error());
                }
                image.property_names.push_back(std::move(name));
            }

            if (auto checked = checkedCount(reader, limits.max_schemas, count);
                !checked)
            {
                return lux::cxx::unexpected(checked.error());
            }
            for (std::uint32_t index{}; index < count; ++index)
            {
                WorldSchemaEntry entry;
                entry.id.hash = reader.readUnsigned<std::uint64_t>();
                if (auto read = readBoundedString(
                        reader, remaining_name_bytes, entry.id.name
                    ); !read)
                {
                    return lux::cxx::unexpected(read.error());
                }
                entry.version = reader.readUnsigned<std::uint32_t>();
                if (!entry.id.valid())
                    return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::INVALID_HASH});
                image.schemas.push_back(std::move(entry));
            }

            if (auto checked = checkedCount(
                    reader, limits.max_archetypes, count
                ); !checked)
            {
                return lux::cxx::unexpected(checked.error());
            }
            for (std::uint32_t index{}; index < count; ++index)
            {
                WorldArchetype archetype;
                std::uint32_t inner{};
                if (auto checked = checkedCount(
                        reader, limits.max_schemas, inner
                    ); !checked)
                {
                    return lux::cxx::unexpected(checked.error());
                }
                while (inner--) archetype.schema_indices.push_back(reader.readUnsigned<std::uint32_t>());
                if (auto checked = checkedCount(
                        reader, limits.max_entities, inner
                    ); !checked)
                {
                    return lux::cxx::unexpected(checked.error());
                }
                while (inner--) archetype.entity_ordinals.push_back(reader.readUnsigned<std::uint32_t>());
                image.archetypes.push_back(std::move(archetype));
            }

            if (auto checked = checkedCount(reader, limits.max_entities, count);
                !checked)
            {
                return lux::cxx::unexpected(checked.error());
            }
            for (std::uint32_t index{}; index < count; ++index)
                image.entities.push_back(WorldEntityRecord{PersistentEntityId{reader.readUuid()}, reader.readUnsigned<std::uint32_t>()});

            if (auto checked = checkedCount(reader, limits.max_columns, count);
                !checked)
            {
                return lux::cxx::unexpected(checked.error());
            }
            std::uint64_t payload_total{};
            std::uint64_t cell_total{};
            for (std::uint32_t index{}; index < count; ++index)
            {
                WorldComponentColumn column;
                column.schema_index = reader.readUnsigned<std::uint32_t>();
                std::uint32_t cells{};
                if (auto checked = checkedCount(reader, limits.max_cells, cells);
                    !checked)
                {
                    return lux::cxx::unexpected(checked.error());
                }
                cell_total += cells;
                if (cell_total > limits.max_cells)
                    return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::LIMIT_EXCEEDED});
                while (cells--)
                {
                    WorldComponentCell cell;
                    cell.entity_ordinal = reader.readUnsigned<std::uint32_t>();
                    const auto size = reader.readUnsigned<std::uint32_t>();
                    payload_total += size;
                    if (payload_total > limits.max_payload_bytes)
                        return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::LIMIT_EXCEEDED});
                    const auto payload = reader.readSpan(size);
                    cell.payload.assign(payload.begin(), payload.end());
                    column.cells.push_back(std::move(cell));
                }
                image.columns.push_back(std::move(column));
            }

            if (auto checked = checkedCount(
                    reader, limits.max_relocations, count
                ); !checked)
            {
                return lux::cxx::unexpected(checked.error());
            }
            while (count--)
            {
                image.entity_relocations.push_back(EntityReferenceRelocation{
                    reader.readUnsigned<std::uint32_t>(), reader.readUnsigned<std::uint32_t>(),
                    reader.readUnsigned<std::uint32_t>(), reader.readUnsigned<std::uint32_t>()});
            }
            if (auto checked = checkedCount(
                    reader, limits.max_relocations, count
                ); !checked)
            {
                return lux::cxx::unexpected(checked.error());
            }
            while (count--)
            {
                image.persistent_relocations.push_back(PersistentReferenceRelocation{
                    reader.readUnsigned<std::uint32_t>(), reader.readUnsigned<std::uint32_t>(),
                    reader.readUnsigned<std::uint32_t>(), PersistentEntityId{reader.readUuid()}});
            }

            if (!reader.ok() || !reader.eof())
                return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::TRUNCATED});

            if (auto valid = validateImage(image, limits); !valid)
                return lux::cxx::unexpected(valid.error());
            return image;
        }
        catch (...)
        {
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::ALLOCATION_FAILURE});
        }
    }
} // namespace lux::ecs
