#pragma once
/**
 * @file EngineExtensions.hpp
 * @brief Domain-neutral facade for asynchronous extension-module loading.
 *
 * Business code sees only requestLoad(), a pollable ticket and committed
 * module snapshots. The implementation owns the thread transitions:
 * DynamicLibrary::load on BlockingIO, registration/catalog publication on the
 * main thread, and dynamic operation installation on AsyncRuntime's
 * coordinator.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/runtime/extensions/ExtensionModuleManager.hpp>
#include <lux/engine/runtime/extensions/OperationTicket.hpp>
#include <lux/engine/runtime/extensions/RuntimeContributionRegistrar.hpp>
#include <lux/engine/runtime/extensions/contribution_visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace lux::events
{
    class DomainEvents;
}

namespace lux::exec
{
    class AsyncRuntime;
}

namespace lux::extensions
{
    class EngineExtensionsCloseSender;
    class EngineExtensions;
    struct EngineExtensionsCloseReport;
    LUX_RUNTIME_CONTRIBUTION_PUBLIC void subscribeEngineExtensionsClose(
        EngineExtensions&,
        lux::cxx::move_only_function<void(EngineExtensionsCloseReport)>)
        noexcept;
    enum class EExtensionLoadPhase : std::uint8_t
    {
        QUEUED,
        LOADING_LIBRARY,
        VALIDATING_MODULE,
        COLLECTING_CONTRIBUTIONS,
        INSTALLING_ASYNC_OPERATIONS,
        COMMITTING_CATALOGS,
        READY
    };

    enum class EExtensionLoadError : std::uint8_t
    {
        NONE,
        QUEUE_FULL,
        BYTE_BUDGET_EXHAUSTED,
        STOPPING,
        UNKNOWN_EXTENSION,
        ID_COLLISION,
        LIBRARY_LOAD_FAILED,
        DEPENDENCY_MISSING,
        DEPENDENCY_NOT_READY,
        DEPENDENCY_VERSION_MISMATCH,
        DEPENDENCY_CYCLE,
        MODULE_COMMIT_FAILED,
        REGISTRATION_FAILED,
        CATALOG_VALIDATION_FAILED,
        ASYNC_OPERATION_INSTALL_FAILED,
        CATALOG_COMMIT_FAILED
    };

    struct ExtensionLoadResult final
    {
        ExtensionId id;
        ExtensionVersion version;
    };

    using ExtensionLoadTicket = OperationTicket<
        EExtensionLoadPhase,
        EExtensionLoadError,
        ExtensionLoadResult>;

    struct ExtensionModuleLoaded final
    {
        ExtensionId id;
        ExtensionVersion version;
    };

    struct EngineExtensionQueueConfig final
    {
        std::size_t capacity{64u};
        std::size_t byte_budget{1024u * 1024u};
    };

    enum class EExtensionRequirementUpdateError : std::uint8_t
    {
        INVALID_REQUIREMENT,
        CONFLICTING_REQUIREMENT,
        ID_COLLISION,
        STOPPING
    };

    /// Editor-only catalogue types stay outside runtime_render_scene. This narrow
    /// transaction is prepared by the editor adapter and participates in the
    /// same validate-before-publish protocol as runtime catalogues.
    class LUX_RUNTIME_CONTRIBUTION_PUBLIC EditorRegistrationTransaction
    {
    public:
        virtual ~EditorRegistrationTransaction() = default;
        [[nodiscard]] virtual lux::cxx::expected<void, std::uint32_t>
        validate() noexcept = 0;
        [[nodiscard]] virtual lux::cxx::expected<void, std::uint32_t>
        commit() noexcept = 0;
    };

    using PrepareEditorRegistration = lux::cxx::move_only_function<
        lux::cxx::expected<
            std::unique_ptr<EditorRegistrationTransaction>,
            std::uint32_t>(ExtensionModuleEntrypoints)>;

    struct EngineExtensionServices final
    {
        ExtensionModuleManager& modules;
        lux::exec::AsyncRuntime& async;
        RuntimeCatalogSet runtime_catalogs;
        lux::events::DomainEvents* events{nullptr};
        PrepareEditorRegistration prepare_editor;
    };

    struct EngineExtensionsCloseReport final
    {
        std::size_t rejected_queued{0u};
        std::size_t pending_loads{0u};
        std::size_t operation_bundles_released{0u};
        std::size_t operation_bundle_close_failures{0u};

        [[nodiscard]] bool terminal() const noexcept
        {
            return pending_loads == 0u;
        }
    };

    struct EngineExtensionsSnapshot final
    {
        std::vector<ExtensionModuleSnapshot> modules;
        std::optional<OperationSnapshot<
            EExtensionLoadPhase,
            EExtensionLoadError,
            ExtensionLoadResult>> active_operation;
        std::size_t queued_commands{0u};
        std::size_t accounted_bytes{0u};
        bool admission_open{false};
        bool closing{false};
    };

    class LUX_RUNTIME_CONTRIBUTION_PUBLIC EngineExtensions final
    {
    public:
        EngineExtensions(
            EngineExtensionServices services,
            std::vector<ExtensionModuleRequirement> requirements,
            EngineExtensionQueueConfig queue = {});
        ~EngineExtensions() noexcept;
        EngineExtensions(const EngineExtensions&) = delete;
        EngineExtensions& operator=(const EngineExtensions&) = delete;

        [[nodiscard]] ExtensionLoadTicket requestLoad(
            ExtensionIdView id) const;

        /// Append project/cooked-manifest requirements at a cold main-thread
        /// path. Identical entries are idempotent; an existing id cannot be
        /// silently redirected to another binary or version contract.
        [[nodiscard]] lux::cxx::expected<
            void,
            EExtensionRequirementUpdateError>
        addRequirements(
            std::vector<ExtensionModuleRequirement> requirements) noexcept;

        /// Main-thread safe point. Usually work reaches this through the
        /// MainThreadMailbox wake, but hosts call it explicitly during idle and
        /// close paths as a deterministic fallback.
        [[nodiscard]] std::size_t processSafePoint(
            std::size_t budget = 8u) noexcept;

        [[nodiscard]] std::vector<ExtensionModuleSnapshot> snapshot() const;
        [[nodiscard]] EngineExtensionsSnapshot runtimeSnapshot() const;
        [[nodiscard]] EngineExtensionsCloseSender closeAsync() noexcept;

    private:
        friend LUX_RUNTIME_CONTRIBUTION_PUBLIC void subscribeEngineExtensionsClose(
            EngineExtensions&,
            lux::cxx::move_only_function<void(EngineExtensionsCloseReport)>)
            noexcept;
        struct Impl;
        std::shared_ptr<Impl> impl_;
    };
}
