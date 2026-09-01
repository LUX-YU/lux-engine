#pragma once

#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/scene/description/visibility.h>
#include <lux/engine/system/SystemInstanceId.hpp>
#include <lux/engine/system/SystemTypeId.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lux::scene
{
    class SceneDescription;
    class SceneDescriptionBuilder;
    class SceneSystemView;
    class SceneRequirementBindingView;
    class SceneSystemDependencyView;

    class LUX_ENGINE_SCENE_DESCRIPTION_PUBLIC SceneRequirementBindingView final
    {
    public:
        SceneRequirementBindingView() noexcept = default;
        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] system::SystemInstanceId system() const noexcept;
        [[nodiscard]] std::string_view requirement() const noexcept;
        [[nodiscard]] std::string_view provider() const noexcept;

    private:
        SceneRequirementBindingView(const SceneDescription& description, std::size_t binding_index) noexcept;
        const SceneDescription* description_{};
        std::size_t binding_index_{};
        friend class SceneDescription;
        friend class SceneSystemView;
    };

    class LUX_ENGINE_SCENE_DESCRIPTION_PUBLIC SceneSystemDependencyView final
    {
    public:
        SceneSystemDependencyView() noexcept = default;
        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] system::SystemInstanceId before() const noexcept;
        [[nodiscard]] system::SystemInstanceId after() const noexcept;

    private:
        SceneSystemDependencyView(const SceneDescription& description, std::size_t dependency_index) noexcept;
        const SceneDescription* description_{};
        std::size_t dependency_index_{};
        friend class SceneDescription;
    };

    class LUX_ENGINE_SCENE_DESCRIPTION_PUBLIC SceneSystemView final
    {
    public:
        SceneSystemView() noexcept = default;
        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] system::SystemInstanceId instanceId() const noexcept;
        [[nodiscard]] std::string_view instanceName() const noexcept;
        [[nodiscard]] const system::SystemTypeId& type() const noexcept;
        [[nodiscard]] std::uint32_t version() const noexcept;
        [[nodiscard]] std::string_view configurationSchemaName() const noexcept;
        [[nodiscard]] std::uint64_t configurationSchemaHash() const noexcept;
        [[nodiscard]] std::uint32_t configurationSchemaVersion() const noexcept;
        [[nodiscard]] std::span<const std::byte> configurationPayload() const noexcept;
        [[nodiscard]] std::size_t requirementBindingCount() const noexcept;
        [[nodiscard]] SceneRequirementBindingView requirementBindingAt(std::size_t index) const noexcept;
        [[nodiscard]] SceneRequirementBindingView findRequirementBinding(std::string_view requirement) const noexcept;

    private:
        SceneSystemView(const SceneDescription& description, std::size_t system_index) noexcept;
        const SceneDescription* description_{};
        std::size_t system_index_{};
        friend class SceneDescription;
    };

    class LUX_ENGINE_SCENE_DESCRIPTION_PUBLIC SceneDescription final
    {
    public:
        SceneDescription() noexcept = default;
        SceneDescription(SceneDescription&&) noexcept = default;
        SceneDescription& operator=(SceneDescription&&) noexcept = default;
        SceneDescription(const SceneDescription&) = delete;
        SceneDescription& operator=(const SceneDescription&) = delete;

        [[nodiscard]] asset::AssetId world() const noexcept;
        [[nodiscard]] asset::AssetId simulation() const noexcept;
        [[nodiscard]] std::size_t systemCount() const noexcept;
        [[nodiscard]] SceneSystemView systemAt(std::size_t index) const noexcept;
        [[nodiscard]] SceneSystemView findSystem(system::SystemInstanceId id) const noexcept;
        [[nodiscard]] SceneSystemView findSystem(std::string_view instance_name) const noexcept;
        [[nodiscard]] std::size_t dependencyCount() const noexcept;
        [[nodiscard]] SceneSystemDependencyView dependencyAt(std::size_t index) const noexcept;

    private:
        struct SystemRecord final
        {
            system::SystemInstanceId id{};
            std::string instance_name;
            system::SystemTypeId type;
            std::uint32_t version{};
            std::string configuration_schema_name;
            std::uint64_t configuration_schema_hash{};
            std::uint32_t configuration_schema_version{};
            std::size_t configuration_offset{};
            std::size_t configuration_size{};
        };
        struct RequirementBindingRecord final
        {
            std::size_t system_ordinal{};
            std::string requirement;
            std::string provider;
        };
        struct DependencyRecord final
        {
            std::size_t before_system{};
            std::size_t after_system{};
        };

        asset::AssetId world_{};
        asset::AssetId simulation_{};
        std::vector<SystemRecord> systems_;
        std::unordered_map<std::uint64_t, std::size_t> system_ordinals_;
        std::vector<std::byte> configuration_payload_;
        std::vector<RequirementBindingRecord> requirement_bindings_;
        std::vector<DependencyRecord> dependencies_;

        friend class SceneDescriptionBuilder;
        friend class SceneSystemView;
        friend class SceneRequirementBindingView;
        friend class SceneSystemDependencyView;
    };
} // namespace lux::scene
