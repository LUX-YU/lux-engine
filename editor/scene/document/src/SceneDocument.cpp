#include <lux/engine/editor/SceneDocument.hpp>
#include <lux/engine/editor/SceneEditing.hpp>
#include <set>

namespace lux::editor
{
    namespace ecs = simulation::ecs;

    SceneDocument::SceneDocument(
        scene::SceneRuntime& runtime,
        scene::SceneInstanceId instance,
        ecs::ComponentSchemaSet schemas,
        scene::ScenePackage package
    )
        : runtime_(runtime), instance_(instance), schemas_(std::move(schemas)), baseline_(std::move(package))
    {
    }

    asset::AssetId SceneDocument::id() const noexcept
    {
        return baseline_.scene->id();
    }

    scene::SceneInstanceId SceneDocument::instance() const noexcept
    {
        return instance_;
    }

    std::uint64_t SceneDocument::revision() const noexcept
    {
        return revision_;
    }

    bool SceneDocument::dirty() const noexcept
    {
        return revision_ != saved_revision_;
    }

    bool SceneDocument::readOnly() const noexcept
    {
        return read_only_;
    }

    void SceneDocument::setReadOnly(bool value) noexcept
    {
        read_only_ = value;
    }

    void SceneDocument::changedRevision() noexcept
    {
        ++revision_;
        // Notification does not own the edit result. Revision remains queryable if a queued receiver is full.
        static_cast<void>(emit(changed));
    }

