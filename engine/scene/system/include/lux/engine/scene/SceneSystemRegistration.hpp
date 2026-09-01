#pragma once

#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>
#include <lux/engine/system/SystemTypeDescription.hpp>

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <concepts>
#include <cstdint>
#include <span>
#include <string_view>

namespace lux::scene
{
    class SceneBuilder;

    enum class EComponentObservation : std::uint8_t
    {
        NONE = 0,
        CONSTRUCT = 1U << 0U,
        UPDATE = 1U << 1U,
        DESTROY = 1U << 2U,
    };

    [[nodiscard]] constexpr EComponentObservation operator|(
        EComponentObservation left,
        EComponentObservation right
    ) noexcept
    {
        return static_cast<EComponentObservation>(
            static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right)
        );
    }

    struct ComponentObservationSpec final
    {
        lux::cxx::TypeToken component;
        std::uint8_t events{};
    };

    struct SceneSystemRequirementSpec final
    {
        std::string_view name;
        std::string_view capability;
        lux::cxx::TypeToken expected_type;
        bool optional{};
    };

    enum class ESceneConnectionOwner : std::uint8_t
    {
        SELF,
        REQUIREMENT,
    };

    struct SceneObjectEndpointRef final
    {
        ESceneConnectionOwner owner{ESceneConnectionOwner::SELF};
        std::string_view requirement;
        std::string_view member;
    };

    struct SceneSystemConnectionSpec final
    {
        SceneObjectEndpointRef signal;
        SceneObjectEndpointRef method;
        object::EDelivery delivery{object::EDelivery::AUTO};
    };

    enum class ESceneSystemBuildError : std::uint8_t
    {
        INVALID_DESCRIPTION,
        UNKNOWN_SYSTEM_TYPE,
        VERSION_MISMATCH,
        DUPLICATE_SYSTEM,
        CONSTRUCTION_FAILURE,
        CONFIGURATION_DECODE_FAILURE,
        ALLOCATION_FAILURE,
        DEPENDENCY_CYCLE,
        UNDECLARED_CONSTRUCTOR_DEPENDENCY,
        MISSING_REQUIREMENT,
        AMBIGUOUS_REQUIREMENT,
        INVALID_REQUIREMENT_BINDING,
        REQUIREMENT_TYPE_MISMATCH,
        CONNECTION_FAILURE,
        EXTERNAL_OPERATION_FAILURE,
        DUPLICATE_STABLE_POINT_TASK,
        DUPLICATE_PRESENTATION_TASK,
    };

    struct SceneSystemBuildFailure final
    {
        ESceneSystemBuildError code{ESceneSystemBuildError::INVALID_DESCRIPTION};
        system::SystemInstanceId system{};
        system::SystemInstanceId related{};
        std::uint64_t subject_hash{};
        lux::serialization::SerializationFailure configuration{};
    };

    using InstallSceneSystemFn = lux::cxx::expected<void, SceneSystemBuildFailure> (*)(
        SceneBuilder& builder,
        SceneSystemView description
    ) noexcept;
    using ProjectSceneSystemObjectFn = object::LuxObject* (*)(void* object) noexcept;

    template <class Type>
    [[nodiscard]] consteval ProjectSceneSystemObjectFn sceneSystemObjectProjection() noexcept
    {
        if constexpr (std::derived_from<Type, object::LuxObject>)
        {
            return +[](void* value) noexcept -> object::LuxObject* {
                return static_cast<object::LuxObject*>(static_cast<Type*>(value));
            };
        }
        return nullptr;
    }

    struct SceneSystemRegistration final
    {
        system::SystemTypeId type;
        lux::cxx::TypeToken cpp_type;
        const system::SystemTypeDescription* description{};
        lux::serialization::PortableValueCodec configuration{};
        std::span<const ComponentObservationSpec> observations;
        std::span<const SceneSystemRequirementSpec> requirements;
        std::span<const SceneSystemConnectionSpec> connections;
        ProjectSceneSystemObjectFn project_object{};
        InstallSceneSystemFn install{};
    };
} // namespace lux::scene
