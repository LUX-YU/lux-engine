#pragma once
#include <lux/engine/editor/scene/RunStore.hpp>
#include <lux/engine/editor/editing/scene/FieldEdit.hpp>
#include <lux/engine/editor/project/ProjectCatalogModel.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/cxx/core/move_only_function.hpp>

namespace lux::editor::scene
{
    // Owns only one component's UI copy and a proposed field value. Every commit reacquires the
    // pause epoch through RunStore; neither a Registry pointer nor SceneEditing escapes a call.
    class RunInspectorFields final
    {
    public:
        using Editing = RunInspectorFields;
        using Target = RunningObjectRef;
        using Status = RunResult<void>;
        using Copy = std::shared_ptr<void> (*)(const void*);
        RunInspectorFields(
            RunStore&,
            Target,
            simulation::ecs::ComponentSchema,
            Copy,
            project::ProjectCatalogModel* = {}
        );
        ~RunInspectorFields() noexcept;
        RunInspectorFields(const RunInspectorFields&) = delete;
        RunInspectorFields& operator=(const RunInspectorFields&) = delete;
        RunInspectorFields(RunInspectorFields&&) = delete;
        RunInspectorFields& operator=(RunInspectorFields&&) = delete;
        [[nodiscard]] Status refresh();
        [[nodiscard]] Status update();
        [[nodiscard]] Status cancel();
        [[nodiscard]] Status release();
        [[nodiscard]] Status withRead(cxx::function_ref<Status()>);
        [[nodiscard]] const Status& status() const noexcept
        {
            return status_;
        }
        [[nodiscard]] Target target() const noexcept
        {
            return target_;
        }
        [[nodiscard]] const void* component(Target, cxx::TypeToken) const noexcept;
        [[nodiscard]] std::uint64_t componentVersion(Target, cxx::TypeToken) const noexcept
        {
            return version_;
        }
        [[nodiscard]] std::string_view writeRestriction() const noexcept;
        [[nodiscard]] bool active() const noexcept
        {
            return !field_.empty();
        }
        [[nodiscard]] bool ownsField(std::string_view field) const noexcept
        {
            return field == field_;
        }
        [[nodiscard]] bool readOnly() const noexcept
        {
            return read_only_;
        }
        void setReadOnly(bool value) noexcept
        {
            read_only_ = value;
        }
        [[nodiscard]] project::ProjectCatalogModel* catalogAccess() const noexcept
        {
            return catalog_;
        }
        [[nodiscard]] project::ProjectCatalogModel* assetCatalog() const noexcept
        {
            return catalog_;
        }
        [[nodiscard]] const char* errorMessage() const noexcept
        {
            return message_.c_str();
        }
        void fail(const char* message);
        [[nodiscard]] bool finish();
        bool queueEdit(cxx::move_only_function<bool()>);
        static Status connectionFailure(object::EConnectError);
        static Status constructionFailure();
        template <class Value> bool changed(Value& value, const Value& before, bool modified)
        {
            if (read_only_)
                value = before;
            return modified && !read_only_;
        }
        template <class Component> const Component* read(Target target) const noexcept
        {
            return static_cast<const Component*>(component(target, cxx::typeToken<Component>()));
        }
        template <class Component, class Value, class Access>
        bool apply(
            Target target,
            const char* identity,
            const char* label,
            Access access,
            const Value& value,
            lux::ui::EditResult change
        )
        {
            if (change.cancelled)
            {
                status_ = cancel();
                return status_.has_value();
            }
            const bool invalid = target != target_ || read_only_ || !history_ || (active() && !ownsField(identity)) ||
                                 !TFieldValue<Value>::valid(value);
            if (invalid)
                return reject(editing::EEditError::INVALID_ARGUMENT);
            if (change.changed)
            {
                const auto prepare = [&]() -> Status {
                    return prepareField<Component>(target, identity, label, access, value);
                };
                status_ = withRead(prepare);
                if (!status_)
                    return false;
            }
            if (change.committed && active())
                commit_ = true;
            return true;
        }
        template <class Component, class Access, class Mutation>
        bool mutateField(Target target, const char* identity, const char* label, Access access, Mutation mutate)
        {
            if (active())
                return reject(editing::EEditError::BUSY);
            const auto prepare = [&]() -> Status {
                const auto* cached = read<Component>(target);
                if (!cached || !access(*cached))
                    return cxx::unexpected(RunFailure{editing::makeEditFailure(editing::EEditError::STALE_TARGET)});
                auto value = *access(*cached);
                if (!mutate(value))
                    return cxx::unexpected(RunFailure{editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED)
                    });
                auto result = prepareField<Component>(target, identity, label, access, value);
                if (result && active())
                    commit_ = true;
                return result;
            };
            status_ = withRead(prepare);
            return status_.has_value();
        }

    private:
        template <class Component, class Value, class Access>
        Status prepareField(Target target, const char* identity, const char* label, Access access, const Value& value)
        {
            if (target != target_ || !history_ || read_only_ || !TFieldValue<Value>::valid(value))
                return cxx::unexpected(RunFailure{editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT)});
            auto* cached = const_cast<Component*>(read<Component>(target));
            if (!cached || !access(*cached))
                return cxx::unexpected(RunFailure{editing::makeEditFailure(editing::EEditError::STALE_TARGET)});
            if (!active() && TFieldValue<Value>::equal(*access(*cached), value))
                return {};
            // The first payload is the comparison baseline for this entire gesture.
            if (!active())
            {
                original_ = copy_(cached);
                if (!original_)
                    return constructionFailure();
                field_ = identity;
            }
            const auto& before = *access(*static_cast<const Component*>(original_.get()));
            pending_ = [target,
                        identity = std::string(identity),
                        label = std::string(label),
                        access,
                        before = Value(before),
                        value = Value(value)](SceneEditing& editing) -> Status {
                const auto* current =
                    static_cast<const Component*>(editing.component(target.entity, cxx::typeToken<Component>()));
                if (!current || !access(*current) || !TFieldValue<Value>::equal(*access(*current), before))
                    return cxx::unexpected(RunFailure{editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED)
                    });
                auto writable = editing.writeTarget(target.entity);
                if (!writable)
                    return cxx::unexpected(RunFailure{writable.error()});
                auto result = editing.setField<Component>(*writable, identity, label, access, value);
                if (!result)
                    return cxx::unexpected(RunFailure{result.error()});
                return {};
            };
            *access(*cached) = value;
            ++version_;
            return {};
        }
        bool reject(editing::EEditError);
        void clear() noexcept;
        RunStore& runs_;
        Target target_;
        simulation::ecs::ComponentSchema schema_;
        Copy copy_;
        project::ProjectCatalogModel* catalog_;
        std::optional<editing::HistorySnapshot> history_;
        std::shared_ptr<void> display_, original_;
        cxx::move_only_function<Status(SceneEditing&)> pending_;
        cxx::move_only_function<bool()> structural_;
        std::string field_, message_;
        std::uint64_t version_{};
        bool commit_{}, read_only_{};
        Status status_;
    };
}
