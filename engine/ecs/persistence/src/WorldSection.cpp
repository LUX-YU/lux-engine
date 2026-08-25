#include <lux/engine/ecs/WorldSection.hpp>

#include <lux/engine/ecs/core/detail/WorldAccess.hpp>
#include <lux/engine/ecs/persistence/detail/LittleEndian.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace lux::ecs
{
    namespace
    {
        constexpr std::array kMagic{
            std::byte{'L'}, std::byte{'X'}, std::byte{'W'}, std::byte{'C'}};
        constexpr std::uint32_t kVersion = 1U;

        [[nodiscard]] PersistenceFailure failure(
            EPersistenceError code,
            std::uint32_t entity = 0U,
            ComponentSchemaId schema = {}
        )
        {
            return {code, entity, std::move(schema)};
        }

        [[nodiscard]] bool lessId(
            const PersistentEntityId& left,
            const PersistentEntityId& right
        ) noexcept
        {
            const auto lhs = left.value.as_bytes();
            const auto rhs = right.value.as_bytes();
            return std::lexicographical_compare(
                lhs.begin(), lhs.end(), rhs.begin(), rhs.end()
            );
        }

        [[nodiscard]] bool lessSchema(
            const ComponentSchema* left,
            const ComponentSchema* right
        ) noexcept
        {
            if (left->id.hash != right->id.hash)
            {
                return left->id.hash < right->id.hash;
            }
            return left->id.name < right->id.name;
        }

        struct BindingLookup final
        {
            std::vector<const ComponentPersistenceBinding*> bindings;
            std::vector<std::shared_ptr<const void>> pins;

            [[nodiscard]] const ComponentPersistenceBinding* find(
                const ComponentSchemaId& id
            ) const noexcept
            {
                const auto iterator = std::lower_bound(
                    bindings.begin(), bindings.end(), id,
                    [](const ComponentPersistenceBinding* binding,
                       const ComponentSchemaId& value)
                    {
                        if (binding->schema().id.hash != value.hash)
                        {
                            return binding->schema().id.hash < value.hash;
                        }
                        return binding->schema().id.name < value.name;
                    }
                );
                return iterator != bindings.end() &&
                        (*iterator)->schema().id == id
                    ? *iterator
                    : nullptr;
            }
        };

        [[nodiscard]] lux::cxx::expected<BindingLookup, PersistenceFailure>
        makeBindingLookup(
            const ComponentSchemaSet& schemas,
            std::span<const ComponentPersistenceContribution> contributions
        ) noexcept
        {
            try
            {
                BindingLookup result;
                for (const auto& contribution : contributions)
                {
                    if (contribution.code_lifetime)
                    {
                        result.pins.push_back(contribution.code_lifetime);
                    }
                    for (const auto& binding : contribution.bindings)
                    {
                        const ComponentSchema* current = schemas.find(
                            binding.schema().id
                        );
                        if (current == nullptr ||
                            current->version != binding.schema().version ||
                            current->cpp_type.hash() !=
                                binding.schema().cpp_type.hash() ||
                            current->cpp_type.name() !=
                                binding.schema().cpp_type.name())
                        {
                            return lux::cxx::unexpected(failure(
                                EPersistenceError::BINDING_MISMATCH,
                                0U,
                                binding.schema().id
                            ));
                        }
                        result.bindings.push_back(&binding);
                    }
                }
                std::sort(
                    result.bindings.begin(),
                    result.bindings.end(),
                    [](const auto* left, const auto* right)
                    {
                        return lessSchema(&left->schema(), &right->schema());
                    }
                );
                for (std::size_t index = 1U;
                     index < result.bindings.size(); ++index)
                {
                    if (result.bindings[index - 1U]->schema().id ==
                        result.bindings[index]->schema().id)
                    {
                        return lux::cxx::unexpected(failure(
                            EPersistenceError::DUPLICATE_BINDING,
                            0U,
                            result.bindings[index]->schema().id
                        ));
                    }
                }
                return result;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    failure(EPersistenceError::ALLOCATION_FAILURE)
                );
            }
        }

        [[nodiscard]] EPersistenceError mapEncodeFailure(
            const lux::serialization::SerializationFailure& source
        ) noexcept
        {
            if (source.code ==
                lux::serialization::ESerializationError::ALLOCATION_FAILURE)
            {
                return EPersistenceError::ALLOCATION_FAILURE;
            }
            if (source.code ==
                lux::serialization::ESerializationError::LIMIT_EXCEEDED)
            {
                return EPersistenceError::LIMIT_EXCEEDED;
            }
            return EPersistenceError::ENTITY_REFERENCE_OUTSIDE_SECTION;
        }

        [[nodiscard]] EPersistenceError mapDecodeFailure(
            const lux::serialization::SerializationFailure& source
        ) noexcept
        {
            if (source.code ==
                lux::serialization::ESerializationError::ALLOCATION_FAILURE)
            {
                return EPersistenceError::ALLOCATION_FAILURE;
            }
            if (source.code ==
                lux::serialization::ESerializationError::LIMIT_EXCEEDED)
            {
                return EPersistenceError::LIMIT_EXCEEDED;
            }
            return EPersistenceError::COMPONENT_DECODE_FAILED;
        }

        [[nodiscard]] bool archetypeContains(
            const WorldArchetype& archetype,
            std::uint32_t schema
        ) noexcept
        {
            return std::binary_search(
                archetype.schema_indices.begin(),
                archetype.schema_indices.end(),
                schema
            );
        }

        [[nodiscard]] lux::cxx::expected<void, PersistenceFailure>
        validateImage(
            const WorldSectionImage& image,
            const WorldSectionLimits& limits
        ) noexcept
        {
            try
            {
            if (image.id.value.is_nil())
            {
                return lux::cxx::unexpected(
                    failure(EPersistenceError::INVALID_SECTION_ID)
                );
            }
            if (image.schemas.size() > limits.max_schemas ||
                image.archetypes.size() > limits.max_archetypes ||
                image.entities.size() > limits.max_entities ||
                image.columns.size() > limits.max_columns)
            {
                return lux::cxx::unexpected(
                    failure(EPersistenceError::LIMIT_EXCEEDED)
                );
            }

            std::uint64_t name_bytes{};
            for (std::size_t index{}; index < image.schemas.size(); ++index)
            {
                const auto& schema = image.schemas[index];
                if (!schema.id.valid())
                {
                    return lux::cxx::unexpected(failure(
                        EPersistenceError::INVALID_HASH,
                        0U,
                        schema.id
                    ));
                }
                if (schema.version == 0U)
                {
                    return lux::cxx::unexpected(failure(
                        EPersistenceError::INVALID_SCHEMA_VERSION,
                        0U,
                        schema.id
                    ));
                }
                name_bytes += schema.id.name.size();
                if (name_bytes > limits.max_name_bytes)
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::LIMIT_EXCEEDED)
                    );
                }
                for (std::size_t other = index + 1U;
                     other < image.schemas.size(); ++other)
                {
                    if (schema.id == image.schemas[other].id)
                    {
                        return lux::cxx::unexpected(failure(
                            EPersistenceError::DUPLICATE_SCHEMA,
                            0U,
                            schema.id
                        ));
                    }
                }
            }

            if (!std::is_sorted(
                    image.entities.begin(), image.entities.end(),
                    [](const auto& left, const auto& right)
                    {
                        return lessId(left.id, right.id);
                    }))
            {
                return lux::cxx::unexpected(
                    failure(EPersistenceError::INVALID_INDEX)
                );
            }
            for (std::size_t index{}; index < image.entities.size(); ++index)
            {
                const auto& entity = image.entities[index];
                if (entity.id.value.is_nil())
                {
                    return lux::cxx::unexpected(failure(
                        EPersistenceError::MISSING_PERSISTENT_ID,
                        static_cast<std::uint32_t>(index)
                    ));
                }
                if (index != 0U &&
                    entity.id == image.entities[index - 1U].id)
                {
                    return lux::cxx::unexpected(failure(
                        EPersistenceError::DUPLICATE_PERSISTENT_ID,
                        static_cast<std::uint32_t>(index)
                    ));
                }
                if (entity.archetype >= image.archetypes.size())
                {
                    return lux::cxx::unexpected(failure(
                        EPersistenceError::INVALID_ARCHETYPE,
                        static_cast<std::uint32_t>(index)
                    ));
                }
            }

            std::vector<bool> archetype_entities(image.entities.size());
            for (const auto& archetype : image.archetypes)
            {
                if (!std::is_sorted(
                        archetype.schema_indices.begin(),
                        archetype.schema_indices.end()) ||
                    std::adjacent_find(
                        archetype.schema_indices.begin(),
                        archetype.schema_indices.end()) !=
                        archetype.schema_indices.end())
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::INVALID_ARCHETYPE)
                    );
                }
                for (const auto schema : archetype.schema_indices)
                {
                    if (schema >= image.schemas.size())
                    {
                        return lux::cxx::unexpected(
                            failure(EPersistenceError::INVALID_INDEX)
                        );
                    }
                }
                for (const auto ordinal : archetype.entity_ordinals)
                {
                    if (ordinal >= image.entities.size() ||
                        archetype_entities[ordinal] ||
                        &archetype != &image.archetypes[
                            image.entities[ordinal].archetype])
                    {
                        return lux::cxx::unexpected(
                            failure(EPersistenceError::INVALID_ARCHETYPE)
                        );
                    }
                    archetype_entities[ordinal] = true;
                }
            }
            if (std::find(
                    archetype_entities.begin(),
                    archetype_entities.end(),
                    false) != archetype_entities.end())
            {
                return lux::cxx::unexpected(
                    failure(EPersistenceError::INVALID_ARCHETYPE)
                );
            }

            std::vector<bool> column_schemas(image.schemas.size());
            std::uint64_t total_rows{};
            std::uint64_t total_payload{};
            for (const auto& column : image.columns)
            {
                if (column.schema_index >= image.schemas.size() ||
                    column_schemas[column.schema_index])
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::DUPLICATE_COMPONENT)
                    );
                }
                column_schemas[column.schema_index] = true;
                if (!std::is_sorted(
                        column.entity_ordinals.begin(),
                        column.entity_ordinals.end()) ||
                    std::adjacent_find(
                        column.entity_ordinals.begin(),
                        column.entity_ordinals.end()) !=
                        column.entity_ordinals.end())
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::INVALID_COLUMN)
                    );
                }
                for (const auto ordinal : column.entity_ordinals)
                {
                    if (ordinal >= image.entities.size() ||
                        !archetypeContains(
                            image.archetypes[image.entities[ordinal].archetype],
                            column.schema_index))
                    {
                        return lux::cxx::unexpected(
                            failure(EPersistenceError::INVALID_COLUMN)
                        );
                    }
                }
                const auto expected_rows = std::count_if(
                    image.entities.begin(), image.entities.end(),
                    [&](const WorldEntityRecord& entity)
                    {
                        return archetypeContains(
                            image.archetypes[entity.archetype],
                            column.schema_index
                        );
                    }
                );
                if (expected_rows != column.entity_ordinals.size())
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::INVALID_COLUMN)
                    );
                }
                if (column.fixed_width)
                {
                    const auto row_count = static_cast<std::uint64_t>(
                        column.entity_ordinals.size()
                    );
                    if (!column.row_offsets.empty() ||
                        (column.fixed_stride != 0U &&
                         row_count >
                            std::numeric_limits<std::uint64_t>::max() /
                                column.fixed_stride) ||
                        static_cast<std::uint64_t>(column.payload.size()) !=
                            row_count * column.fixed_stride)
                    {
                        return lux::cxx::unexpected(
                            failure(EPersistenceError::INVALID_PAYLOAD)
                        );
                    }
                }
                else
                {
                    if (column.fixed_stride != 0U ||
                        column.row_offsets.size() !=
                            column.entity_ordinals.size() + 1U ||
                        column.row_offsets.empty() ||
                        column.row_offsets.front() != 0U ||
                        column.row_offsets.back() != column.payload.size() ||
                        !std::is_sorted(
                            column.row_offsets.begin(),
                            column.row_offsets.end()))
                    {
                        return lux::cxx::unexpected(
                            failure(EPersistenceError::INVALID_PAYLOAD)
                        );
                    }
                }
                total_rows += column.entity_ordinals.size();
                total_payload += column.payload.size();
                if (total_rows > limits.max_rows ||
                    total_payload > limits.max_payload_bytes)
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::LIMIT_EXCEEDED)
                    );
                }
            }
            for (std::size_t schema{}; schema < image.schemas.size(); ++schema)
            {
                const bool used = std::any_of(
                    image.archetypes.begin(), image.archetypes.end(),
                    [schema](const WorldArchetype& archetype)
                    {
                        return archetypeContains(
                            archetype,
                            static_cast<std::uint32_t>(schema)
                        );
                    }
                );
                if (used != column_schemas[schema])
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::INVALID_COLUMN)
                    );
                }
            }
            return {};
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    failure(EPersistenceError::ALLOCATION_FAILURE)
                );
            }
            catch (const std::length_error&)
            {
                return lux::cxx::unexpected(
                    failure(EPersistenceError::LIMIT_EXCEEDED)
                );
            }
        }

        [[nodiscard]] bool countFits(std::size_t value) noexcept
        {
            return value <= std::numeric_limits<std::uint32_t>::max();
        }
    } // namespace

    lux::cxx::expected<WorldSectionImage, PersistenceFailure>
    WorldSectionWriter::build(
        const World& world,
        const ComponentSchemaSet& schemas,
        std::span<const ComponentPersistenceContribution> contributions,
        WorldSectionId id,
        WorldSectionWriteSelection selection
    ) noexcept
    {
        if (!detail::WorldColdAccess::ownerIdle(world))
        {
            return lux::cxx::unexpected(failure(EPersistenceError::WORLD_BUSY));
        }
        if (id.value.is_nil())
        {
            return lux::cxx::unexpected(
                failure(EPersistenceError::INVALID_SECTION_ID)
            );
        }
        auto lookup = makeBindingLookup(schemas, contributions);
        if (!lookup)
        {
            return lux::cxx::unexpected(lookup.error());
        }
        try
        {
            struct SelectedEntity final
            {
                Entity runtime{NullEntity};
                PersistentEntityId persistent;
            };

            std::vector<SelectedEntity> selected_entities;
            selected_entities.reserve(selection.entities.size());
            for (const Entity entity : selection.entities)
            {
                if (!world.valid(entity))
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::INVALID_ENTITY)
                    );
                }
                if (std::any_of(
                        selected_entities.begin(), selected_entities.end(),
                        [entity](const auto& item)
                        {
                            return item.runtime == entity;
                        }))
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::DUPLICATE_ENTITY)
                    );
                }
                const PersistentId* persistent = world.find<PersistentId>(entity);
                if (persistent == nullptr || persistent->value.value.is_nil())
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::MISSING_PERSISTENT_ID)
                    );
                }
                selected_entities.push_back({entity, persistent->value});
            }
            std::sort(
                selected_entities.begin(), selected_entities.end(),
                [](const auto& left, const auto& right)
                {
                    return lessId(left.persistent, right.persistent);
                }
            );
            for (std::size_t index = 1U; index < selected_entities.size(); ++index)
            {
                if (selected_entities[index - 1U].persistent ==
                    selected_entities[index].persistent)
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::DUPLICATE_PERSISTENT_ID)
                    );
                }
            }

            std::vector<const ComponentSchema*> selected_schemas;
            selected_schemas.reserve(selection.schemas.size());
            for (const auto& id_value : selection.schemas)
            {
                const ComponentSchema* schema = schemas.find(id_value);
                if (schema == nullptr)
                {
                    return lux::cxx::unexpected(failure(
                        EPersistenceError::MISSING_SCHEMA,
                        0U,
                        id_value
                    ));
                }
                selected_schemas.push_back(schema);
            }
            std::sort(
                selected_schemas.begin(), selected_schemas.end(), lessSchema
            );
            if (std::adjacent_find(
                    selected_schemas.begin(), selected_schemas.end(),
                    [](const auto* left, const auto* right)
                    {
                        return left->id == right->id;
                    }) != selected_schemas.end())
            {
                return lux::cxx::unexpected(
                    failure(EPersistenceError::DUPLICATE_SCHEMA)
                );
            }

            WorldSectionImage image;
            image.id = id;
            for (const auto* schema : selected_schemas)
            {
                image.schemas.push_back({schema->id, schema->version});
            }
            image.entities.reserve(selected_entities.size());
            for (std::size_t ordinal{}; ordinal < selected_entities.size(); ++ordinal)
            {
                std::vector<std::uint32_t> signature;
                for (std::size_t schema{}; schema < selected_schemas.size(); ++schema)
                {
                    if (selected_schemas[schema]->operations.has(
                            world, selected_entities[ordinal].runtime))
                    {
                        signature.push_back(static_cast<std::uint32_t>(schema));
                    }
                }
                auto archetype = std::find_if(
                    image.archetypes.begin(), image.archetypes.end(),
                    [&](const WorldArchetype& candidate)
                    {
                        return candidate.schema_indices == signature;
                    }
                );
                if (archetype == image.archetypes.end())
                {
                    image.archetypes.push_back({std::move(signature), {}});
                    archetype = std::prev(image.archetypes.end());
                }
                const auto archetype_index = static_cast<std::uint32_t>(
                    std::distance(image.archetypes.begin(), archetype)
                );
                archetype->entity_ordinals.push_back(
                    static_cast<std::uint32_t>(ordinal)
                );
                image.entities.push_back({
                    selected_entities[ordinal].persistent,
                    archetype_index
                });
            }

            std::vector<EntityOrdinal> entity_ordinals;
            entity_ordinals.reserve(selected_entities.size());
            for (std::size_t ordinal{}; ordinal < selected_entities.size(); ++ordinal)
            {
                entity_ordinals.push_back({
                    selected_entities[ordinal].runtime,
                    static_cast<std::uint32_t>(ordinal)
                });
            }
            std::sort(
                entity_ordinals.begin(), entity_ordinals.end(),
                [](const auto& left, const auto& right)
                {
                    return entityBits(left.entity) < entityBits(right.entity);
                }
            );

            for (std::size_t schema_index{};
                 schema_index < selected_schemas.size(); ++schema_index)
            {
                std::vector<Entity> row_entities;
                std::vector<std::uint32_t> row_ordinals;
                for (std::size_t ordinal{}; ordinal < selected_entities.size(); ++ordinal)
                {
                    if (selected_schemas[schema_index]->operations.has(
                            world, selected_entities[ordinal].runtime))
                    {
                        row_entities.push_back(selected_entities[ordinal].runtime);
                        row_ordinals.push_back(static_cast<std::uint32_t>(ordinal));
                    }
                }
                if (row_entities.empty())
                {
                    continue;
                }
                const ComponentPersistenceBinding* binding =
                    lookup->find(selected_schemas[schema_index]->id);
                if (binding == nullptr)
                {
                    return lux::cxx::unexpected(failure(
                        EPersistenceError::MISSING_BINDING,
                        0U,
                        selected_schemas[schema_index]->id
                    ));
                }
                WorldComponentColumn column;
                column.schema_index = static_cast<std::uint32_t>(schema_index);
                column.entity_ordinals = std::move(row_ordinals);
                column.row_offsets.reserve(row_entities.size() + 1U);
                EcsBinaryWriter writer(
                    column.payload,
                    entity_ordinals,
                    &column.row_offsets
                );
                auto encoded = binding->encode()(world, row_entities, writer);
                if (!encoded)
                {
                    return lux::cxx::unexpected(failure(
                        mapEncodeFailure(encoded.error()),
                        0U,
                        selected_schemas[schema_index]->id
                    ));
                }
                if (column.row_offsets.size() != row_entities.size() + 1U)
                {
                    return lux::cxx::unexpected(failure(
                        EPersistenceError::INVALID_PAYLOAD,
                        0U,
                        selected_schemas[schema_index]->id
                    ));
                }
                const std::uint64_t candidate_stride =
                    column.row_offsets[1U] - column.row_offsets[0U];
                column.fixed_width = true;
                for (std::size_t row{1U}; row < row_entities.size(); ++row)
                {
                    if (column.row_offsets[row + 1U] -
                            column.row_offsets[row] != candidate_stride)
                    {
                        column.fixed_width = false;
                        break;
                    }
                }
                if (column.fixed_width)
                {
                    column.fixed_stride = candidate_stride;
                    column.row_offsets.clear();
                }
                image.columns.push_back(std::move(column));
            }
            if (auto valid = validateImage(image, {}); !valid)
            {
                return lux::cxx::unexpected(valid.error());
            }
            return image;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                failure(EPersistenceError::ALLOCATION_FAILURE)
            );
        }
    }

    lux::cxx::expected<std::unique_ptr<World>, PersistenceFailure>
    WorldSectionReader::materialize(
        const WorldSectionImage& image,
        const ComponentSchemaSet& schemas,
        std::span<const ComponentPersistenceContribution> contributions,
        WorldConfig config
    ) noexcept
    {
        if (auto valid = validateImage(image, {}); !valid)
        {
            return lux::cxx::unexpected(valid.error());
        }
        auto lookup = makeBindingLookup(schemas, contributions);
        if (!lookup)
        {
            return lux::cxx::unexpected(lookup.error());
        }
        try
        {
            std::vector<const ComponentSchema*> current_schemas;
            current_schemas.reserve(image.schemas.size());
            for (const auto& entry : image.schemas)
            {
                const ComponentSchema* schema = schemas.find(entry.id);
                if (schema == nullptr)
                {
                    return lux::cxx::unexpected(failure(
                        EPersistenceError::MISSING_SCHEMA, 0U, entry.id
                    ));
                }
                if (schema->version != entry.version)
                {
                    return lux::cxx::unexpected(failure(
                        EPersistenceError::INVALID_SCHEMA_VERSION,
                        0U,
                        entry.id
                    ));
                }
                if (lookup->find(entry.id) == nullptr)
                {
                    return lux::cxx::unexpected(failure(
                        EPersistenceError::MISSING_BINDING, 0U, entry.id
                    ));
                }
                current_schemas.push_back(schema);
            }

            auto world = std::make_unique<World>(config);
            std::vector<Entity> ordinal_entities;
            ordinal_entities.reserve(image.entities.size());
            {
                auto edit = detail::WorldColdAccess::suppressingEdit(*world);
                for (std::size_t ordinal{}; ordinal < image.entities.size(); ++ordinal)
                {
                    ordinal_entities.push_back(edit.create());
                }
                for (const auto& column : image.columns)
                {
                    std::vector<Entity> row_entities;
                    row_entities.reserve(column.entity_ordinals.size());
                    for (const auto ordinal : column.entity_ordinals)
                    {
                        row_entities.push_back(ordinal_entities[ordinal]);
                    }
                    const auto* binding = lookup->find(
                        image.schemas[column.schema_index].id
                    );
                    EcsBinaryReader reader(
                        column.payload,
                        column.row_offsets,
                        column.fixed_width,
                        column.fixed_stride,
                        ordinal_entities
                    );
                    auto decoded = binding->decode()(edit, row_entities, reader);
                    if (!decoded)
                    {
                        return lux::cxx::unexpected(failure(
                            mapDecodeFailure(decoded.error()),
                            0U,
                            image.schemas[column.schema_index].id
                        ));
                    }
                }
                for (std::size_t ordinal{}; ordinal < image.entities.size(); ++ordinal)
                {
                    const PersistentId* existing = world->find<PersistentId>(
                        ordinal_entities[ordinal]
                    );
                    if (existing == nullptr)
                    {
                        edit.emplace<PersistentId>(
                            ordinal_entities[ordinal],
                            image.entities[ordinal].id
                        );
                    }
                    else if (existing->value != image.entities[ordinal].id)
                    {
                        return lux::cxx::unexpected(failure(
                            EPersistenceError::INVALID_PAYLOAD,
                            static_cast<std::uint32_t>(ordinal),
                            persistentIdComponentSchema().id
                        ));
                    }
                }
            }
            detail::establishWorldChangeBaseline(*world);
            return world;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                failure(EPersistenceError::ALLOCATION_FAILURE)
            );
        }
    }

    lux::cxx::expected<std::vector<std::byte>, PersistenceFailure>
    encodeWorldSection(const WorldSectionImage& image) noexcept
    {
        if (auto valid = validateImage(image, {}); !valid)
        {
            return lux::cxx::unexpected(valid.error());
        }
        if (!countFits(image.schemas.size()) ||
            !countFits(image.archetypes.size()) ||
            !countFits(image.entities.size()) ||
            !countFits(image.columns.size()))
        {
            return lux::cxx::unexpected(
                failure(EPersistenceError::LIMIT_EXCEEDED)
            );
        }
        try
        {
            std::vector<std::byte> bytes;
            persistence::detail::Writer writer(bytes);
            writer.writeBytes(kMagic.data(), kMagic.size());
            writer.writeUnsigned(kVersion);
            writer.writeUuid(image.id.value);
            writer.writeUnsigned(static_cast<std::uint32_t>(image.schemas.size()));
            writer.writeUnsigned(static_cast<std::uint32_t>(image.archetypes.size()));
            writer.writeUnsigned(static_cast<std::uint32_t>(image.entities.size()));
            writer.writeUnsigned(static_cast<std::uint32_t>(image.columns.size()));
            for (const auto& schema : image.schemas)
            {
                writer.writeUnsigned(schema.id.hash);
                writer.writeString(schema.id.name);
                writer.writeUnsigned(schema.version);
            }
            for (const auto& archetype : image.archetypes)
            {
                writer.writeUnsigned(
                    static_cast<std::uint32_t>(archetype.schema_indices.size())
                );
                for (const auto schema : archetype.schema_indices)
                {
                    writer.writeUnsigned(schema);
                }
                writer.writeUnsigned(
                    static_cast<std::uint32_t>(archetype.entity_ordinals.size())
                );
                for (const auto ordinal : archetype.entity_ordinals)
                {
                    writer.writeUnsigned(ordinal);
                }
            }
            for (const auto& entity : image.entities)
            {
                writer.writeUuid(entity.id.value);
                writer.writeUnsigned(entity.archetype);
            }
            for (const auto& column : image.columns)
            {
                writer.writeUnsigned(column.schema_index);
                writer.writeUnsigned<std::uint8_t>(column.fixed_width ? 1U : 0U);
                writer.writeUnsigned(column.fixed_stride);
                writer.writeUnsigned(
                    static_cast<std::uint32_t>(column.entity_ordinals.size())
                );
                for (const auto ordinal : column.entity_ordinals)
                {
                    writer.writeUnsigned(ordinal);
                }
                writer.writeUnsigned(
                    static_cast<std::uint32_t>(column.row_offsets.size())
                );
                for (const auto offset : column.row_offsets)
                {
                    writer.writeUnsigned(offset);
                }
                writer.writeUnsigned(
                    static_cast<std::uint64_t>(column.payload.size())
                );
                writer.writeBytes(column.payload.data(), column.payload.size());
            }
            return bytes;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                failure(EPersistenceError::ALLOCATION_FAILURE)
            );
        }
    }

    lux::cxx::expected<WorldSectionImage, PersistenceFailure>
    decodeWorldSection(
        std::span<const std::byte> bytes,
        WorldSectionLimits limits
    ) noexcept
    {
        if (bytes.size() > limits.max_image_bytes)
        {
            return lux::cxx::unexpected(
                failure(EPersistenceError::LIMIT_EXCEEDED)
            );
        }
        if (bytes.size() < kMagic.size() ||
            !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
        {
            return lux::cxx::unexpected(
                failure(EPersistenceError::INVALID_MAGIC)
            );
        }
        try
        {
            persistence::detail::Reader reader(bytes);
            std::array<std::byte, 4> magic{};
            reader.readBytes(magic.data(), magic.size());
            if (reader.readUnsigned<std::uint32_t>() != kVersion)
            {
                return lux::cxx::unexpected(
                    failure(EPersistenceError::UNSUPPORTED_VERSION)
                );
            }
            WorldSectionImage image;
            image.id.value = reader.readUuid();
            const auto schema_count = reader.readUnsigned<std::uint32_t>();
            const auto archetype_count = reader.readUnsigned<std::uint32_t>();
            const auto entity_count = reader.readUnsigned<std::uint32_t>();
            const auto column_count = reader.readUnsigned<std::uint32_t>();
            if (!reader.ok())
            {
                return lux::cxx::unexpected(
                    failure(EPersistenceError::TRUNCATED)
                );
            }
            if (schema_count > limits.max_schemas ||
                archetype_count > limits.max_archetypes ||
                entity_count > limits.max_entities ||
                column_count > limits.max_columns)
            {
                return lux::cxx::unexpected(
                    failure(EPersistenceError::LIMIT_EXCEEDED)
                );
            }
            image.schemas.reserve(schema_count);
            std::uint64_t name_bytes{};
            for (std::uint32_t index{}; index < schema_count; ++index)
            {
                const auto hash = reader.readUnsigned<std::uint64_t>();
                const auto name_size = reader.readUnsigned<std::uint32_t>();
                name_bytes += name_size;
                if (name_bytes > limits.max_name_bytes)
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::LIMIT_EXCEEDED)
                    );
                }
                const auto name_bytes_view = reader.readSpan(name_size);
                const auto version = reader.readUnsigned<std::uint32_t>();
                if (!reader.ok())
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::TRUNCATED)
                    );
                }
                image.schemas.push_back({
                    ComponentSchemaId{
                        hash,
                        std::string(
                            reinterpret_cast<const char*>(name_bytes_view.data()),
                            name_bytes_view.size()
                        )
                    },
                    version
                });
            }
            image.archetypes.reserve(archetype_count);
            for (std::uint32_t index{}; index < archetype_count; ++index)
            {
                WorldArchetype archetype;
                const auto signature_count = reader.readUnsigned<std::uint32_t>();
                if (signature_count > schema_count)
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::INVALID_ARCHETYPE)
                    );
                }
                archetype.schema_indices.reserve(signature_count);
                for (std::uint32_t item{}; item < signature_count; ++item)
                {
                    archetype.schema_indices.push_back(
                        reader.readUnsigned<std::uint32_t>()
                    );
                }
                const auto members = reader.readUnsigned<std::uint32_t>();
                if (members > entity_count)
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::INVALID_ARCHETYPE)
                    );
                }
                archetype.entity_ordinals.reserve(members);
                for (std::uint32_t item{}; item < members; ++item)
                {
                    archetype.entity_ordinals.push_back(
                        reader.readUnsigned<std::uint32_t>()
                    );
                }
                image.archetypes.push_back(std::move(archetype));
            }
            image.entities.reserve(entity_count);
            for (std::uint32_t index{}; index < entity_count; ++index)
            {
                image.entities.push_back({
                    PersistentEntityId{reader.readUuid()},
                    reader.readUnsigned<std::uint32_t>()
                });
            }
            image.columns.reserve(column_count);
            std::uint64_t total_rows{};
            std::uint64_t total_payload{};
            for (std::uint32_t index{}; index < column_count; ++index)
            {
                WorldComponentColumn column;
                column.schema_index = reader.readUnsigned<std::uint32_t>();
                const auto fixed = reader.readUnsigned<std::uint8_t>();
                if (fixed > 1U)
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::INVALID_COLUMN)
                    );
                }
                column.fixed_width = fixed != 0U;
                column.fixed_stride = reader.readUnsigned<std::uint64_t>();
                const auto rows = reader.readUnsigned<std::uint32_t>();
                total_rows += rows;
                if (total_rows > limits.max_rows)
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::LIMIT_EXCEEDED)
                    );
                }
                column.entity_ordinals.reserve(rows);
                for (std::uint32_t row{}; row < rows; ++row)
                {
                    column.entity_ordinals.push_back(
                        reader.readUnsigned<std::uint32_t>()
                    );
                }
                const auto offset_count = reader.readUnsigned<std::uint32_t>();
                if (offset_count > rows + 1U)
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::INVALID_PAYLOAD)
                    );
                }
                column.row_offsets.reserve(offset_count);
                for (std::uint32_t offset{}; offset < offset_count; ++offset)
                {
                    column.row_offsets.push_back(
                        reader.readUnsigned<std::uint64_t>()
                    );
                }
                const auto payload_size = reader.readUnsigned<std::uint64_t>();
                total_payload += payload_size;
                if (payload_size > std::numeric_limits<std::size_t>::max() ||
                    total_payload > limits.max_payload_bytes)
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::LIMIT_EXCEEDED)
                    );
                }
                const auto payload = reader.readSpan(
                    static_cast<std::size_t>(payload_size)
                );
                if (!reader.ok())
                {
                    return lux::cxx::unexpected(
                        failure(EPersistenceError::TRUNCATED)
                    );
                }
                column.payload.assign(payload.begin(), payload.end());
                image.columns.push_back(std::move(column));
            }
            if (!reader.ok())
            {
                return lux::cxx::unexpected(
                    failure(EPersistenceError::TRUNCATED)
                );
            }
            if (!reader.eof())
            {
                return lux::cxx::unexpected(
                    failure(EPersistenceError::INVALID_PAYLOAD)
                );
            }
            if (auto valid = validateImage(image, limits); !valid)
            {
                return lux::cxx::unexpected(valid.error());
            }
            return image;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                failure(EPersistenceError::ALLOCATION_FAILURE)
            );
        }
    }
} // namespace lux::ecs
