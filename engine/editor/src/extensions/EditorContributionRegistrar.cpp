#include <lux/engine/editor/extensions/EditorContributionRegistrar.hpp>

#include <utility>

namespace lux::extensions
{
    namespace
    {
        class EditorTransaction final
            : public EditorRegistrationTransaction
        {
        public:
            EditorTransaction(
                EditorRegistrationDraft draft,
                lux::editor::EditorPanelCatalog& panels) noexcept
                : draft_(std::move(draft)), panels_(&panels)
            {}

            [[nodiscard]] lux::cxx::expected<void, std::uint32_t>
            validate() noexcept override
            {
                auto result = panels_->validateBatch(draft_.panels);
                if (!result)
                {
                    return lux::cxx::unexpected(
                        static_cast<std::uint32_t>(result.error()));
                }
                return {};
            }

            [[nodiscard]] lux::cxx::expected<void, std::uint32_t>
            commit() noexcept override
            {
                auto result = commitEditorCatalogs(
                    std::move(draft_),
                    *panels_);
                if (!result)
                {
                    return lux::cxx::unexpected(
                        static_cast<std::uint32_t>(result.error()));
                }
                return {};
            }

        private:
            EditorRegistrationDraft draft_;
            lux::editor::EditorPanelCatalog* panels_{nullptr};
        };
    }

    lux::cxx::expected<void, EEditorContributionDraftError>
    EditorPanelRegistrar::add(
        lux::editor::EditorPanelContributionDescriptor descriptor)
    {
        if (!finished_ || *finished_ || !module_)
        {
            return lux::cxx::unexpected(
                EEditorContributionDraftError::REGISTRAR_FINISHED);
        }
        if (!descriptor.id.isValid() || !descriptor.create)
        {
            return lux::cxx::unexpected(
                EEditorContributionDraftError::INVALID_DESCRIPTOR);
        }
        descriptor.provider = module_->id();
        descriptor.module = module_;
        draft_->panels.push_back(std::move(descriptor));
        return {};
    }

    EditorContributionRegistrar::EditorContributionRegistrar(
        ModuleLease module) noexcept
        : module_(std::move(module))
        , panels_(draft_, module_, finished_)
    {}

    EditorRegistrationDraft EditorContributionRegistrar::finish() && noexcept
    {
        finished_ = true;
        return std::move(draft_);
    }

    lux::cxx::expected<void, lux::editor::EEditorPanelCatalogError>
    commitEditorCatalogs(
        EditorRegistrationDraft&& draft,
        lux::editor::EditorPanelCatalog& panels)
    {
        if (auto checked = panels.validateBatch(draft.panels); !checked)
            return checked;
        return panels.addBatch(std::move(draft.panels));
    }

    PrepareEditorRegistration makeEditorRegistrationAdapter(
        lux::editor::EditorPanelCatalog& panels)
    {
        return [&panels](ExtensionModuleEntrypoints entrypoints)
            -> lux::cxx::expected<
                std::unique_ptr<EditorRegistrationTransaction>,
                std::uint32_t>
        {
            if (!entrypoints.module || !entrypoints.editor)
            {
                return lux::cxx::unexpected(static_cast<std::uint32_t>(
                    EExtensionRegistrationError::INVALID_DESCRIPTOR));
            }
            EditorContributionRegistrar registrar{entrypoints.module};
            const auto registered = entrypoints.editor(registrar);
            if (!registered)
            {
                return lux::cxx::unexpected(
                    static_cast<std::uint32_t>(registered.error));
            }
            std::unique_ptr<EditorRegistrationTransaction> transaction =
                std::make_unique<EditorTransaction>(
                    std::move(registrar).finish(),
                    panels);
            return transaction;
        };
    }
}