    SceneEditResult<VSceneEdit> SceneDocument::execute(const VSceneEdit& operation) noexcept
    {
        if (read_only_)
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::READ_ONLY});
        }
        SceneEditResult<VSceneEdit> result = cxx::unexpected(VSceneEditFailure{ESceneEditError::NOT_RESIDENT});
        auto apply = [&](ecs::Registry& registry) noexcept
        {
            auto* residency = registry.ctx().find<scene::WorldResidency>();
            if (!residency)
            {
                return;
            }
            std::set<std::uint32_t> affected;
            std::visit(
                [&](const auto& change) noexcept
                {
                    using Change = std::remove_cvref_t<decltype(change)>;
                    if constexpr (std::is_same_v<Change, SceneCreateObjects>)
                    {
                        for (const auto& object : change.objects)
                        {
                            affected.insert(object.partition.value);
                        }
                    }
                    else
                    {
                        const auto add = [&](world::WorldObjectId id) noexcept
                        {
                            partition::PartitionOrdinal partition;
                            if (residency->partitionOf(residency->identities().entity(id), partition))
                            {
                                affected.insert(partition.value);
                            }
                        };
                        if constexpr (std::is_same_v<Change, SceneEraseObjects>)
                        {
                            for (const auto id : change.objects)
                            {
                                add(id);
                            }
                        }
                        else
                        {
                            for (const auto& component : change.components)
                            {
                                add(component.object);
                            }
                        }
                    }
                },
                operation
            );
            result = detail::applySceneEdit(registry, *residency, schemas_, operation);
            if (!result)
            {
                return;
            }
            // An undo may need this partition after saving. Retain only partitions actually changed.
            for (const auto partition : affected)
            {
                if (!retained_partitions_.contains(partition))
                {
                    auto retained = residency->retain({partition});
                    if (!retained)
                    {
                        std::terminate(); // Same non-suspending structural exclusion proved residency.
                    }
                    retained_partitions_.emplace(partition, std::move(*retained));
                }
            }
        };
        auto edited = runtime_.editInstance(instance_, apply);
        if (!edited)
        {
            return cxx::unexpected(VSceneEditFailure{edited.error()});
        }
        return result;
    }

    SceneEditResult<void> SceneDocument::apply(const SceneEdit& edit) noexcept
    {
        if (edit.based_on != revision_)
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::STALE_REVISION});
        }
        auto inverse = execute(edit.operation);
        if (!inverse)
        {
            return cxx::unexpected(inverse.error());
        }
        history_.erase(history_.begin() + history_position_, history_.end());
        history_.push_back({edit.operation, std::move(*inverse)});
        history_position_ = history_.size();
        changedRevision();
        return {};
    }

    SceneEditResult<void> SceneDocument::undo() noexcept
    {
        if (history_position_ == 0)
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::NO_HISTORY});
        }
        auto applied = execute(history_[history_position_ - 1].inverse);
        if (!applied)
        {
            return cxx::unexpected(applied.error());
        }
        --history_position_;
        changedRevision();
        return {};
    }

    SceneEditResult<void> SceneDocument::redo() noexcept
    {
        if (history_position_ == history_.size())
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::NO_HISTORY});
        }
        auto applied = execute(history_[history_position_].forward);
        if (!applied)
        {
            return cxx::unexpected(applied.error());
        }
        ++history_position_;
        changedRevision();
        return {};
    }

    SceneEditResult<SceneObjectData> SceneDocument::object(world::WorldObjectId id) const noexcept
    {
        auto borrowed = std::as_const(runtime_).borrowInstance(instance_);
        if (!borrowed)
        {
            return cxx::unexpected(VSceneEditFailure{borrowed.error()});
        }
        const auto& registry = borrowed->get();
        const auto* residency = registry.ctx().find<scene::WorldResidency>();
        if (!residency)
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::NOT_RESIDENT});
        }
        return detail::captureSceneObject(registry, *residency, schemas_, id);
    }

    SceneEditResult<std::vector<world::WorldObjectId>> SceneDocument::objects() const noexcept
    {
        auto borrowed = std::as_const(runtime_).borrowInstance(instance_);
        if (!borrowed)
        {
            return cxx::unexpected(VSceneEditFailure{borrowed.error()});
        }
        const auto* residency = borrowed->get().ctx().find<scene::WorldResidency>();
        if (!residency)
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::NOT_RESIDENT});
        }
        std::vector<world::WorldObjectId> result;
        result.reserve(residency->identities().size());
        for (const auto& [id, entity] : residency->identities().entries())
        {
            result.push_back(id);
        }
        std::ranges::sort(result, world::WorldObjectIdLess{});
        return result;
    }

    SceneEditResult<scene::ScenePackage> SceneDocument::capture() const noexcept
    {
        auto borrowed = std::as_const(runtime_).borrowInstance(instance_);
        if (!borrowed)
        {
            return cxx::unexpected(VSceneEditFailure{borrowed.error()});
        }
        const auto& registry = borrowed->get();
        const auto* residency = registry.ctx().find<scene::WorldResidency>();
        if (!residency)
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::NOT_RESIDENT});
        }
        auto partitions = baseline_.partitions;
        for (const auto& resident : residency->resident())
        {
            auto captured = detail::captureScenePartition(registry, *residency, schemas_, resident.partition);
            if (!captured)
            {
                return cxx::unexpected(captured.error());
            }
            const auto found = std::ranges::find_if(
                partitions,
                [&](const auto& item) noexcept { return item->partition() == resident.partition; }
            );
            if (found == partitions.end())
            {
                partitions.push_back(std::move(*captured));
            }
            else
            {
                *found = std::move(*captured);
            }
        }
        auto package = scene::assembleScenePackage(baseline_, partitions);
        if (!package)
        {
            return cxx::unexpected(VSceneEditFailure{package.error()});
        }
        return std::move(*package);
    }

    SceneEditResult<void> SceneDocument::saved(scene::ScenePackage package, std::uint64_t captured_revision) noexcept
    {
        if (captured_revision > revision_ || captured_revision < saved_revision_ || !package.scene ||
            package.scene->id() != id())
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::STALE_REVISION});
        }
        baseline_ = std::move(package);
        saved_revision_ = captured_revision;
        static_cast<void>(emit(changed));
        return {};
    }

    const scene::ScenePackage& SceneDocument::baseline() const noexcept
    {
        return baseline_;
    }
} // namespace lux::editor
