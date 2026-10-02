#include <lux/engine/editor/scene/ModelCreationOperation.hpp>
#include <lux/engine/editor/scene/SceneAlgorithms.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <algorithm>

namespace lux::editor::scene
{
    namespace
    {
        template <class Error> auto rejected(Error error)
        {
            return cxx::unexpected(ModelCreationFailure{std::move(error)});
        }
        bool busy(const SceneEditError& error) noexcept
        {
            return error.code == ESceneEditError::BUSY ||
                   (error.code == ESceneEditError::SESSION && error.session == sessions::ESessionError::BUSY);
        }
    }
    struct ModelCreationOperation::Impl final
    {
        using Model = std::shared_ptr<const asset::ModelAsset>;
        using Loaded = cxx::expected<Model, process::asset_loading::AssetLoadFailure>;
        sessions::TSessionAccess<SceneSession> sessions_;
        project::ProjectCatalogModel& catalog_;
        project::ProjectCatalogSnapshot snapshot_;
        simulation::ecs::ComponentSchemaSet schemas_;
        ModelPlacement placement_;
        std::optional<process::TTaskResult<Model, process::asset_loading::AssetLoadFailure>> loaded_;
        std::optional<ModelCreationFailure> failure_;
        std::optional<SceneEditReceipt> receipt_;
        bool cancelled_{}, committing_{};
        process::TaskScope tasks_;
        Impl(
            process::ExecutionRuntime& execution,
            sessions::TSessionAccess<SceneSession> sessions,
            project::ProjectCatalogModel& catalog,
            project::ProjectCatalogSnapshot snapshot,
            simulation::ecs::ComponentSchemaSet schemas,
            ModelPlacement placement
        )
            : sessions_(sessions), catalog_(catalog), snapshot_(std::move(snapshot)), schemas_(std::move(schemas)),
              placement_(std::move(placement)), tasks_(execution)
        {}
        ~Impl() noexcept
        {
            tasks_.requestStop();
            (void)tasks_.join(); // Only owning transport results can arrive; no model or UI callback.
        }
        ModelCreationResult<SceneEditReceipt> fail(ModelCreationFailure failure)
        {
            failure_ = std::move(failure);
            return cxx::unexpected(*failure_);
        }
        EModelCreationStage stage() const noexcept
        {
            if (receipt_)
                return EModelCreationStage::INSERTED;
            if (cancelled_)
                return EModelCreationStage::CANCELLED;
            if (failure_ || (loaded_ && !*loaded_))
                return EModelCreationStage::FAILED;
            return loaded_ ? EModelCreationStage::READY : EModelCreationStage::READING;
        }
        ModelCreationResult<SceneEditReceipt> commit()
        {
            if (committing_)
                return rejected(SceneEditError{ESceneEditError::BUSY});
            if (receipt_)
                return *receipt_;
            if (failure_)
                return cxx::unexpected(*failure_);
            if (cancelled_)
                return rejected(process::TaskCancelled{});
            if (!loaded_)
                return rejected(SceneEditError{ESceneEditError::BUSY});
            if (!*loaded_)
            {
                const auto& error = loaded_->error();
                if (const auto* domain = error.domainFailure())
                    return fail({*domain});
                if (const auto* execution = error.executionFailure())
                    return fail({*execution});
                cancelled_ = true;
                return rejected(process::TaskCancelled{});
            }
            struct CommitScope final
            {
                bool& active;
                explicit CommitScope(bool& value) noexcept : active(value)
                {
                    active = true;
                }
                ~CommitScope()
                {
                    active = false;
                }
            } scope{committing_};
            auto version = catalog_.version();
            if (!version)
                return rejected(version.error());
            if (*version != snapshot_.version)
                return fail({project::VProjectQueryFailure{EAssetReferenceError::STALE_CATALOG}});
            auto session = sessions_.edit(placement_.target);
            if (!session)
            {
                SceneEditError error{session.error()};
                return busy(error) ? rejected(error) : fail({error});
            }
            const auto observed = session->get().describe().current;
            if (observed != placement_.based_on)
                return fail({SceneEditError{ESceneEditError::STALE_CONTENT}});
            const auto& model = **loaded_;
            for (const auto& primitive : model->data().primitives)
            {
                const auto exists = [&](asset::AssetId id, std::uint32_t magic) {
                    return std::ranges::any_of(snapshot_.assets, [&](const auto& entry) {
                        return entry.id == id && entry.magic == magic;
                    });
                };
                if (!exists(primitive.mesh, asset::MeshAsset::primary_magic) ||
                    !exists(primitive.material, asset::MaterialAsset::primary_magic))
                    return fail({editing::makeEditFailure(
                        editing::EEditError::PRECONDITION_FAILED,
                        static_cast<std::uint64_t>(EModelCreationError::MISSING_DEPENDENCY),
                        "model.dependencies"
                    )});
            }
            auto read = session->get().read();
            if (!read)
                return busy(read.error()) ? rejected(read.error()) : fail({read.error()});
            auto objects =
                read->withRead([&](const SceneReadView& view) -> SceneEditResult<std::vector<SceneObjectData>> {
                    auto result = makeSceneModelObjects(
                        *model,
                        placement_.position,
                        placement_.partition,
                        view.configuration().world->data(),
                        schemas_
                    );
                    if (!result)
                    {
                        SceneEditError error{ESceneEditError::HISTORY};
                        error.history = result.error();
                        return cxx::unexpected(error);
                    }
                    return std::move(*result);
                });
            if (!objects)
                return busy(objects.error()) ? rejected(objects.error()) : fail({objects.error()});
            // Component codecs can invoke extension code. Recheck the catalog after that boundary;
            // the captured model remains owned and can be retried after a temporary access failure.
            version = catalog_.version();
            if (!version)
                return rejected(version.error());
            if (*version != snapshot_.version)
                return fail({project::VProjectQueryFailure{EAssetReferenceError::STALE_CATALOG}});
            SceneEditBatch batch{placement_.based_on, "Insert model", {}};
            batch.edits.reserve(objects->size());
            for (auto& object : *objects)
                batch.edits.emplace_back(SceneCreateObject{std::move(object)});
            auto applied = session->get().apply(std::move(batch));
            if (!applied)
                return busy(applied.error()) ? rejected(applied.error()) : fail({applied.error()});
            receipt_ = *applied;
            return *receipt_;
        }
    };
    ModelCreationOperation::ModelCreationOperation(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    ModelCreationOperation::~ModelCreationOperation() noexcept = default;
    ModelCreationResult<std::unique_ptr<ModelCreationOperation>> ModelCreationOperation::start(
        process::ExecutionRuntime& execution,
        sessions::TSessionAccess<SceneSession> sessions,
        project::ProjectCatalogModel& catalog,
        process::asset_loading::AssetReadPort reads,
        simulation::ecs::ComponentSchemaSet schemas,
        ModelPlacement placement
    )
    {
        auto asset = catalog.resolve(placement.asset, asset::ModelAsset::primary_magic);
        if (!asset)
            return rejected(asset.error());
        auto snapshot = catalog.snapshot();
        if (!snapshot)
            return rejected(snapshot.error());
        auto target = sessions.describe(placement.target);
        if (!target)
            return rejected(SceneEditError{target.error()});
        if (target->current != placement.based_on)
            return rejected(SceneEditError{ESceneEditError::STALE_CONTENT});
        auto impl = std::make_unique<Impl>(
            execution,
            sessions,
            catalog,
            std::move(*snapshot),
            std::move(schemas),
            std::move(placement)
        );
        auto task = impl->tasks_.submit(
            {.name = "Read model for insertion"},
            [read = std::move(reads), cpu = execution.cpu(), id = *asset](process::TaskReporter reporter
            ) mutable noexcept {
                auto load = stdexec::then(
                    process::asset_loading::loadAsset<asset::ModelAsset>(
                        std::move(read),
                        cpu,
                        id,
                        {256U * 1024U * 1024U, 512U * 1024U * 1024U, 64},
                        reporter.stopToken()
                    ),
                    [](Impl::Model model) noexcept -> Impl::Loaded { return model; }
                );
                return stdexec::upon_error(
                    std::move(load),
                    [](process::asset_loading::AssetLoadFailure failure) noexcept -> Impl::Loaded {
                        return cxx::unexpected(std::move(failure));
                    }
                );
            },
            [owner = impl.get()](process::TTaskResult<Impl::Model, process::asset_loading::AssetLoadFailure>&& result
            ) noexcept { owner->loaded_.emplace(std::move(result)); }
        );
        if (!task)
            return rejected(task.error());
        return std::unique_ptr<ModelCreationOperation>(new ModelCreationOperation(std::move(impl)));
    }
    EModelCreationStage ModelCreationOperation::stage() const noexcept
    {
        return impl_->stage();
    }
    bool ModelCreationOperation::settled() const noexcept
    {
        return impl_->loaded_.has_value();
    }
    ModelCreationResult<SceneEditReceipt> ModelCreationOperation::commit()
    {
        return impl_->commit();
    }
    void ModelCreationOperation::cancel() noexcept
    {
        if (!impl_->committing_ && !impl_->receipt_)
        {
            impl_->cancelled_ = true;
            impl_->tasks_.requestStop();
        }
    }
    const std::optional<ModelCreationFailure>& ModelCreationOperation::failure() const noexcept
    {
        return impl_->failure_;
    }
}
