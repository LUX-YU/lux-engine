#include <lux/engine/runtime/extensions/ExtensionModuleManager.hpp>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <unordered_map>
#include <utility>

namespace lux::extensions
{
    namespace
    {
        [[nodiscard]] ExtensionId copyId(lux::cxx::AbiStringView id)
        {
            return ExtensionId{id.view()};
        }

        [[nodiscard]] bool sameId(
            const ExtensionId& lhs,
            ExtensionIdView rhs) noexcept
        {
            return sameStableId(lhs.view(), rhs);
        }

        [[nodiscard]] ExtensionModuleLoadFailure loadFailure(
            EExtensionModuleLoadError code,
            const ExtensionModuleRequirement& requirement,
            std::string detail = {})
        {
            return ExtensionModuleLoadFailure{
                code,
                requirement.id,
                {},
                std::move(detail)};
        }
    }

    lux::cxx::expected<
        PreparedExtensionModule,
        ExtensionModuleLoadFailure>
    ExtensionModuleManager::prepare(
        const ExtensionModuleRequirement& requirement) noexcept
    {
        if (!requirement.id.isValid() ||
            !isCanonicalStableName(requirement.id.name()) ||
            !requirement.source.valid())
        {
            return lux::cxx::unexpected(loadFailure(
                EExtensionModuleLoadError::INVALID_REQUIREMENT,
                requirement));
        }

        lux::engine::platform::DynamicLibrary library;
        const bool loaded =
            requirement.source.kind() == EExtensionModuleSource::FILE_PATH
            ? library.load(requirement.source.path())
            : library.load_from_memory(
                  requirement.source.image().view(),
                  requirement.source.hint());
        if (!loaded)
        {
            return lux::cxx::unexpected(loadFailure(
                EExtensionModuleLoadError::LIBRARY_LOAD_FAILED,
                requirement,
                library.last_error()));
        }

        auto* get_descriptor =
            library.get_symbol<GetExtensionModuleV5Fn>(
                kGetExtensionModuleV5Symbol);
        if (!get_descriptor)
        {
            return lux::cxx::unexpected(loadFailure(
                EExtensionModuleLoadError::DESCRIPTOR_SYMBOL_MISSING,
                requirement));
        }
        const auto* descriptor = get_descriptor();
        if (!descriptor)
        {
            return lux::cxx::unexpected(loadFailure(
                EExtensionModuleLoadError::NULL_DESCRIPTOR,
                requirement));
        }
        constexpr std::size_t kRequiredSize =
            offsetof(ExtensionModuleDescriptorV5, dependency_count) +
            sizeof(decltype(ExtensionModuleDescriptorV5::dependency_count));
        if (descriptor->struct_size < kRequiredSize)
        {
            return lux::cxx::unexpected(loadFailure(
                EExtensionModuleLoadError::DESCRIPTOR_TOO_SMALL,
                requirement));
        }
        if (descriptor->extension_abi != kExtensionAbiV5)
        {
            return lux::cxx::unexpected(loadFailure(
                EExtensionModuleLoadError::EXTENSION_ABI_MISMATCH,
                requirement));
        }
        if (descriptor->engine_abi_fingerprint !=
            kEngineExtensionAbiFingerprint)
        {
            return lux::cxx::unexpected(loadFailure(
                EExtensionModuleLoadError::ENGINE_ABI_MISMATCH,
                requirement));
        }
        if (descriptor->id.view().empty() ||
            !isCanonicalStableName(descriptor->id.view()))
        {
            return lux::cxx::unexpected(loadFailure(
                EExtensionModuleLoadError::INVALID_DESCRIPTOR_ID,
                requirement));
        }
        if (requirement.id.name() != descriptor->id.view())
        {
            return lux::cxx::unexpected(loadFailure(
                EExtensionModuleLoadError::MANIFEST_ID_MISMATCH,
                requirement));
        }
        if (descriptor->target != requirement.target)
        {
            return lux::cxx::unexpected(loadFailure(
                EExtensionModuleLoadError::TARGET_MISMATCH,
                requirement));
        }
        if (!satisfiesExtensionVersion(
                descriptor->version,
                requirement.required_major,
                requirement.minimum_minor))
        {
            return lux::cxx::unexpected(loadFailure(
                EExtensionModuleLoadError::VERSION_MISMATCH,
                requirement));
        }

        std::vector<ExtensionDependency> dependencies;
        constexpr std::size_t kMaximumDependencies = 1024u;
        if (descriptor->dependency_count > kMaximumDependencies ||
            (descriptor->dependency_count != 0u && !descriptor->dependencies))
        {
            return lux::cxx::unexpected(loadFailure(
                EExtensionModuleLoadError::INVALID_DEPENDENCY,
                requirement));
        }
        dependencies.reserve(descriptor->dependency_count);
        for (std::size_t index = 0u;
             index < descriptor->dependency_count;
             ++index)
        {
            const auto& dependency = descriptor->dependencies[index];
            if (dependency.id.view().empty() ||
                !isCanonicalStableName(dependency.id.view()) ||
                dependency.id.view() == descriptor->id.view())
            {
                auto failure = loadFailure(
                    EExtensionModuleLoadError::INVALID_DEPENDENCY,
                    requirement);
                failure.dependency = copyId(dependency.id);
                return lux::cxx::unexpected(std::move(failure));
            }
            dependencies.push_back(ExtensionDependency{
                copyId(dependency.id),
                dependency.required_major,
                dependency.minimum_minor});
        }

        auto module = std::make_shared<ModuleLifetime>(
            std::move(library),
            copyId(descriptor->id),
            descriptor->version,
            requirement.source.origin());

        PreparedExtensionModule result;
        result.module_ = std::move(module);
        result.target_ = descriptor->target;
        result.dependencies_ = std::move(dependencies);
        result.world_systems_entry_ =
            result.module_->library().get_symbol<
                InstallWorldSystemsV5Fn>(
                kInstallWorldSystemsV5Symbol);
        result.render_features_entry_ =
            result.module_->library().get_symbol<
                InstallRenderFeaturesV5Fn>(
                kInstallRenderFeaturesV5Symbol);
        result.editor_entry_ =
            result.module_->library().get_symbol<
                RegisterEditorContributionsV5Fn>(
                kRegisterEditorContributionsV5Symbol);
        return result;
    }

