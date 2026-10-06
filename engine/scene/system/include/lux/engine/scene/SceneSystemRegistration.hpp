#pragma once

#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>
#include <lux/engine/system/SystemTypeDescription.hpp>

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <concepts>
#include <cstdint>
#include <lux/engine/error/Error.hpp>
#include <memory>
#include <span>
#include <string_view>

namespace lux::scene
{
    class SceneSystemInstaller;

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
        return static_cast<EComponentObservation>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
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
        EXTERNAL_OPERATION_FAILURE,
        DUPLICATE_STABLE_POINT_TASK,
    };

    struct SceneSystemBuildFailure final
    {
        ESceneSystemBuildError code{ESceneSystemBuildError::INVALID_DESCRIPTION};
        system::SystemInstanceId system{};
        system::SystemInstanceId related{};
        std::uint64_t subject_hash{};
        lux::serialization::SerializationFailure configuration{};
        error::Error cause;
    };

    using InstallSceneSystemFn = lux::cxx::expected<void, SceneSystemBuildFailure> (*)(
        SceneSystemInstaller& builder,
        SceneSystemDescription description
    ) noexcept;
    struct SceneSystemCapabilityProjection final
    {
        lux::cxx::TypeToken type;
        void* (*project)(void*) noexcept {};
    };

    template <class Concrete, class Capability>
        requires std::derived_from<Concrete, Capability>
    [[nodiscard]] consteval SceneSystemCapabilityProjection sceneSystemCapabilityProjection() noexcept
    {
        return {lux::cxx::typeToken<Capability>(), +[](void* object) noexcept -> void* {
                    return static_cast<Capability*>(static_cast<Concrete*>(object));
                }};
    }

    struct SceneSystemRegistration final
    {
        system::SystemTypeId type;
        lux::cxx::TypeToken cpp_type;
        const system::SystemTypeDescription* description{};
        lux::serialization::PortableValueCodec configuration{};
        std::span<const ComponentObservationSpec> observations;
        std::span<const SceneSystemRequirementSpec> requirements;
        InstallSceneSystemFn install{};
        std::span<const SceneSystemCapabilityProjection> projections;
        std::shared_ptr<const void> code_lifetime;
    };
    [[nodiscard]] inline bool validSceneSystemRegistration(const SceneSystemRegistration& registration) noexcept
    {
        const bool invalid_identity = !registration.type.valid() || !registration.cpp_type.isValid() ||
                                      !registration.description || !registration.install;
        if (invalid_identity)
            return false;
        const auto& description = *registration.description;
        const bool invalid_description = !system::validSystemTypeDescription(description) ||
                                         description.canonical_name != registration.type.name ||
                                         system::systemTypeId(description.canonical_name) != registration.type;
        const bool invalid_configuration =
            description.configuration_schema_name.empty() == registration.configuration.valid();
        if (invalid_description || invalid_configuration)
            return false;
        for (std::size_t i{}; i < registration.requirements.size(); ++i)
        {
            const auto& value = registration.requirements[i];
            if (value.name.empty() || value.capability.empty() || !value.expected_type.isValid())
                return false;
            for (std::size_t previous{}; previous < i; ++previous)
                if (registration.requirements[previous].name == value.name)
                    return false;
        }
        for (std::size_t i{}; i < registration.projections.size(); ++i)
        {
            const auto& value = registration.projections[i];
            if (!value.type.isValid() || !value.project)
                return false;
            for (std::size_t previous{}; previous < i; ++previous)
                if (registration.projections[previous].type == value.type)
                    return false;
        }
        return true;
    }
} // namespace lux::scene
