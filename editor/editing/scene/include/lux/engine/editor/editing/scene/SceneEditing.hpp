#pragma once
#include <lux/engine/editor/editing/scene/SceneEdit.hpp>
#include <lux/engine/scene/WorldResidency.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <functional>
#include <variant>
namespace lux::scene
{
    class SceneRuntime;
}
namespace lux::editor
{
    class ProjectStorage;
}
namespace lux::editor::scene
{
    class SceneEditing final
    {
    public:
        SceneEditing(
            lux::scene::SceneRuntime&,
            lux::scene::SceneInstanceId,
            const lux::simulation::ecs::ComponentSchemaSet&,
            editing::EditHistory&,
            const ProjectStorage* = nullptr
        ) noexcept;
        // The host may restrict admission; history replay remains tied to this instance and history.
        std::function<editing::EditResult<void>()> admission;
        std::function<void(const ComponentNotice&)> changed;
        [[nodiscard]] const ProjectStorage* project() const noexcept
        {
            return project_;
        }
        [[nodiscard]] const lux::simulation::ecs::ComponentSchemaSet& schemas() const noexcept
        {
            return schemas_;
        }
        [[nodiscard]] std::span<const ComponentNotice> componentChanges() const noexcept
        {
            return component_changes_;
        }
        // Structural EditOperations reserve before commit. Recording only changes bookkeeping;
        // the operation remains responsible for Registry writes, invalidation and publication.
        [[nodiscard]] editing::EditResult<void> prepareComponentChanges(std::size_t count) noexcept;
        void recordComponentChange(lux::simulation::ecs::Entity, lux::cxx::TypeToken, editing::Revision) noexcept;
        void forgetComponentChanges(lux::simulation::ecs::Entity) noexcept;
        void forgetComponentChange(lux::simulation::ecs::Entity, lux::cxx::TypeToken) noexcept;
        [[nodiscard]] bool active() const noexcept
        {
            return field_edit_.index() != 0;
        }
        [[nodiscard]] bool busy() const noexcept
        {
            return busy_;
        }
        void close() noexcept
        {
            closed_ = true;
        }
        [[nodiscard]] std::string_view writeRestriction() const noexcept;
        [[nodiscard]] const void* component(lux::simulation::ecs::Entity, lux::cxx::TypeToken) const noexcept;
        [[nodiscard]] std::uint64_t componentVersion(lux::simulation::ecs::Entity, lux::cxx::TypeToken) const noexcept;
        [[nodiscard]] editing::EditResult<SceneWriteTarget> writeTarget(lux::simulation::ecs::Entity) const noexcept;
        [[nodiscard]] editing::EditResult<void> canAddComponent(SceneWriteTarget, lux::cxx::TypeToken) const noexcept;
        [[nodiscard]] editing::EditResult<editing::ApplyResult> addComponent(SceneWriteTarget, lux::cxx::TypeToken);
        template <class Component, class Value, class Access>
        [[nodiscard]] editing::EditResult<editing::ApplyResult>
        setField(SceneWriteTarget, std::string_view field, std::string_view label, Access, const Value&);

        template <class Component, class Value, class Access>
        [[nodiscard]] editing::EditResult<FieldEditToken> beginFieldEdit(
            SceneWriteTarget,
            std::string origin,
            std::string_view field,
            std::string_view label,
            Access
        );

        [[nodiscard]] bool fieldEditWritable(const FieldEditToken&) const noexcept;
        [[nodiscard]] editing::EditResult<void> fieldEdited(const FieldEditToken&);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> finishFieldEdit(const FieldEditToken&);
        [[nodiscard]] editing::EditResult<void> finishFieldEdits();

    private:
        class AddComponent;
        template <class Component, class Value, class Access> friend class detail::TFieldEdit;
        [[nodiscard]] lux::world::WorldObjectId fieldIdentity(const SceneWriteTarget&) const noexcept;
        [[nodiscard]] std::vector<lux::scene::PartitionRetention> retainFieldTargets(
            const SceneWriteTarget&,
            lux::cxx::TypeToken
        ) const;
        [[nodiscard]] std::size_t fieldResidencyCount() const noexcept;
        [[nodiscard]] SceneWriteTarget replayTarget(SceneWriteTarget, lux::world::WorldObjectId) const noexcept;
        [[nodiscard]] editing::EditResult<void*> fieldAccess(const SceneWriteTarget&, lux::cxx::TypeToken, bool);
        [[nodiscard]] editing::EditResult<void> checkFieldSize(std::size_t) const noexcept;
        [[nodiscard]] editing::EditResult<void> validateFieldValue(lux::cxx::TypeToken, const void*, const void*) const;
        void fieldChanged(const SceneWriteTarget&, lux::cxx::TypeToken, bool in_progress) noexcept;
        [[nodiscard]] editing::EditResult<editing::ApplyResult> executeField(editing::EditOperationPtr&);
        [[nodiscard]] editing::EditResult<FieldEditToken>
        adoptFieldEdit(std::string origin, std::unique_ptr<detail::RegistryFieldEdit>&);

        [[nodiscard]] editing::EditResult<void> checkAdmission() const noexcept;
        [[nodiscard]] lux::simulation::ecs::Registry& registry() const noexcept;
        [[nodiscard]] lux::scene::WorldResidency* residency() const noexcept;
        [[nodiscard]] bool atSafePoint() const noexcept;
        lux::scene::SceneRuntime& runtime_;
        lux::scene::SceneInstanceId scene_;
        const lux::simulation::ecs::ComponentSchemaSet& schemas_;
        [[nodiscard]] lux::simulation::ecs::Entity resolve(lux::simulation::ecs::Entity) const noexcept;
        std::vector<ComponentNotice> component_changes_;
        std::uint64_t next_component_change_{1};
        editing::EditHistory& history_;
        const ProjectStorage* project_;
        struct FieldGesture final
        {
            FieldEditToken token;
            editing::EditOperationPtr operation;
        };
        std::variant<std::monostate, FieldGesture> field_edit_;
        std::uint64_t next_field_edit_{1};
        bool busy_{}, closed_{};
    };
}