    lux::cxx::expected<
        std::vector<ModuleLease>,
        ExtensionModuleCommitFailure>
    ExtensionModuleManager::commitBatch(
        std::vector<PreparedExtensionModule> modules) noexcept
    {
        if (closed_)
        {
            return lux::cxx::unexpected(ExtensionModuleCommitFailure{
                EExtensionModuleCommitError::MANAGER_CLOSED});
        }
        if (modules.empty())
            return std::vector<ModuleLease>{};

        std::unordered_map<std::uint64_t, std::size_t> pending_by_hash;
        pending_by_hash.reserve(modules.size());
        for (std::size_t index = 0u; index < modules.size(); ++index)
        {
            const auto& prepared = modules[index];
            if (!prepared || !prepared.module_->id().isValid())
            {
                return lux::cxx::unexpected(ExtensionModuleCommitFailure{
                    EExtensionModuleCommitError::INVALID_PREPARED_MODULE});
            }
            const auto id = prepared.module_->id().view();
            for (const auto& existing : records_)
            {
                const auto existing_id = existing.module->id().view();
                if (existing_id.hash() != id.hash())
                    continue;
                return lux::cxx::unexpected(ExtensionModuleCommitFailure{
                    existing_id.name() == id.name()
                        ? EExtensionModuleCommitError::DUPLICATE_EXTENSION
                        : EExtensionModuleCommitError::HASH_COLLISION,
                    prepared.module_->id()});
            }
            const auto [found, inserted] = pending_by_hash.emplace(
                id.hash(),
                index);
            if (!inserted)
            {
                const auto other_id = modules[found->second].module_->id().view();
                return lux::cxx::unexpected(ExtensionModuleCommitFailure{
                    other_id.name() == id.name()
                        ? EExtensionModuleCommitError::DUPLICATE_EXTENSION
                        : EExtensionModuleCommitError::HASH_COLLISION,
                    prepared.module_->id()});
            }
        }

        std::vector<std::size_t> indegree(modules.size(), 0u);
        std::vector<std::vector<std::size_t>> dependents(modules.size());
        for (std::size_t index = 0u; index < modules.size(); ++index)
        {
            for (const auto& dependency : modules[index].dependencies_)
            {
                const Record* existing = findRecord(dependency.id.view());
                if (existing)
                {
                    if (existing->state != EExtensionModuleState::READY)
                    {
                        return lux::cxx::unexpected(
                            ExtensionModuleCommitFailure{
                                EExtensionModuleCommitError::
                                    DEPENDENCY_NOT_READY,
                                modules[index].module_->id(),
                                dependency.id});
                    }
                    if (!satisfiesExtensionVersion(
                            existing->module->version(),
                            dependency.required_major,
                            dependency.minimum_minor))
                    {
                        return lux::cxx::unexpected(
                            ExtensionModuleCommitFailure{
                                EExtensionModuleCommitError::
                                    DEPENDENCY_VERSION_MISMATCH,
                                modules[index].module_->id(),
                                dependency.id});
                    }
                    continue;
                }
                const auto pending = pending_by_hash.find(dependency.id.hash());
                if (pending == pending_by_hash.end() ||
                    !sameId(
                        modules[pending->second].module_->id(),
                        dependency.id.view()))
                {
                    return lux::cxx::unexpected(
                        ExtensionModuleCommitFailure{
                            EExtensionModuleCommitError::MISSING_DEPENDENCY,
                            modules[index].module_->id(),
                            dependency.id});
                }
                const auto dependency_index = pending->second;
                if (!satisfiesExtensionVersion(
                        modules[dependency_index].module_->version(),
                        dependency.required_major,
                        dependency.minimum_minor))
                {
                    return lux::cxx::unexpected(
                        ExtensionModuleCommitFailure{
                            EExtensionModuleCommitError::
                                DEPENDENCY_VERSION_MISMATCH,
                            modules[index].module_->id(),
                            dependency.id});
                }
                ++indegree[index];
                dependents[dependency_index].push_back(index);
            }
        }

        std::deque<std::size_t> ready;
        for (std::size_t index = 0u; index < indegree.size(); ++index)
            if (indegree[index] == 0u)
                ready.push_back(index);
        std::vector<std::size_t> order;
        order.reserve(modules.size());
        while (!ready.empty())
        {
            const auto index = ready.front();
            ready.pop_front();
            order.push_back(index);
            for (const auto dependent : dependents[index])
            {
                if (--indegree[dependent] == 0u)
                    ready.push_back(dependent);
            }
        }
        if (order.size() != modules.size())
        {
            return lux::cxx::unexpected(ExtensionModuleCommitFailure{
                EExtensionModuleCommitError::DEPENDENCY_CYCLE});
        }

        records_.reserve(records_.size() + modules.size());
        std::vector<ModuleLease> committed;
        committed.reserve(modules.size());
        for (const auto index : order)
        {
            auto& prepared = modules[index];
            committed.push_back(prepared.module_);
            records_.push_back(Record{
                std::move(prepared.module_),
                prepared.target_,
                EExtensionModuleState::LOADED,
                std::move(prepared.dependencies_),
                prepared.world_systems_entry_,
                prepared.render_features_entry_,
                prepared.editor_entry_});
        }
        return committed;
    }

