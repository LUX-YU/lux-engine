#pragma once

#include <lux/engine/ecs/ComponentSchema.hpp>
#include <lux/engine/ecs/persistence/EcsBinaryIO.hpp>
#include <lux/engine/ecs/persistence/detail/ComponentStorageAccess.hpp>
#include <lux/engine/serialization/Traits.hpp>
#include <lux/engine/ecs/persistence_contract/visibility.h>

#include <cstddef>
#include <memory>
#include <span>
#include <type_traits>

namespace lux::ecs
{
    [[nodiscard]] LUX_ENGINE_ECS_PERSISTENCE_CONTRACT_PUBLIC
    std::uint32_t persistenceContractVersion() noexcept;

    using EncodeComponentColumnFn = lux::serialization::SerializationResult (*)(
        const World&,
        std::span<const Entity>,
        EcsBinaryWriter&
    ) noexcept;

    using DecodeComponentColumnFn = lux::serialization::SerializationResult (*)(
        WorldEdit&,
        std::span<const Entity>,
        EcsBinaryReader&
    ) noexcept;

    struct ComponentPersistenceBinding final
    {
        [[nodiscard]] constexpr const ComponentSchema& schema() const noexcept
        {
            return *schema_;
        }

        [[nodiscard]] constexpr EncodeComponentColumnFn encode() const noexcept
        {
            return encode_;
        }

        [[nodiscard]] constexpr DecodeComponentColumnFn decode() const noexcept
        {
            return decode_;
        }

    private:
        constexpr ComponentPersistenceBinding(
            const ComponentSchema& schema,
            EncodeComponentColumnFn encode,
            DecodeComponentColumnFn decode
        ) noexcept
            : schema_(&schema), encode_(encode), decode_(decode)
        {
        }

        template <class Component>
        friend constexpr ComponentPersistenceBinding
        bindComponentPersistence(const ComponentSchema&) noexcept;

        const ComponentSchema* schema_{};
        EncodeComponentColumnFn encode_{};
        DecodeComponentColumnFn decode_{};
    };

    struct ComponentPersistenceContribution final
    {
        std::shared_ptr<const void> code_lifetime;
        std::span<const ComponentPersistenceBinding> bindings;
    };

    namespace detail
    {
#if defined(LUX_ECS_PERSISTENCE_TESTING)
        struct ColumnThunkTestStats final
        {
            static inline std::size_t encode_calls{};
            static inline std::size_t decode_calls{};
            static inline std::size_t storage_lookups{};

            static void reset() noexcept
            {
                encode_calls = 0U;
                decode_calls = 0U;
                storage_lookups = 0U;
            }
        };
#endif

        template <class Component>
        [[nodiscard]] lux::serialization::SerializationResult encodeColumn(
            const World& world,
            std::span<const Entity> entities,
            EcsBinaryWriter& writer
        ) noexcept
        {
#if defined(LUX_ECS_PERSISTENCE_TESTING)
            ++ColumnThunkTestStats::encode_calls;
            ++ColumnThunkTestStats::storage_lookups;
#endif
            const auto* storage = PersistenceStorageAccess::storage<Component>(world);
            if (storage == nullptr)
            {
                return lux::cxx::unexpected<lux::serialization::SerializationFailure>(lux::serialization::SerializationFailure{
                    lux::serialization::ESerializationError::INVALID_VALUE,
                    writer.offset()
                });
            }
            for (const Entity entity : entities)
            {
                auto result = writer.beginRow();
                if (!result)
                {
                    return result;
                }
                result = lux::serialization::write(
                    writer,
                    storage->get(entity)
                );
                if (!result)
                {
                    return result;
                }
            }
            return writer.endColumn();
        }

        template <class Component>
        [[nodiscard]] lux::serialization::SerializationResult decodeColumn(
            WorldEdit& edit,
            std::span<const Entity> entities,
            EcsBinaryReader& reader
        ) noexcept
        {
#if defined(LUX_ECS_PERSISTENCE_TESTING)
            ++ColumnThunkTestStats::decode_calls;
#endif
            static_assert(std::default_initializable<Component>);
            static_assert(std::is_nothrow_move_constructible_v<Component>);
            try
            {
                edit.reserve<Component>(entities.size());
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected<lux::serialization::SerializationFailure>(lux::serialization::SerializationFailure{
                    lux::serialization::ESerializationError::ALLOCATION_FAILURE,
                    reader.offset()
                });
            }
            for (std::size_t index{}; index < entities.size(); ++index)
            {
                auto result = reader.beginRow(index);
                if (!result)
                {
                    return result;
                }
                Component value{};
                result = lux::serialization::read(reader, value);
                if (result)
                {
                    result = reader.endRow();
                }
                if (!result)
                {
                    return result;
                }
                try
                {
                    edit.emplace<Component>(entities[index], std::move(value));
                }
                catch (const std::bad_alloc&)
                {
                    return lux::cxx::unexpected<lux::serialization::SerializationFailure>(lux::serialization::SerializationFailure{
                        lux::serialization::ESerializationError::ALLOCATION_FAILURE,
                        reader.offset()
                    });
                }
            }
            return {};
        }
    } // namespace detail

    template <class Component>
    [[nodiscard]] constexpr ComponentPersistenceBinding
    bindComponentPersistence(const ComponentSchema& schema) noexcept
    {
        static_assert(lux::meta::HasTypeStaticInfo<Component>);
        static_assert(std::default_initializable<Component>);
        static_assert(std::is_nothrow_move_constructible_v<Component>);
        return ComponentPersistenceBinding{
            schema,
            &detail::encodeColumn<Component>,
            &detail::decodeColumn<Component>
        };
    }
} // namespace lux::ecs
