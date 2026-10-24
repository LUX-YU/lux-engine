#include <lux/engine/editor/SceneDocument.hpp>
#include <lux/engine/editor/SceneEditing.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
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

    exec::task<scene::SceneRuntimeResult<void>> SceneDocument::whenEditable() const noexcept
    {
        return runtime_.whenEditable(instance_);
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

    std::uint64_t SceneDocument::hierarchyRevision() const noexcept
    {
        return hierarchy_revision_;
    }

    bool SceneDocument::readOnly() const noexcept
    {
        return read_only_;
    }

    void SceneDocument::setReadOnly(bool value) noexcept
    {
        if (read_only_ == value)
        {
            return;
        }
        read_only_ = value;
        static_cast<void>(emit(changed));
    }

    void SceneDocument::changedRevision() noexcept
    {
        ++revision_;
        auto borrowed = std::as_const(runtime_).borrowInstance(instance_);
        const bool removed =
            selection_.valid() && borrowed &&
            borrowed->get().ctx().get<scene::WorldResidency>().identities().entity(selection_) == ecs::NullEntity;
        if (removed)
        {
            selection_ = {};
            static_cast<void>(emit(selectionChanged));
        }
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
        if (result)
        {
            const auto* write = std::get_if<SceneWriteComponents>(&operation);
            const bool structural =
                !write || std::ranges::any_of(
                              write->components,
                              [](const auto& change) noexcept { return change.schema.name == "lux.ecs.Parent"; }
                          );
            if (structural)
            {
                ++hierarchy_revision_;
            }
        }
        return result;
    }

    SceneEditResult<void> SceneDocument::apply(const SceneEdit& edit) noexcept
    {
        if (preview_)
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::READ_ONLY});
        }
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
        if (preview_)
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::READ_ONLY});
        }
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
        if (preview_)
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::READ_ONLY});
        }
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

    bool SceneDocument::previewing() const noexcept
    {
        return preview_.has_value();
    }

    SceneEditResult<void> SceneDocument::preview(const SceneEdit& edit) noexcept
    {
        if (edit.based_on != revision_)
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::STALE_REVISION});
        }
        const auto* write = std::get_if<SceneWriteComponents>(&edit.operation);
        if (!write || write->components.size() != 1 || !write->components.front().value)
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::INVALID_COMPONENT});
        }
        if (preview_)
        {
            const auto& before = std::get<SceneWriteComponents>(preview_->forward).components.front();
            const auto& next = write->components.front();
            if (before.object != next.object || before.schema != next.schema)
            {
                return cxx::unexpected(VSceneEditFailure{ESceneEditError::INVALID_COMPONENT});
            }
        }
        auto inverse = execute(edit.operation);
        if (!inverse)
        {
            return cxx::unexpected(inverse.error());
        }
        if (!preview_)
        {
            preview_ = History{edit.operation, std::move(*inverse)};
        }
        else
        {
            preview_->forward = edit.operation;
        }
        changedRevision();
        return {};
    }

    SceneEditResult<void> SceneDocument::finishPreview(bool commit) noexcept
    {
        if (!preview_)
        {
            return {};
        }
        if (commit)
        {
            history_.erase(history_.begin() + history_position_, history_.end());
            history_.push_back(std::move(*preview_));
            history_position_ = history_.size();
        }
        else
        {
            auto restored = execute(preview_->inverse);
            if (!restored)
            {
                return cxx::unexpected(restored.error());
            }
            changedRevision();
        }
        preview_.reset();
        static_cast<void>(emit(changed));
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

    world::WorldObjectId SceneDocument::selection() const noexcept
    {
        return selection_;
    }

    bool SceneDocument::select(world::WorldObjectId id) noexcept
    {
        if (id != world::WorldObjectId{} && !object(id))
        {
            return false;
        }
        if (selection_ != id)
        {
            selection_ = id;
            static_cast<void>(emit(selectionChanged));
        }
        return true;
    }

    bool SceneDocument::canUndo() const noexcept
    {
        return !read_only_ && !preview_ && history_position_ != 0;
    }

    bool SceneDocument::canRedo() const noexcept
    {
        return !read_only_ && !preview_ && history_position_ != history_.size();
    }

    SceneEditResult<ecs::DecodedComponent> SceneDocument::component(
        world::WorldObjectId id,
        const ecs::ComponentSchemaId& type
    ) const noexcept
    {
        auto captured = object(id);
        if (!captured)
        {
            return cxx::unexpected(captured.error());
        }
        const auto* schema = schemas_.find(type);
        const auto value = std::ranges::find(captured->components, type, &SceneComponentData::schema);
        const bool unavailable = !schema || !schema->decode_value || value == captured->components.end();
        if (unavailable)
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::INVALID_COMPONENT});
        }
        auto borrowed = std::as_const(runtime_).borrowInstance(instance_);
        if (!borrowed)
        {
            return cxx::unexpected(VSceneEditFailure{borrowed.error()});
        }
        const auto& identities = borrowed->get().ctx().get<scene::WorldResidency>().identities();
        ecs::ComponentEntityResolver resolver{
            &identities,
            [](const void* state,
               world::WorldObjectId id) noexcept -> cxx::expected<ecs::Entity, ecs::ComponentDecodeFailure>
            {
                const auto entity = static_cast<const ecs::WorldEntityMap*>(state)->entity(id);
                if (entity == ecs::NullEntity)
                {
                    return cxx::unexpected(
                        ecs::ComponentDecodeFailure{ecs::EComponentDecodeError::UNRESOLVED_REFERENCE, 0, id}
                    );
                }
                return entity;
            }
        };
        auto decoded = schema->decode_value(value->version, value->payload, resolver, schema->code_lifetime);
        if (!decoded)
        {
            return cxx::unexpected(VSceneEditFailure{decoded.error()});
        }
        return std::move(*decoded);
    }

    SceneEditResult<SceneEdit> SceneDocument::encodeComponentEdit(
        world::WorldObjectId id,
        std::uint64_t based_on,
        cxx::TypeToken type,
        const void* value
    ) const noexcept
    {
        if (based_on != revision_)
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::STALE_REVISION});
        }
        const auto* schema = schemas_.find(type);
        if (!schema || !schema->capture_value || !value)
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::INVALID_COMPONENT});
        }
        auto borrowed = std::as_const(runtime_).borrowInstance(instance_);
        if (!borrowed)
        {
            return cxx::unexpected(VSceneEditFailure{borrowed.error()});
        }
        const auto& identities = borrowed->get().ctx().get<scene::WorldResidency>().identities();
        auto captured = schema->capture_value(value, schema->code_lifetime);
        auto bytes = captured.encode(identities, 64U << 20U);
        if (!bytes)
        {
            return cxx::unexpected(VSceneEditFailure{bytes.error()});
        }
        SceneWriteComponents write;
        write.components.push_back({id, schema->id, SceneComponentData{schema->id, schema->version, std::move(*bytes)}}
        );
        return SceneEdit{based_on, std::move(write)};
    }

    SceneEditResult<world::WorldObjectId> SceneDocument::parent(world::WorldObjectId id) const noexcept
    {
        auto borrowed = std::as_const(runtime_).borrowInstance(instance_);
        if (!borrowed)
        {
            return cxx::unexpected(VSceneEditFailure{borrowed.error()});
        }
        const auto& registry = borrowed->get();
        const auto& identities = registry.ctx().get<scene::WorldResidency>().identities();
        const auto entity = identities.entity(id);
        if (entity == ecs::NullEntity)
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::INVALID_OBJECT});
        }
        const auto* value = registry.try_get<const ecs::Parent>(entity);
        return value ? identities.object(value->entity) : world::WorldObjectId{};
    }

    SceneEditResult<SceneEdit> SceneDocument::prepareParentEdit(
        world::WorldObjectId child,
        world::WorldObjectId parent,
        std::uint64_t based_on
    ) const noexcept
    {
        const auto* schema = schemas_.find(cxx::typeToken<ecs::Parent>());
        if (!schema)
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::INVALID_COMPONENT});
        }
        if (!parent.valid())
        {
            SceneWriteComponents write;
            write.components.push_back({child, schema->id, {}});
            return SceneEdit{based_on, std::move(write)};
        }
        auto borrowed = std::as_const(runtime_).borrowInstance(instance_);
        if (!borrowed)
        {
            return cxx::unexpected(VSceneEditFailure{borrowed.error()});
        }
        const auto entity = borrowed->get().ctx().get<scene::WorldResidency>().identities().entity(parent);
        if (entity == ecs::NullEntity)
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::INVALID_OBJECT});
        }
        const ecs::Parent value{entity};
        return prepareComponentEdit(child, based_on, value);
    }

    SceneEditResult<scene::ScenePackage> SceneDocument::capture() const noexcept
    {
        if (preview_)
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::READ_ONLY});
        }
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