    ExtensionModuleManager::Record* ExtensionModuleManager::findRecord(
        ExtensionIdView id) noexcept
    {
        const auto found = std::ranges::find_if(
            records_,
            [id](const Record& record) noexcept
            {
                return sameStableId(record.module->id().view(), id);
            });
        return found == records_.end() ? nullptr : &*found;
    }

    const ExtensionModuleManager::Record* ExtensionModuleManager::findRecord(
        ExtensionIdView id) const noexcept
    {
        const auto found = std::ranges::find_if(
            records_,
            [id](const Record& record) noexcept
            {
                return sameStableId(record.module->id().view(), id);
            });
        return found == records_.end() ? nullptr : &*found;
    }

    ModuleLease ExtensionModuleManager::find(ExtensionIdView id) const noexcept
    {
        const auto* record = findRecord(id);
        return record ? record->module : ModuleLease{};
    }

    EExtensionRequirementStatus ExtensionModuleManager::requirementStatus(
        ExtensionIdView id,
        std::uint16_t required_major,
        std::uint16_t minimum_minor) const noexcept
    {
        for (const auto& record : records_)
        {
            const auto candidate = record.module->id().view();
            if (candidate.hash() != id.hash())
                continue;
            if (candidate.name() != id.name())
                return EExtensionRequirementStatus::ID_COLLISION;
            if (!satisfiesExtensionVersion(
                    record.module->version(),
                    required_major,
                    minimum_minor))
            {
                return EExtensionRequirementStatus::VERSION_MISMATCH;
            }
            return record.state == EExtensionModuleState::READY
                ? EExtensionRequirementStatus::READY
                : EExtensionRequirementStatus::NOT_READY;
        }
        return EExtensionRequirementStatus::MISSING;
    }

