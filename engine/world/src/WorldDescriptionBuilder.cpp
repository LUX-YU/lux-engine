#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/detail/WorldFailureInjection.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <unordered_map>
#include <utility>

namespace lux::world
{
    struct WorldDescriptionBuilder::Impl final
    {
        struct PendingData final
        {
            WorldDataSchemaId schema;
            std::uint32_t version{};
            std::vector<std::byte> payload;
        };

        struct PendingObject final
        {
            WorldObjectId id;
            std::vector<PendingData> data;
        };

        std::vector<PendingObject> objects;
        std::unordered_map<
            WorldObjectId,
            std::size_t,
            WorldObjectIdHash> object_index;
    };

    namespace
    {
        [[nodiscard]] WorldDescriptionFailure failure(
            EWorldDescriptionError code,
            WorldObjectId object = {},
            WorldDataSchemaId schema = {}
        ) noexcept
        {
            return {code, object, std::move(schema)};
        }

        template <class PendingObject>
        [[nodiscard]] auto findData(
            PendingObject& object,
            const WorldDataSchemaId& schema
        ) noexcept
        {
            return std::find_if(
                object.data.begin(),
                object.data.end(),
                [&](const auto& candidate) noexcept
                {
                    return candidate.schema == schema;
                }
            );
        }
    }

    WorldDescriptionBuilder::WorldDescriptionBuilder()
        : impl_(std::make_unique<Impl>())
    {
    }

    WorldDescriptionBuilder::~WorldDescriptionBuilder() = default;
    WorldDescriptionBuilder::WorldDescriptionBuilder(
        WorldDescriptionBuilder&&
    ) noexcept = default;
    WorldDescriptionBuilder& WorldDescriptionBuilder::operator=(
        WorldDescriptionBuilder&&
    ) noexcept = default;

