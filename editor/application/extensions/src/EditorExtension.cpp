#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <algorithm>

namespace lux::editor::extensions
{
    lux::project::PluginResult<EditorExtension> EditorExtension::load(
        const lux::project::PluginDescription& description,
        const lux::project::PluginLibrary& runtime,
        std::span<const EditorExtension> dependencies
    )
    {
        const auto fail = [&](lux::project::EPluginError code, std::string subject) {
            return cxx::unexpected(lux::project::PluginFailure{code, description.identity.id, std::move(subject)});
        };
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
        const auto counts = table->counts;
        const bool invalid_counts = counts.commands > 256 || counts.sessions > 256 || counts.views > 256 ||
                                    counts.reflection > 256 || counts.configurations > 256 || counts.components > 256 ||
                                    !table->contribute;
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
        const auto counts = table->counts;
        const bool mismatch = counts.commands != draft.commands.size() || counts.sessions != draft.sessions.size() ||
                              counts.reflection != draft.reflection.size() || counts.views != draft.views.size() ||
                              counts.configurations != draft.configurations.size() ||
                              counts.components != draft.components.size();
        if (mismatch)
            return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "extension.counts"});
        const auto lease = contracts::CodeLease::plugin(pinned);
        draft.code.push_back(lease);
        for (const auto& entry : draft.reflection)
            if (!entry.code.sameOwner(lease))
                return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "reflection.code"});
        for (const auto& entry : draft.commands)
            if (!entry || !entry->usesCode(lease))
                return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "command.code"});
        for (const auto& entry : draft.sessions)
            if (!entry || !entry->usesCode(lease))
                return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "session.code"});
        for (const auto& entry : draft.views)
            if (!entry || !entry->usesCode(lease))
                return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "view.code"});
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
    const lux::project::MetadataIdentity& EditorExtension::identity() const noexcept
    {
        return identity_;
    }
    const std::shared_ptr<const void>& EditorExtension::code() const noexcept
    {
        return code_;
    }
}
