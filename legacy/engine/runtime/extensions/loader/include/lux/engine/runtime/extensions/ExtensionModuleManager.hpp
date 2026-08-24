#pragma once
/**
 * @file ExtensionModuleManager.hpp
 * @brief Domain-agnostic extension loading, validation and code lifetime.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/runtime/extensions/ModuleLifetime.hpp>
#include <lux/engine/runtime/extensions/loader_visibility.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lux::extensions
{
    struct ExtensionDependency final
    {
        ExtensionId id;
        std::uint16_t required_major{0u};
        std::uint16_t minimum_minor{0u};
    };

    struct ExtensionModuleRequirement final
    {
        ExtensionId id;
        ExtensionModuleSource source;
        EExtensionModuleTarget target{EExtensionModuleTarget::RUNTIME};
        std::uint16_t required_major{0u};
        std::uint16_t minimum_minor{0u};

        [[nodiscard]] static ExtensionModuleRequirement fromPath(
            ExtensionId id,
            std::filesystem::path path,
            EExtensionModuleTarget target,
            std::uint16_t required_major,
            std::uint16_t minimum_minor)
        {
            return ExtensionModuleRequirement{
                std::move(id),
                ExtensionModuleSource::fromPath(std::move(path)),
                target,
                required_major,
                minimum_minor};
        }

        [[nodiscard]] static ExtensionModuleRequirement fromMemory(
            ExtensionId id,
            lux::cxx::SharedBytes<> image,
            std::string hint,
            EExtensionModuleTarget target,
            std::uint16_t required_major,
            std::uint16_t minimum_minor)
        {
            return ExtensionModuleRequirement{
                std::move(id),
                ExtensionModuleSource::fromMemory(
                    std::move(image),
                    std::move(hint)),
                target,
                required_major,
                minimum_minor};
        }
    };

    enum class EExtensionModuleLoadError : std::uint8_t
    {
        INVALID_REQUIREMENT,
        LIBRARY_LOAD_FAILED,
        DESCRIPTOR_SYMBOL_MISSING,
        NULL_DESCRIPTOR,
        DESCRIPTOR_TOO_SMALL,
        EXTENSION_ABI_MISMATCH,
        ENGINE_ABI_MISMATCH,
        INVALID_DESCRIPTOR_ID,
        MANIFEST_ID_MISMATCH,
        TARGET_MISMATCH,
        VERSION_MISMATCH,
        INVALID_DEPENDENCY
    };

    struct ExtensionModuleLoadFailure final
    {
        EExtensionModuleLoadError code{
            EExtensionModuleLoadError::INVALID_REQUIREMENT};
        ExtensionId module;
        ExtensionId dependency;
        std::string detail;
    };

    struct ExtensionModuleEntrypoints final
    {
        InstallWorldSystemsV5Fn* world_systems{nullptr};
        InstallRenderFeaturesV5Fn* render_features{nullptr};
        InstallEditorPanelsV5Fn* editor_panels{nullptr};
        ModuleLease module;
    };

    class PreparedExtensionModule final
    {
    public:
        PreparedExtensionModule() noexcept = default;
        PreparedExtensionModule(const PreparedExtensionModule&) = delete;
        PreparedExtensionModule& operator=(const PreparedExtensionModule&) =
            delete;
        PreparedExtensionModule(PreparedExtensionModule&&) noexcept = default;
        PreparedExtensionModule& operator=(PreparedExtensionModule&&) noexcept =
            default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return static_cast<bool>(module_);
        }
        [[nodiscard]] const ModuleLifetime& module() const noexcept
        {
            return *module_;
        }
        [[nodiscard]] ModuleLease lease() const noexcept
        {
            return module_;
        }
        [[nodiscard]] EExtensionModuleTarget target() const noexcept
        {
            return target_;
        }
        [[nodiscard]] std::span<const ExtensionDependency>
        dependencies() const noexcept
        {
            return dependencies_;
        }

    private:
        friend class ExtensionModuleManager;

        ModuleLease module_;
        EExtensionModuleTarget target_{EExtensionModuleTarget::RUNTIME};
        std::vector<ExtensionDependency> dependencies_;
        InstallWorldSystemsV5Fn* world_systems_entry_{nullptr};
        InstallRenderFeaturesV5Fn* render_features_entry_{nullptr};
        InstallEditorPanelsV5Fn* editor_panels_entry_{nullptr};
    };

    enum class EExtensionModuleCommitError : std::uint8_t
    {
        INVALID_PREPARED_MODULE,
        DUPLICATE_EXTENSION,
        HASH_COLLISION,
        MISSING_DEPENDENCY,
        DEPENDENCY_NOT_READY,
        DEPENDENCY_VERSION_MISMATCH,
        DEPENDENCY_CYCLE,
        MANAGER_CLOSED
    };

    struct ExtensionModuleCommitFailure final
    {
        EExtensionModuleCommitError code{
            EExtensionModuleCommitError::INVALID_PREPARED_MODULE};
        ExtensionId module;
        ExtensionId dependency;
    };

    enum class EExtensionModuleState : std::uint8_t
    {
        LOADED,
        REGISTERING,
        READY,
        FAILED
    };

    struct ExtensionModuleSnapshot final
    {
        ExtensionId id;
        ExtensionVersion version;
        ExtensionModuleOrigin origin;
        EExtensionModuleTarget target{EExtensionModuleTarget::RUNTIME};
        EExtensionModuleState state{EExtensionModuleState::LOADED};
        std::vector<ExtensionDependency> dependencies;
    };

    enum class EExtensionRequirementStatus : std::uint8_t
    {
        READY,
        MISSING,
        VERSION_MISMATCH,
        NOT_READY,
        ID_COLLISION
    };

    enum class EExtensionModuleCloseError : std::uint8_t
    {
        MODULE_IN_USE,
        ALREADY_CLOSED
    };

    class LUX_RUNTIME_EXTENSION_LOADER_PUBLIC ExtensionModuleManager final
    {
    public:
        ExtensionModuleManager() = default;
        ~ExtensionModuleManager() = default;

        ExtensionModuleManager(const ExtensionModuleManager&) = delete;
        ExtensionModuleManager& operator=(const ExtensionModuleManager&) =
            delete;

        [[nodiscard]] static lux::cxx::expected<
            PreparedExtensionModule,
            ExtensionModuleLoadFailure>
        prepare(const ExtensionModuleRequirement& requirement) noexcept;

        [[nodiscard]] lux::cxx::expected<
            std::vector<ModuleLease>,
            ExtensionModuleCommitFailure>
        commitBatch(std::vector<PreparedExtensionModule> modules) noexcept;

        [[nodiscard]] ModuleLease find(ExtensionIdView id) const noexcept;
        [[nodiscard]] EExtensionRequirementStatus requirementStatus(
            ExtensionIdView id,
            std::uint16_t required_major,
            std::uint16_t minimum_minor) const noexcept;
        [[nodiscard]] ExtensionModuleEntrypoints entrypoints(
            ExtensionIdView id) const noexcept;
        [[nodiscard]] std::vector<ExtensionModuleSnapshot> snapshot() const;

        [[nodiscard]] bool beginRegistration(ExtensionIdView id) noexcept;
        [[nodiscard]] bool markReady(ExtensionIdView id) noexcept;
        [[nodiscard]] bool markFailed(ExtensionIdView id) noexcept;

        [[nodiscard]] lux::cxx::expected<void, EExtensionModuleCloseError>
        close() noexcept;

    private:
        struct Record final
        {
            ModuleLease module;
            EExtensionModuleTarget target{EExtensionModuleTarget::RUNTIME};
            EExtensionModuleState state{EExtensionModuleState::LOADED};
            std::vector<ExtensionDependency> dependencies;
            InstallWorldSystemsV5Fn* world_systems_entry{nullptr};
            InstallRenderFeaturesV5Fn* render_features_entry{nullptr};
            InstallEditorPanelsV5Fn* editor_panels_entry{nullptr};
        };

        [[nodiscard]] Record* findRecord(ExtensionIdView id) noexcept;
        [[nodiscard]] const Record* findRecord(ExtensionIdView id) const noexcept;

        std::vector<Record> records_;
        bool closed_{false};
    };
}
