#pragma once

#include <lux/engine/serialization/BinaryReader.hpp>
#include <lux/engine/serialization/Serialization.hpp>
#include <lux/engine/serialization/external_support/Eigen.hpp>
#include <lux/engine/simulation/ecs/ComponentSchema.hpp>

#include <array>
#include <concepts>
#include <new>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::simulation::ecs
{
    namespace detail
    {
        template <class Type>
        concept SemanticArchiveOnly = requires(Type value) { luxBinarySemanticArchiveOnly(value); };

        template <class Type>
        struct IsVector final : std::false_type
        {
        };

        template <class Type, class Allocator>
        struct IsVector<std::vector<Type, Allocator>> final : std::true_type
        {
            using Value = Type;
        };

        template <class Type>
        struct IsOptional final : std::false_type
        {
        };

        template <class Type>
        struct IsOptional<std::optional<Type>> final : std::true_type
        {
            using Value = Type;
        };

        template <class Type>
        struct IsArray final : std::false_type
        {
        };

        template <class Type, std::size_t Size>
        struct IsArray<std::array<Type, Size>> final : std::true_type
        {
            using Value = Type;
        };

        template <class Type>
        struct IsPair final : std::false_type
        {
        };

        template <class First, class Second>
        struct IsPair<std::pair<First, Second>> final : std::true_type
        {
            using FirstType = First;
            using SecondType = Second;
        };

        template <class Type>
        consteval bool directMaterializableValue();

        template <class Type, std::size_t Index = 0U>
        consteval bool directMaterializableFields()
        {
            using Fields = std::remove_cvref_t<decltype(lux::meta::TypeStaticInfo<Type>::fields)>;
            if constexpr (Index == std::tuple_size_v<Fields>)
            {
                return true;
            }
            else
            {
                using Descriptor = std::tuple_element_t<Index, Fields>;
                using Member = std::remove_cvref_t<decltype(std::declval<Type>().*Descriptor::pointer)>;
                return directMaterializableValue<Member>() &&
                       directMaterializableFields<Type, Index + 1U>();
            }
        }

        template <class Type, std::size_t Index = 0U>
        consteval bool directMaterializableTuple()
        {
            if constexpr (Index == std::tuple_size_v<Type>)
            {
                return true;
            }
            else
            {
                return directMaterializableValue<std::tuple_element_t<Index, Type>>() &&
                       directMaterializableTuple<Type, Index + 1U>();
            }
        }

        template <class Type>
        consteval bool directMaterializableValue()
        {
            using Value = std::remove_cvref_t<Type>;
            if constexpr (SemanticArchiveOnly<Value>)
                return false;
            else if constexpr (std::is_arithmetic_v<Value> || std::is_enum_v<Value> ||
                               std::same_as<Value, std::string>)
                return true;
            else if constexpr (lux::serialization::HasSerializerDefinition<Value>)
                return true;
            else if constexpr (IsVector<Value>::value)
                return directMaterializableValue<typename IsVector<Value>::Value>();
            else if constexpr (IsOptional<Value>::value)
                return directMaterializableValue<typename IsOptional<Value>::Value>();
            else if constexpr (IsArray<Value>::value)
                return directMaterializableValue<typename IsArray<Value>::Value>();
            else if constexpr (IsPair<Value>::value)
            {
                return directMaterializableValue<typename IsPair<Value>::FirstType>() &&
                       directMaterializableValue<typename IsPair<Value>::SecondType>();
            }
            else if constexpr (lux::meta::HasTypeStaticInfo<Value>)
                return directMaterializableFields<Value>();
            else if constexpr (requires { std::tuple_size<Value>::value; })
                return directMaterializableTuple<Value>();
            else
                return false;
        }

        template <class Component>
        consteval bool directMaterializableComponent()
        {
            return std::is_default_constructible_v<Component> &&
                   std::is_move_constructible_v<Component> &&
                   std::is_nothrow_destructible_v<Component> &&
                   directMaterializableValue<Component>();
        }

        [[nodiscard]] inline ComponentDecodeFailure decodeFailure(
            EComponentDecodeError code,
            std::size_t offset = 0U
        ) noexcept
        {
            return ComponentDecodeFailure{code, offset};
        }

        template <class Component, std::uint32_t Version>
        [[nodiscard]] lux::cxx::expected<void, ComponentDecodeFailure> decodeEmplaceComponent(
            Registry& registry,
            Entity entity,
            std::uint32_t encoded_schema_version,
            std::span<const std::byte> encoded_payload
        ) noexcept
        {
            if (!registry.valid(entity))
            {
                return lux::cxx::unexpected(
                    decodeFailure(EComponentDecodeError::INVALID_ENTITY)
                );
            }
            if (encoded_schema_version != Version)
            {
                return lux::cxx::unexpected(
                    decodeFailure(EComponentDecodeError::UNSUPPORTED_VERSION)
                );
            }

            lux::serialization::BinaryReader reader(encoded_payload);
            const lux::serialization::SerializationBudget budget{
                encoded_payload.size(),
                encoded_payload.size(),
                64U
            };
            auto decoded = lux::serialization::read<Component>(reader, budget);
            if (!decoded)
            {
                const auto code = decoded.error().code == lux::serialization::ESerializationError::UNSUPPORTED_TYPE
                    ? EComponentDecodeError::UNSUPPORTED_TYPE
                    : EComponentDecodeError::MALFORMED_PAYLOAD;
                return lux::cxx::unexpected(decodeFailure(code, decoded.error().offset));
            }
            if (reader.remaining() != 0U)
            {
                return lux::cxx::unexpected(
                    decodeFailure(EComponentDecodeError::MALFORMED_PAYLOAD, reader.offset())
                );
            }

            try
            {
                registry.emplace_or_replace<Component>(entity, std::move(*decoded));
                return {};
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    decodeFailure(EComponentDecodeError::ALLOCATION_FAILURE)
                );
            }
            catch (...)
            {
                return lux::cxx::unexpected(
                    decodeFailure(EComponentDecodeError::COMPONENT_CONSTRUCTION_FAILURE)
                );
            }
        }
    } // namespace detail

    template <class Component, std::uint32_t Version>
    [[nodiscard]] consteval DecodeEmplaceComponentFn directComponentDecodeEmplace() noexcept
    {
        if constexpr (detail::directMaterializableComponent<Component>())
            return &detail::decodeEmplaceComponent<Component, Version>;
        else
            return nullptr;
    }
} // namespace lux::simulation::ecs