    lux::cxx::expected<void, WorldDescriptionFailure>
    WorldDescriptionBuilder::addObject(WorldObjectId id) noexcept
    {
        if (!id.valid())
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::INVALID_OBJECT_ID,
                id
            ));
        if (impl_->object_index.contains(id))
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::DUPLICATE_OBJECT_ID,
                id
            ));
        if (detail::consumeWorldFailureForTest(
                detail::EWorldFailurePoint::DESCRIPTION_MUTATION_ALLOCATION
            ))
        {
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::ALLOCATION_FAILURE,
                id
            ));
        }
        try
        {
            const std::size_t index = impl_->objects.size();
            impl_->objects.push_back({id, {}});
            try
            {
                impl_->object_index.emplace(id, index);
            }
            catch (...)
            {
                impl_->objects.pop_back();
                throw;
            }
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::ALLOCATION_FAILURE,
                id
            ));
        }
    }

    lux::cxx::expected<void, WorldDescriptionFailure>
    WorldDescriptionBuilder::eraseObject(WorldObjectId id) noexcept
    {
        const auto found = impl_->object_index.find(id);
        if (found == impl_->object_index.end())
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::OBJECT_NOT_FOUND,
                id
            ));

        const std::size_t index = found->second;
        const std::size_t last = impl_->objects.size() - 1U;
        if (index != last)
        {
            impl_->objects[index] = std::move(impl_->objects[last]);
            impl_->object_index[impl_->objects[index].id] = index;
        }
        impl_->objects.pop_back();
        impl_->object_index.erase(found);
        return {};
    }

    lux::cxx::expected<void, WorldDescriptionFailure>
    WorldDescriptionBuilder::addData(
        WorldObjectId object,
        WorldDataSchemaId schema,
        std::uint32_t version,
        std::span<const std::byte> payload
    ) noexcept
    {
        if (!schema.valid())
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::INVALID_SCHEMA_ID,
                object,
                std::move(schema)
            ));
        if (version == 0U)
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::INVALID_SCHEMA_VERSION,
                object,
                std::move(schema)
            ));
        const auto object_it = impl_->object_index.find(object);
        if (object_it == impl_->object_index.end())
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::OBJECT_NOT_FOUND,
                object,
                std::move(schema)
            ));
        auto& target = impl_->objects[object_it->second];
        if (findData(target, schema) != target.data.end())
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::DUPLICATE_OBJECT_DATA,
                object,
                std::move(schema)
            ));
        if (detail::consumeWorldFailureForTest(
                detail::EWorldFailurePoint::DESCRIPTION_MUTATION_ALLOCATION
            ))
        {
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::ALLOCATION_FAILURE,
                object,
                std::move(schema)
            ));
        }
        try
        {
            target.data.push_back({
                std::move(schema),
                version,
                std::vector<std::byte>(payload.begin(), payload.end())
            });
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::ALLOCATION_FAILURE,
                object,
                std::move(schema)
            ));
        }
    }

    lux::cxx::expected<void, WorldDescriptionFailure>
    WorldDescriptionBuilder::setData(
        WorldObjectId object,
        WorldDataSchemaId schema,
        std::uint32_t version,
        std::span<const std::byte> payload
    ) noexcept
    {
        if (!schema.valid())
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::INVALID_SCHEMA_ID,
                object,
                std::move(schema)
            ));
        if (version == 0U)
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::INVALID_SCHEMA_VERSION,
                object,
                std::move(schema)
            ));
        const auto object_it = impl_->object_index.find(object);
        if (object_it == impl_->object_index.end())
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::OBJECT_NOT_FOUND,
                object,
                std::move(schema)
            ));
        auto& target = impl_->objects[object_it->second];
        auto data_it = findData(target, schema);
        if (detail::consumeWorldFailureForTest(
                detail::EWorldFailurePoint::DESCRIPTION_MUTATION_ALLOCATION
            ))
        {
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::ALLOCATION_FAILURE,
                object,
                std::move(schema)
            ));
        }
        try
        {
            std::vector<std::byte> replacement(payload.begin(), payload.end());
            if (data_it == target.data.end())
            {
                target.data.push_back({
                    std::move(schema),
                    version,
                    std::move(replacement)
                });
            }
            else
            {
                data_it->version = version;
                data_it->payload = std::move(replacement);
            }
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::ALLOCATION_FAILURE,
                object,
                std::move(schema)
            ));
        }
    }

    lux::cxx::expected<void, WorldDescriptionFailure>
    WorldDescriptionBuilder::eraseData(
        WorldObjectId object,
        const WorldDataSchemaId& schema
    ) noexcept
    {
        const auto object_it = impl_->object_index.find(object);
        if (object_it == impl_->object_index.end())
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::OBJECT_NOT_FOUND,
                object,
                schema
            ));
        auto& target = impl_->objects[object_it->second];
        const auto data_it = findData(target, schema);
        if (data_it == target.data.end())
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::DATA_NOT_FOUND,
                object,
                schema
            ));
        target.data.erase(data_it);
        return {};
    }

    void WorldDescriptionBuilder::clear() noexcept
    {
        impl_->objects.clear();
        impl_->object_index.clear();
    }

    lux::cxx::expected<WorldDescription, WorldDescriptionFailure>
    WorldDescriptionBuilder::build() && noexcept
    {
        if (detail::consumeWorldFailureForTest(
                detail::EWorldFailurePoint::DESCRIPTION_BUILD_ALLOCATION
            ))
        {
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::ALLOCATION_FAILURE
            ));
        }
        if (detail::consumeWorldFailureForTest(
                detail::EWorldFailurePoint::DESCRIPTION_BUILD_SIZE_OVERFLOW
            ))
        {
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::SIZE_OVERFLOW
            ));
        }
        try
        {
            WorldDescription result;
            std::sort(
                impl_->objects.begin(),
                impl_->objects.end(),
                [](const Impl::PendingObject& left,
                   const Impl::PendingObject& right) noexcept
                {
                    return WorldObjectIdLess{}(left.id, right.id);
                }
            );

            std::vector<WorldDataSchemaId> all_schemas;
            std::size_t total_data{};
            std::size_t total_payload{};
            for (auto& object : impl_->objects)
            {
                if (object.data.size() >
                    std::numeric_limits<std::size_t>::max() - total_data)
                {
                    return lux::cxx::unexpected(failure(
                        EWorldDescriptionError::SIZE_OVERFLOW,
                        object.id
                    ));
                }
                total_data += object.data.size();
                for (auto& data : object.data)
                {
                    if (data.payload.size() >
                        std::numeric_limits<std::size_t>::max() - total_payload)
                    {
                        return lux::cxx::unexpected(failure(
                            EWorldDescriptionError::SIZE_OVERFLOW,
                            object.id,
                            data.schema
                        ));
                    }
                    total_payload += data.payload.size();
                    all_schemas.push_back(data.schema);
                }
            }

            std::sort(
                all_schemas.begin(),
                all_schemas.end(),
                WorldDataSchemaIdLess{}
            );
            for (std::size_t index = 1U; index < all_schemas.size(); ++index)
            {
                const auto& previous = all_schemas[index - 1U];
                const auto& current = all_schemas[index];
                if (previous.hash == current.hash && previous.name != current.name)
                {
                    return lux::cxx::unexpected(failure(
                        EWorldDescriptionError::SCHEMA_HASH_COLLISION,
                        {},
                        current
                    ));
                }
            }
            all_schemas.erase(
                std::unique(all_schemas.begin(), all_schemas.end()),
                all_schemas.end()
            );

            result.schemas_ = std::move(all_schemas);
            result.objects_.reserve(impl_->objects.size());
            result.data_.reserve(total_data);
            result.payload_.reserve(total_payload);

            for (auto& object : impl_->objects)
            {
                std::sort(
                    object.data.begin(),
                    object.data.end(),
                    [](const Impl::PendingData& left,
                       const Impl::PendingData& right) noexcept
                    {
                        return WorldDataSchemaIdLess{}(
                            left.schema,
                            right.schema
                        );
                    }
                );

                WorldDescription::ObjectRecord object_record;
                object_record.id = object.id;
                object_record.first_data = result.data_.size();
                object_record.data_count = object.data.size();
                for (auto& source : object.data)
                {
                    const auto schema_it = std::lower_bound(
                        result.schemas_.begin(),
                        result.schemas_.end(),
                        source.schema,
                        WorldDataSchemaIdLess{}
                    );
                    WorldDescription::DataRecord data_record;
                    data_record.schema_ordinal = static_cast<std::size_t>(
                        std::distance(result.schemas_.begin(), schema_it)
                    );
                    data_record.version = source.version;
                    data_record.payload_offset = result.payload_.size();
                    data_record.payload_size = source.payload.size();
                    result.data_.push_back(data_record);
                    result.payload_.insert(
                        result.payload_.end(),
                        source.payload.begin(),
                        source.payload.end()
                    );
                }
                result.objects_.push_back(object_record);
            }

            impl_->objects.clear();
            impl_->object_index.clear();
            return result;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::ALLOCATION_FAILURE
            ));
        }
        catch (...)
        {
            return lux::cxx::unexpected(failure(
                EWorldDescriptionError::ALLOCATION_FAILURE
            ));
        }
    }
}
