#pragma once

#include <lux/engine/serialization/BinaryReader.hpp>
#include <lux/engine/serialization/Serialization.hpp>
#include <lux/engine/serialization/external_support/Eigen.hpp>
#include <lux/engine/simulation/ecs/ComponentSchema.hpp>
#include <lux/engine/simulation/ecs/WorldComponentArchive.hpp>

#include <array>
#include <concepts>
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
            if constexpr (std::same_as<Value, Entity>)
                return true;
            else if constexpr (SemanticArchiveOnly<Value>)
                return false;
            else if constexpr (std::is_arithmetic_v<Value> || std::is_enum_v<Value> ||
                               std::same_as<Value, std::string>)
                return true;
            else if constexpr (lux::serialization::HasSerializerDefinition<Value>)
                return true;
            else if constexpr (std::is_bounded_array_v<Value>)
            {
                return directMaterializableValue<std::remove_extent_t<Value>>();
            }
            else if constexpr (lux::serialization::SequenceContainer<Value> || lux::serialization::SetContainer<Value>)
            {
                return directMaterializableValue<typename Value::value_type>();
            }
            else if constexpr (lux::serialization::MapContainer<Value>)
            {
                return directMaterializableValue<typename Value::key_type>() &&
                    directMaterializableValue<typename Value::mapped_type>();
            }
            else if constexpr (std::same_as<Value, std::monostate>)
            {
                return true;
            }
            else if constexpr (lux::serialization::VariantValue<Value>)
            {
                return []<std::size_t... Index>(std::index_sequence<Index...>)
                {
                    return (directMaterializableValue<std::variant_alternative_t<Index, Value>>() && ...);
                }(std::make_index_sequence<std::variant_size_v<Value>>{});
            }
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
        [[nodiscard]] lux::cxx::expected<Component, ComponentDecodeFailure> decodeComponent(
            std::uint32_t encoded_schema_version, std::span<const std::byte> encoded_payload,
            WorldComponentArchive<lux::serialization::BinaryReader> &reader)
        {
            if (encoded_schema_version != Version)
            {
                return lux::cxx::unexpected(
                    decodeFailure(EComponentDecodeError::UNSUPPORTED_VERSION)
                );
            }

            const lux::serialization::SerializationBudget budget{
                encoded_payload.size(),
                encoded_payload.size(),
                64U
            };
            auto decoded = lux::serialization::read<Component>(reader, budget);
            if (!decoded)
            {
                if (reader.unresolvedReference().valid())
                    return lux::cxx::unexpected(ComponentDecodeFailure{EComponentDecodeError::UNRESOLVED_REFERENCE,
                        decoded.error().offset, reader.unresolvedReference()});
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

            return std::move(*decoded);
        }

        template <class Component, std::uint32_t Version>
        [[nodiscard]] lux::cxx::expected<void, ComponentDecodeFailure> decodeEmplaceComponent(
            Registry &registry, const WorldEntityMap &identities, Entity entity, std::uint32_t encoded_schema_version,
            std::span<const std::byte> encoded_payload) noexcept
        {
            if (!registry.valid(entity))
            {
                return lux::cxx::unexpected(decodeFailure(EComponentDecodeError::INVALID_ENTITY));
            }
            lux::serialization::BinaryReader binary(encoded_payload);
            WorldComponentArchive reader(binary, identities, registry);
            auto decoded = decodeComponent<Component, Version>(encoded_schema_version, encoded_payload, reader);
            if (!decoded)
            {
                return lux::cxx::unexpected(decoded.error());
            }
            registry.emplace_or_replace<Component>(entity, std::move(*decoded));
            return {};
        }

        template <class Component, std::uint32_t Version>
        [[nodiscard]] lux::cxx::expected<DecodedComponent, ComponentDecodeFailure> decodeComponentValue(
            std::uint32_t encoded_schema_version, std::span<const std::byte> encoded_payload,
            ComponentEntityResolver resolver, std::shared_ptr<const void> code)
        {
            lux::serialization::BinaryReader binary(encoded_payload);
            WorldComponentArchive reader(binary, resolver);
            auto decoded = decodeComponent<Component, Version>(encoded_schema_version, encoded_payload, reader);
            if (!decoded)
            {
                return lux::cxx::unexpected(decoded.error());
            }
            return DecodedComponent::own(std::move(*decoded), std::move(code));
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

    template <class Component, std::uint32_t Version>
    [[nodiscard]] consteval DecodeComponentValueFn directComponentDecodeValue() noexcept
    {
        if constexpr (detail::directMaterializableComponent<Component>() &&
                      componentInstallHasNoBusinessFailure<Component>)
        {
            return &detail::decodeComponentValue<Component, Version>;
        }
        else
        {
            return nullptr;
        }
    }

    template <class Component> [[nodiscard]] consteval CaptureComponentValueFn directComponentValueCapture() noexcept
    {
        if constexpr (detail::directMaterializableComponent<Component>() && std::is_copy_constructible_v<Component>)
        {
            return +[](const void *value, std::shared_ptr<const void> code) -> ComponentCapture
            {
                struct Owned final
                {
                    std::shared_ptr<const void> code;
                    Component value;
                };
                auto owned = std::make_shared<const Owned>(std::move(code), *static_cast<const Component *>(value));
                auto captured = std::shared_ptr<const void>(owned, &owned->value);
                return ComponentCapture{std::move(captured),
                    +[](const void* capture, const WorldEntityMap& identities, std::size_t limit) -> ComponentEncodeResult
                    {
                        std::vector<std::byte> bytes;
                        serialization::BinaryWriter binary(bytes);
                        WorldComponentArchive writer(binary, identities);
                        const auto encoded = serialization::write(writer, *static_cast<const Component*>(capture),
                            serialization::SerializationBudget{limit, limit, 64});
                        if (!encoded)
                        {
                            return lux::cxx::unexpected(encoded.error());
                        }
                        return bytes;
                    }};
            };
        }
        else
        {
            return nullptr;
        }
    }

    template <class Component> [[nodiscard]] consteval CaptureComponentFn directComponentCapture() noexcept
    {
        if constexpr (detail::directMaterializableComponent<Component>() && std::is_copy_constructible_v<Component>)
        {
            return +[](const Registry &registry, Entity entity,
                       std::shared_ptr<const void> code) -> lux::cxx::expected<ComponentCapture, ComponentDecodeFailure>
            {
                const auto *value = registry.try_get<Component>(entity);
                if (!value)
                {
                    return lux::cxx::unexpected(ComponentDecodeFailure{EComponentDecodeError::INVALID_ENTITY});
                }
                constexpr auto capture = directComponentValueCapture<Component>();
                return capture(value, std::move(code));
            };
        }
        else
        {
            return nullptr;
        }
    }
} // namespace lux::simulation::ecs
