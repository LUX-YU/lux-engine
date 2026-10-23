#include <lux/engine/EngineContext.hpp>
#include <lux/engine/editor/AssetImport.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/ProjectAssets.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <random>

namespace lux::editor
{
    struct ProjectAssets::Impl final
    {
        execution::ExecutionRuntime& execution;
        execution::TaskScope& tasks;
        asset::AssetVfs& assets;
        std::filesystem::path root;
        detail::AssetCatalog catalog;
        std::map<asset::AssetId, asset::MountLease> mounts;
        std::map<asset::AssetId, std::shared_ptr<const asset::ModelAsset>> models;
        std::optional<AssetImportFailure> failure;
        execution::TaskId pending;
        bool ready{};
        std::mt19937 random{std::random_device{}()};

        explicit Impl(EditorContext& context) noexcept
            : execution(context.engine().execution()), tasks(context.tasks()), assets(context.assets()),
              root(context.project().root)
        {
        }

        AssetImportResult<void> adopt(detail::ImportedAssets result) noexcept
        {
            std::vector<asset::AssetId> removed_ids;
            std::vector<asset::MountLease> removed;
            for (auto entry = mounts.begin(); entry != mounts.end();)
            {
                const bool replaced =
                    result.complete_catalog ||
                    std::ranges::any_of(result.models, [&](const auto& model) { return model->id() == entry->first; });
                if (!replaced)
                {
                    ++entry;
                    continue;
                }
                removed_ids.push_back(entry->first);
                removed.push_back(std::move(entry->second));
                entry = mounts.erase(entry);
            }
            auto published = assets.replaceMounts(removed, result.mounts);
            if (!published)
            {
                for (std::size_t i = 0; i < removed.size(); ++i)
                {
                    mounts.emplace(removed_ids[i], std::move(removed[i]));
                }
                return cxx::unexpected(AssetImportFailure{
                    EAssetImportError::PUBLICATION,
                    "Files were published, but VFS rejected mount adoption",
                    true
                });
            }
            if (result.complete_catalog)
            {
                models.clear();
            }
            for (std::size_t i = 0; i < result.models.size(); ++i)
            {
                const auto id = result.models[i]->id();
                mounts.emplace(id, std::move((*published)[i]));
                models[id] = std::move(result.models[i]);
            }
            catalog = std::move(result.catalog);
            ready = true;
            failure.reset();
            return {};
        }

        AssetImportResult<execution::TaskId> start(
            ProjectAssets& collection,
            std::optional<ProjectAsset> input
        ) noexcept
        {
            if (pending)
            {
                return cxx::unexpected(
                    AssetImportFailure{EAssetImportError::BUSY, "Another catalog operation is active"}
                );
            }
            auto submitted = tasks.submit(
                {input ? "Import model" : "Open asset catalog", "Assets"},
                [id = collection.objectId(),
                 execution = &execution,
                 root = root,
                 expected = catalog,
                 input = std::move(input)](execution::TaskReporter reporter) mutable noexcept
                {
                    auto work = input ? detail::importModel(
                                            *execution,
                                            std::move(root),
                                            std::move(expected),
                                            std::move(*input),
                                            reporter
                                        )
                                      : detail::openAssetCatalog(*execution, std::move(root), reporter);
                    auto result = execution::taskResult(stdexec::write_env(
                        stdexec::starts_on(execution->mainScheduler(), std::move(work)),
                        stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
                    ));
                    return stdexec::then(
                        std::move(result),
                        [id](auto result) noexcept
                        {
                            auto outcome = result ? execution::ETaskState::SUCCEEDED : execution::ETaskState::FAILED;
                            // Main's existing generation check protects a closed project. No captured Context,
                            // second completion queue, or retained ProjectAssets pointer crosses the worker phase.
                            auto object = object::ObjectRuntime::instance().resolve(id);
                            if (!object)
                            {
                                return outcome;
                            }
                            auto& collection = *static_cast<ProjectAssets*>(*object);
                            auto& state = *collection.impl_;
                            state.pending = {};
                            if (result)
                            {
                                auto adopted = state.adopt(std::move(*result));
                                if (!adopted)
                                {
                                    state.failure = std::move(adopted.error());
                                    outcome = execution::ETaskState::FAILED;
                                }
                            }
                            else if (const auto* domain = result.error().domainFailure())
                            {
                                state.failure = *domain;
                            }
                            else
                            {
                                state.failure =
                                    AssetImportFailure{EAssetImportError::EXECUTION, "Asset execution failed"};
                            }
                            if (state.failure && state.failure->code == EAssetImportError::CANCELLED)
                            {
                                outcome = execution::ETaskState::CANCELLED;
                            }
                            static_cast<void>(collection.emit(collection.changed));
                            return outcome;
                        }
                    );
                }
            );
            if (!submitted)
            {
                return cxx::unexpected(AssetImportFailure{EAssetImportError::EXECUTION, "Asset task admission failed"});
            }
            pending = *submitted;
            return *submitted;
        }
    };

