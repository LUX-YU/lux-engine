#pragma once

#include <lux/engine/scene/SceneDescription.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace lux::scene
{
    enum class ESceneDescriptionError : std::uint8_t
    {
        INVALID_WORLD,
        INVALID_SIMULATION,
        INVALID_SYSTEM_INSTANCE,
        INVALID_SYSTEM_INSTANCE_NAME,
        INVALID_SYSTEM_TYPE,
        INVALID_SYSTEM_VERSION,
        DUPLICATE_SYSTEM_INSTANCE,
        DUPLICATE_SYSTEM_INSTANCE_NAME,
        INVALID_CONFIGURATION_SCHEMA,
        SYSTEM_NOT_FOUND,
        INVALID_REQUIREMENT,
        INVALID_PROVIDER_NAME,
        DUPLICATE_REQUIREMENT_BINDING,
        INVALID_DEPENDENCY,
        DUPLICATE_DEPENDENCY,
        DEPENDENCY_CYCLE,
        SIZE_OVERFLOW,
        ALLOCATION_FAILURE,
    };

    struct SceneDescriptionFailure final
    {
        ESceneDescriptionError code{ESceneDescriptionError::ALLOCATION_FAILURE};
        system::SystemInstanceId system{};
        std::uint64_t subject_hash{};
    };

    class LUX_ENGINE_SCENE_DESCRIPTION_PUBLIC SceneDescriptionBuilder final
    {
    public:
        SceneDescriptionBuilder();
        ~SceneDescriptionBuilder();
        SceneDescriptionBuilder(SceneDescriptionBuilder&&) noexcept;
        SceneDescriptionBuilder& operator=(SceneDescriptionBuilder&&) noexcept;
        SceneDescriptionBuilder(const SceneDescriptionBuilder&) = delete;
        SceneDescriptionBuilder& operator=(const SceneDescriptionBuilder&) = delete;

        void setWorld(asset::AssetId id) noexcept;
        void setSimulation(asset::AssetId id) noexcept;
        [[nodiscard]] lux::cxx::expected<void, SceneDescriptionFailure> addSystem(
            system::SystemInstanceId id,
            std::string_view instance_name,
            const system::SystemTypeId& type,
            std::uint32_t system_version,
            std::string_view configuration_schema_name,
            std::uint32_t configuration_schema_version,
            std::span<const std::byte> configuration = {}
        ) noexcept;
        [[nodiscard]] lux::cxx::expected<void, SceneDescriptionFailure> setSystemConfiguration(
            system::SystemInstanceId id,
            std::span<const std::byte> configuration
        ) noexcept;
        [[nodiscard]] lux::cxx::expected<void, SceneDescriptionFailure> eraseSystem(
            system::SystemInstanceId id
        ) noexcept;
        [[nodiscard]] lux::cxx::expected<void, SceneDescriptionFailure> bindRequirement(
            system::SystemInstanceId system,
            std::string_view requirement,
            std::string_view provider
        ) noexcept;
        [[nodiscard]] lux::cxx::expected<void, SceneDescriptionFailure> unbindRequirement(
            system::SystemInstanceId system,
            std::string_view requirement
        ) noexcept;
        [[nodiscard]] lux::cxx::expected<void, SceneDescriptionFailure> addDependency(
            system::SystemInstanceId before,
            system::SystemInstanceId after
        ) noexcept;
        [[nodiscard]] lux::cxx::expected<void, SceneDescriptionFailure> eraseDependency(
            system::SystemInstanceId before,
            system::SystemInstanceId after
        ) noexcept;
        void clear() noexcept;
        [[nodiscard]] lux::cxx::expected<SceneDescription, SceneDescriptionFailure> build() && noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::scene
