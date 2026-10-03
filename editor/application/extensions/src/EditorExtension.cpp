#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <algorithm>

namespace lux::editor::extensions
{
    namespace
    {
        bool validCounts(ContributionCounts counts) noexcept
        {
            return counts.commands <= 256 && counts.sessions <= 256 && counts.views <= 256 &&
                   counts.reflection <= 256 && counts.configurations <= 256 && counts.components <= 256 &&
                   counts.settings <= 256;
        }
        ContributionResult<ContributionDraft> normalizeDraft(
            std::shared_ptr<const void> pinned,
            ContributionCounts counts,
            ContributionDraft incoming
        )
        {
            auto draft = std::move(incoming);
            const bool mismatch =
                counts.commands != draft.commands.size() || counts.sessions != draft.sessions.size() ||
                counts.reflection != draft.reflection.size() || counts.views != draft.views.size() ||
                counts.configurations != draft.configurations.size() || counts.components != draft.components.size() ||
                counts.settings != draft.settings.size();
            if (mismatch)
                return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "extension.counts"});
            const auto lease = contracts::CodeLease::plugin(pinned);
            draft.code.push_back(lease);
            for (const auto& entry : draft.reflection)
                if (!entry.code.sameOwner(lease))
                    return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "reflection.code"}
                    );
            for (const auto& entry : draft.commands)
                if (!entry || !entry->usesCode(lease))
                    return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "command.code"});
            for (const auto& entry : draft.sessions)
                if (!entry || !entry->usesCode(lease))
                    return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "session.code"});
            for (const auto& entry : draft.views)
                if (!entry || !entry->usesCode(lease))
                    return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "view.code"});
            for (const auto& item : draft.settings)
                if (!item.entry || !item.entry->usesCode(lease))
                    return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "settings.code"});
            for (auto& entry : draft.configurations)
                entry.code = lease;
            for (auto& entry : draft.components)
            {
                struct Pins final
                {
                    std::shared_ptr<const void> library, original;
                };
                entry.code = std::make_shared<Pins>(pinned, std::move(entry.code));
            }
            return draft;
        }
    } // namespace
    lux::project::PluginResult<EditorExtension> EditorExtension::load(
        const lux::project::PluginDescription& description,
        const lux::project::PluginLibrary& runtime,
        std::span<const EditorExtension> dependencies
    )
    {
        const auto fail = [&](lux::project::EPluginError code, std::string subject)
        { return cxx::unexpected(lux::project::PluginFailure{code, description.identity.id, std::move(subject)}); };
        if (runtime.identity() != description.identity)
            return fail(lux::project::EPluginError::MODULE_MISMATCH, "runtime");
        if (!description.editor_library)
        {
            // A selected runtime-only dependency still participates in the ordered identity/pin set.
            // It contributes no Editor behavior; this does not waive any declared Editor dependency.
            EditorExtension result;
            result.identity_ = description.identity;
            result.code_ = runtime.runtimeCode();
            return result;
        }
        std::vector<std::shared_ptr<const void>> pins{runtime.runtimeCode()};
        for (const auto& identity : description.dependencies)
        {
            const auto found = std::ranges::find(dependencies, identity, &EditorExtension::identity);
            if (found == dependencies.end())
                return fail(lux::project::EPluginError::MISSING_DEPENDENCY, identity.id);
            pins.push_back(found->code());
        }
        auto library = lux::project::loadPluginLibrary(description, *description.editor_library, pins);
        if (!library)
            return cxx::unexpected(library.error());
        const auto get = (*library)->get_symbol<GetEditorExtension>(kEditorExtensionSymbol);
        if (!get)
            return fail(lux::project::EPluginError::MISSING_EXPORT, kEditorExtensionSymbol);
        const auto* table = get();
        const bool invalid_header = !table || table->structure_size != sizeof(EditorExtensionExports) ||
                                    table->interface_version != kEditorExtensionVersion;
        if (invalid_header)
            return fail(lux::project::EPluginError::INVALID_EXPORT, "editor.header");
        if (!table->editor_sdk_abi || std::string_view(table->editor_sdk_abi) != kEditorExtensionAbi)
            return fail(lux::project::EPluginError::ABI_MISMATCH, "editor.sdk");
        const bool has_activation = table->activate != nullptr;
        const auto requirements = table->requires_capabilities;
        const bool has_requirements = requirements.sessions || requirements.project || requirements.workbench;
        const bool has_activation_entries = table->activation_counts != ContributionCounts{};
        const bool is_invalid_activation = !has_activation && (has_requirements || has_activation_entries);
        const bool invalid_counts = !validCounts(table->counts) || !validCounts(table->activation_counts) ||
                                    !table->contribute || is_invalid_activation;
        if (invalid_counts)
            return fail(lux::project::EPluginError::INVALID_EXPORT, "editor.counts");
        EditorExtension result;
        result.identity_ = description.identity;
        result.code_ = std::move(*library);
        result.exports_ = table;
        return result;
    }
    ContributionResult<ContributionDraft> EditorExtension::contributions() const
    {
        // External pin encloses callbacks, error construction and complete draft destruction on rejection.
        const auto pinned = code_;
        const auto* table = exports_;
        if (!pinned)
            return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "extension"});
        ContributionDraft draft;
        if (!table)
            return draft;
        try
        {
            auto result = table->contribute(draft, contracts::CodeLease::plugin(pinned));
            if (!result)
                return cxx::unexpected(result.error());
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            return cxx::unexpected(ContributionFailure{EContributionError::CALLBACK, "extension.contribute"});
        }
        return normalizeDraft(pinned, table->counts, std::move(draft));
    }
    ContributionResult<ContributionDraft> EditorExtension::activate(const ExtensionCapabilities& supplied) const
    {
        if (std::this_thread::get_id() != owner_)
            return cxx::unexpected(ContributionFailure{EContributionError::WRONG_THREAD, "extension.activate"});
        const auto pinned = code_;
        if (!pinned)
            return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "extension"});
        const auto* table = exports_;
        if (!table || !table->activate)
            return ContributionDraft{};
        const auto requirements = table->requires_capabilities;
        const bool is_missing_sessions = requirements.sessions && !supplied.sessions;
        const bool is_missing_project = requirements.project && !supplied.project;
        const bool is_missing_workbench = requirements.workbench && !supplied.workbench;
        if (is_missing_sessions || is_missing_project || is_missing_workbench)
            return cxx::unexpected(ContributionFailure{
                EContributionError::UNAVAILABLE,
                "extension.capabilities",
                0,
                is_missing_sessions  ? "sessions"
                : is_missing_project ? "project"
                                     : "workbench"
            });
        const ExtensionCapabilities selected{
            requirements.sessions ? supplied.sessions : nullptr,
            requirements.project ? supplied.project : nullptr,
            requirements.workbench ? supplied.workbench : nullptr
        };
        ContributionDraft draft;
        try
        {
            auto result = table->activate(draft, contracts::CodeLease::plugin(pinned), selected);
            if (!result)
                return cxx::unexpected(result.error());
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            return cxx::unexpected(ContributionFailure{EContributionError::CALLBACK, "extension.activate"});
        }
        return normalizeDraft(pinned, table->activation_counts, std::move(draft));
    }
    const lux::project::MetadataIdentity& EditorExtension::identity() const noexcept
    {
        return identity_;
    }
    const std::shared_ptr<const void>& EditorExtension::code() const noexcept
    {
        return code_;
    }
} // namespace lux::editor::extensions