    ProjectAssets::ProjectAssets(EditorContext& context) noexcept : impl_(std::make_unique<Impl>(context)) {}

    ProjectAssets::~ProjectAssets() noexcept = default;

    AssetImportResult<execution::TaskId> ProjectAssets::open() noexcept
    {
        auto started = impl_->start(*this, {});
        if (!started)
        {
            impl_->failure = started.error();
        }
        return started;
    }

    AssetImportResult<execution::TaskId> ProjectAssets::importModel(
        std::filesystem::path source,
        toolchain::ModelCookConfiguration configuration
    ) noexcept
    {
        if (!source.is_absolute() || source.filename().empty())
        {
            return cxx::unexpected(
                AssetImportFailure{EAssetImportError::INVALID_INPUT, "Choose an absolute model source path"}
            );
        }
        uuids::uuid_random_generator generate{impl_->random};
        ProjectAsset input;
        input.id = asset::AssetId{generate()};
        const auto name = source.stem().u8string();
        input.name.assign(reinterpret_cast<const char*>(name.data()), name.size());
        input.source = std::move(source);
        input.configuration = std::move(configuration);
        return import(std::move(input));
    }

    AssetImportResult<execution::TaskId> ProjectAssets::reimport(asset::AssetId id) noexcept
    {
        const auto found = std::ranges::find(impl_->catalog.records, id, &ProjectAsset::id);
        if (found == impl_->catalog.records.end())
        {
            return cxx::unexpected(AssetImportFailure{EAssetImportError::INVALID_INPUT, "Unknown imported model"});
        }
        return import(*found);
    }

    AssetImportResult<execution::TaskId> ProjectAssets::import(ProjectAsset input) noexcept
    {
        if (!impl_->ready)
        {
            return cxx::unexpected(AssetImportFailure{EAssetImportError::NOT_READY, "Asset catalog has not opened"});
        }
        const bool exhausted = input.revision == std::numeric_limits<std::uint64_t>::max() ||
                               impl_->catalog.revision == std::numeric_limits<std::uint64_t>::max();
        if (exhausted)
        {
            return cxx::unexpected(AssetImportFailure{EAssetImportError::INVALID_INPUT, "Asset revision exhausted"});
        }
        uuids::uuid_random_generator generate{impl_->random};
        ++input.revision;
        input.package =
            ".luxassets/products/" + uuids::to_string(input.id.uuid()) + "/" + uuids::to_string(generate()) + ".luxpak";
        return impl_->start(*this, std::move(input));
    }

    bool ProjectAssets::ready() const noexcept
    {
        return impl_->ready;
    }

    bool ProjectAssets::busy() const noexcept
    {
        return static_cast<bool>(impl_->pending);
    }

    std::uint64_t ProjectAssets::revision() const noexcept
    {
        return impl_->catalog.revision;
    }

    std::span<const ProjectAsset> ProjectAssets::records() const noexcept
    {
        return impl_->catalog.records;
    }

    const asset::ModelAsset* ProjectAssets::model(asset::AssetId id) const noexcept
    {
        const auto found = impl_->models.find(id);
        return found == impl_->models.end() ? nullptr : found->second.get();
    }

    const std::optional<AssetImportFailure>& ProjectAssets::failure() const noexcept
    {
        return impl_->failure;
    }
} // namespace lux::editor
