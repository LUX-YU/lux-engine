#include <algorithm>
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/editor/extensions/EditorExtension.hpp>

namespace lux::editor::extensions
{
    namespace
    {
        bool validCounts(ContributionCounts counts) noexcept
        {
            return counts.commands <= 256 && counts.sessions <= 256 && counts.reflection <= 256 &&
                   counts.settings <= 256 && counts.services <= 256 && counts.ui <= 256;
        }
        lux::project::PluginResult<void> validateTable(
            const lux::project::MetadataIdentity& identity,
            const EditorExtensionExports* table
        )
        {
            const auto fail = [&](lux::project::EPluginError code, std::string subject)
            { return cxx::unexpected(lux::project::PluginFailure{code, identity.id, std::move(subject)}); };
            const bool invalid_header = !table || table->structure_size != sizeof(EditorExtensionExports) ||
                                        table->interface_version != kEditorExtensionVersion;
            if (invalid_header)
            {
                return fail(lux::project::EPluginError::INVALID_EXPORT, "editor.header");
            }
            if (!table->editor_sdk_abi || std::string_view(table->editor_sdk_abi) != kEditorExtensionAbi)
            {
                return fail(lux::project::EPluginError::ABI_MISMATCH, "editor.sdk");
            }
            const bool has_activation = table->activate != nullptr;
            const auto requirements = table->requires_capabilities;
            const bool has_requirements = requirements.sessions || requirements.project || requirements.workbench;
            const bool has_activation_entries = table->activation_counts != ContributionCounts{};
            const bool is_invalid_activation = !has_activation && (has_requirements || has_activation_entries);
            const bool invalid_counts = !validCounts(table->counts) || !validCounts(table->activation_counts) ||
                                        !table->contribute || is_invalid_activation;
            if (invalid_counts)
            {
                return fail(lux::project::EPluginError::INVALID_EXPORT, "editor.counts");
            }
            return {};
        }
        ContributionResult<ContributionDraft> normalizeDraft(
            lux::object::CodeLease lease,
            ContributionCounts counts,
            ContributionDraft incoming
        )
        {
            auto draft = std::move(incoming);
            const bool mismatch =
                counts.commands != draft.commands.size() || counts.sessions != draft.sessions.size() ||
                counts.reflection != draft.reflection.size() || counts.settings != draft.settings.size() ||
                counts.services != draft.services.size() || counts.ui != draft.ui.size();
            if (mismatch)
            {
                return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "extension.counts"});
            }
            draft.code.push_back(lease);
            for (const auto& entry : draft.services)
            {
                if (!entry || !entry->code().sameOwner(lease))
                {
                    return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "service.code"});
                }
            }
            for (const auto& entry : draft.ui)
            {
                if (!entry || !entry->code().sameOwner(lease))
                {
                    return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "ui.code"});
                }
            }
            for (const auto& entry : draft.reflection)
            {
                if (!entry.code.sameOwner(lease))
                {
                    return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "reflection.code"}
                    );
                }
            }
            for (const auto& entry : draft.commands)
            {
                if (!entry || !entry->usesCode(lease))
                {
                    return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "command.code"});
                }
            }
            for (const auto& entry : draft.sessions)
            {
                if (!entry || !entry->usesCode(lease))
                {
                    return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "session.code"});
                }
            }
            for (const auto& item : draft.settings)
            {
                if (!item.entry || !item.entry->usesCode(lease))
                {
                    return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "settings.code"});
                }
            }
            return draft;
        }
    } // namespace
    lux::project::PluginResult<EditorExtension> EditorExtension::fromStatic(const EditorModuleDescriptor& module)
    {
        const bool is_invalid_identity = module.name.empty() || !module.version;
        const bool is_invalid_export = module.exports == nullptr;
        const bool is_invalid_module = is_invalid_identity || is_invalid_export;
        if (is_invalid_module)
        {
            return cxx::unexpected(lux::project::PluginFailure{
                lux::project::EPluginError::INVALID_EXPORT,
                std::string(module.name),
                "editor.module"
            });
        }
        EditorExtension result;
        result.identity_ = {std::string(module.name), module.version};
        const auto* table = module.exports();
        if (auto valid = validateTable(result.identity_, table); !valid)
        {
            return cxx::unexpected(std::move(valid.error()));
        }
        result.exports_ = table;
        return result;
    }
    lux::project::PluginResult<EditorExtension> EditorExtension::load(
        const lux::project::PluginDescription& description,
        const lux::project::PluginLibrary& runtime,
        std::span<const EditorExtension> dependencies
    )
    {
        const auto fail = [&](lux::project::EPluginError code, std::string subject)
        { return cxx::unexpected(lux::project::PluginFailure{code, description.identity.id, std::move(subject)}); };
        if (runtime.identity() != description.identity)
        {
            return fail(lux::project::EPluginError::MODULE_MISMATCH, "runtime");
        }
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
            {
                return fail(lux::project::EPluginError::MISSING_DEPENDENCY, identity.id);
            }
            pins.push_back(found->code());
        }
        auto library = lux::project::loadPluginLibrary(description, *description.editor_library, pins);
        if (!library)
        {
            return cxx::unexpected(library.error());
        }
        const auto get = (*library)->get_symbol<GetEditorExtension>(kEditorExtensionSymbol);
        if (!get)
        {
            return fail(lux::project::EPluginError::MISSING_EXPORT, kEditorExtensionSymbol);
        }
        const auto* table = get();
        if (auto valid = validateTable(description.identity, table); !valid)
        {
            return cxx::unexpected(std::move(valid.error()));
        }
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
        if (!pinned && !exports_)
        {
            return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "extension"});
        }
        const auto lease = pinned ? lux::object::CodeLease::plugin(pinned) : lux::object::CodeLease::builtin();
        ContributionDraft draft;
        if (!table)
        {
            return draft;
        }
        try
        {
            auto result = table->contribute(draft, lease);
            if (!result)
            {
                return cxx::unexpected(result.error());
            }
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            return cxx::unexpected(ContributionFailure{EContributionError::CALLBACK, "extension.contribute"});
        }
        return normalizeDraft(lease, table->counts, std::move(draft));
    }
    ContributionResult<ContributionDraft> EditorExtension::activate(const ExtensionCapabilities& supplied) const
    {
        if (std::this_thread::get_id() != owner_)
        {
            return cxx::unexpected(ContributionFailure{EContributionError::WRONG_THREAD, "extension.activate"});
        }
        const auto pinned = code_;
        if (!pinned && !exports_)
        {
            return cxx::unexpected(ContributionFailure{EContributionError::INVALID_ARGUMENT, "extension"});
        }
        const auto* table = exports_;
        if (!table || !table->activate)
        {
            return ContributionDraft{};
        }
        const auto requirements = table->requires_capabilities;
        const bool is_missing_sessions = requirements.sessions && !supplied.sessions;
        const bool is_missing_project = requirements.project && !supplied.project;
        const bool is_missing_workbench = requirements.workbench && !supplied.workbench;
        if (is_missing_sessions || is_missing_project || is_missing_workbench)
        {
            return cxx::unexpected(ContributionFailure{
                EContributionError::UNAVAILABLE,
                "extension.capabilities",
                0,
                is_missing_sessions  ? "sessions"
                : is_missing_project ? "project"
                                     : "workbench"
            });
        }
        const ExtensionCapabilities selected{
            requirements.sessions ? supplied.sessions : nullptr,
            requirements.project ? supplied.project : nullptr,
            requirements.workbench ? supplied.workbench : nullptr
        };
        const auto lease = pinned ? lux::object::CodeLease::plugin(pinned) : lux::object::CodeLease::builtin();
        ContributionDraft draft;
        try
        {
            auto result = table->activate(draft, lease, selected);
            if (!result)
            {
                return cxx::unexpected(result.error());
            }
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            return cxx::unexpected(ContributionFailure{EContributionError::CALLBACK, "extension.activate"});
        }
        return normalizeDraft(lease, table->activation_counts, std::move(draft));
    }
    const lux::project::MetadataIdentity& EditorExtension::identity() const noexcept
    {
        return identity_;
    }
    const std::shared_ptr<const void>& EditorExtension::code() const noexcept
    {
        return code_;
    }
    lux::project::PluginResult<std::vector<EditorExtension>> loadStaticEditorModules(
        std::span<GetEditorModule* const> modules
    )
    {
        std::vector<EditorExtension> result;
        result.reserve(modules.size());
        for (const auto get : modules)
        {
            if (!get)
            {
                return cxx::unexpected(
                    lux::project::PluginFailure{lux::project::EPluginError::INVALID_EXPORT, {}, "editor.module"}
                );
            }
            const auto& descriptor = get();
            const bool is_duplicate = std::ranges::any_of(
                result,
                [&](const auto& previous) { return previous.identity().id == descriptor.name; }
            );
            if (is_duplicate)
            {
                return cxx::unexpected(lux::project::PluginFailure{
                    lux::project::EPluginError::INVALID_EXPORT,
                    std::string(descriptor.name),
                    "editor.module.duplicate"
                });
            }
            auto module = EditorExtension::fromStatic(descriptor);
            if (!module)
            {
                return cxx::unexpected(std::move(module.error()));
            }
            result.push_back(std::move(*module));
        }
        return result;
    }
} // namespace lux::editor::extensions