    ExtensionModuleEntrypoints ExtensionModuleManager::entrypoints(
        ExtensionIdView id) const noexcept
    {
        const auto* record = findRecord(id);
        if (!record)
            return {};
        return {
            record->world_systems_entry,
            record->render_features_entry,
            record->editor_entry,
            record->module};
    }

    std::vector<ExtensionModuleSnapshot>
    ExtensionModuleManager::snapshot() const
    {
        std::vector<ExtensionModuleSnapshot> result;
        result.reserve(records_.size());
        for (const auto& record : records_)
        {
            result.push_back(ExtensionModuleSnapshot{
                record.module->id(),
                record.module->version(),
                record.module->origin(),
                record.target,
                record.state,
                record.dependencies});
        }
        return result;
    }

    bool ExtensionModuleManager::beginRegistration(ExtensionIdView id) noexcept
    {
        auto* record = findRecord(id);
        if (!record || record->state != EExtensionModuleState::LOADED)
            return false;
        record->state = EExtensionModuleState::REGISTERING;
        return true;
    }

    bool ExtensionModuleManager::markReady(ExtensionIdView id) noexcept
    {
        auto* record = findRecord(id);
        if (!record || record->state != EExtensionModuleState::REGISTERING)
            return false;
        record->state = EExtensionModuleState::READY;
        return true;
    }

    bool ExtensionModuleManager::markFailed(ExtensionIdView id) noexcept
    {
        auto* record = findRecord(id);
        if (!record || record->state == EExtensionModuleState::READY)
            return false;
        record->state = EExtensionModuleState::FAILED;
        return true;
    }

    lux::cxx::expected<void, EExtensionModuleCloseError>
    ExtensionModuleManager::close() noexcept
    {
        if (closed_)
        {
            return lux::cxx::unexpected(
                EExtensionModuleCloseError::ALREADY_CLOSED);
        }
        for (const auto& record : records_)
        {
            if (record.module.use_count() != 1u)
            {
                return lux::cxx::unexpected(
                    EExtensionModuleCloseError::MODULE_IN_USE);
            }
        }
        records_.clear();
        closed_ = true;
        return {};
    }
}
