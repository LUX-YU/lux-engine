#pragma once
/**
 * @file EditorContributionRegistrar.hpp
 * @brief Unpublished editor-only contribution transaction for one module.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/editor/extensions/EditorTools.hpp>
#include <lux/engine/editor/visibility.h>
#include <lux/engine/runtime/extensions/ModuleLifetime.hpp>
#include <lux/engine/runtime/extensions/EngineExtensions.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace lux::extensions
{
    enum class EEditorContributionDraftError : std::uint8_t
    {
        INVALID_DESCRIPTOR,
        REGISTRAR_FINISHED
    };

    struct EditorRegistrationDraft final
    {
        EditorRegistrationDraft() = default;
        EditorRegistrationDraft(const EditorRegistrationDraft&) = delete;
        EditorRegistrationDraft& operator=(const EditorRegistrationDraft&) =
            delete;
        EditorRegistrationDraft(EditorRegistrationDraft&&) noexcept = default;
        EditorRegistrationDraft& operator=(EditorRegistrationDraft&&) noexcept =
            default;

        std::vector<lux::editor::EditorPanelContributionDescriptor> panels;
    };

    class EditorPanelRegistrar final
    {
    public:
        [[nodiscard]] lux::cxx::expected<
            void,
            EEditorContributionDraftError>
        add(lux::editor::EditorPanelContributionDescriptor descriptor);

    private:
        friend class EditorContributionRegistrar;
        EditorPanelRegistrar(
            EditorRegistrationDraft& draft,
            ModuleLease module,
            const bool& finished) noexcept
            : draft_(&draft), module_(std::move(module)), finished_(&finished)
        {}

        EditorRegistrationDraft* draft_{nullptr};
        ModuleLease module_;
        const bool* finished_{nullptr};
    };

    class LUX_EDITOR_PUBLIC EditorContributionRegistrar final
    {
    public:
        explicit EditorContributionRegistrar(ModuleLease module) noexcept;
        EditorContributionRegistrar(const EditorContributionRegistrar&) =
            delete;
        EditorContributionRegistrar& operator=(
            const EditorContributionRegistrar&) = delete;

        [[nodiscard]] EditorPanelRegistrar& panels() noexcept
        {
            return panels_;
        }

        [[nodiscard]] EditorRegistrationDraft finish() && noexcept;

    private:
        ModuleLease module_;
        bool finished_{false};
        EditorRegistrationDraft draft_;
        EditorPanelRegistrar panels_;
    };

    [[nodiscard]] LUX_EDITOR_PUBLIC lux::cxx::expected<
        void,
        lux::editor::EEditorPanelCatalogError>
    commitEditorCatalogs(
        EditorRegistrationDraft&& draft,
        lux::editor::EditorPanelCatalog& panels);

    /// Build the editor half of EngineExtensions without exposing editor
    /// catalogue types to runtime_render_scene or Player builds.
    [[nodiscard]] LUX_EDITOR_PUBLIC PrepareEditorRegistration
    makeEditorRegistrationAdapter(
        lux::editor::EditorPanelCatalog& panels);
}
