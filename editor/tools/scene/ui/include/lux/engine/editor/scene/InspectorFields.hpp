#pragma once
#include <lux/engine/editor/scene/SceneInteraction.hpp>
#include <lux/engine/editor/project/ProjectCatalogAccess.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <functional>

namespace lux::editor::scene
{
    // One component's display/gesture cache. Author data remains inside SceneSession. Generated
    // controls use accessors on this cache; an edit reacquires the model's scratch field at commit.
    class InspectorFields final
    {
    public:
        using Editing = InspectorFields;
        using Target = SceneObjectRef;
        using Status = SceneEditResult<void>;
        InspectorFields(
            sessions::TSessionAccess<SceneSession>,
            SceneInteractionGroup&,
            Target,
            simulation::ecs::ComponentSchema,
            project::ProjectCatalogAccess = {}
        );
        ~InspectorFields() noexcept;
        InspectorFields(const InspectorFields&) = delete;
        InspectorFields& operator=(const InspectorFields&) = delete;
        InspectorFields(InspectorFields&&) = delete;
        InspectorFields& operator=(InspectorFields&&) = delete;

        [[nodiscard]] Status refresh();
        [[nodiscard]] Status update();
        [[nodiscard]] Status cancel();
        [[nodiscard]] Status release();
        [[nodiscard]] Status withRead(const std::function<Status()>&);
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
            return field_ == field;
        }
        [[nodiscard]] bool readOnly() const noexcept
        {
            return read_only_;
        }
        void setReadOnly(bool value) noexcept
        {
            read_only_ = value;
        }
        [[nodiscard]] project::ProjectCatalogAccess assetCatalog() const noexcept
        {
            return catalog_;
        }
        [[nodiscard]] project::ProjectCatalogAccess catalogAccess() const noexcept
        {
            return catalog_;
        }
        [[nodiscard]] const char* errorMessage() const noexcept
        {
            return message_.c_str();
        }
        void fail(const char* message)
        {
            message_ = message;
            static_cast<void>(reject(ESceneEditError::INVALID_FIELD));
        }
        [[nodiscard]] bool finish();
        bool queueEdit(std::function<bool()> edit);
        template <class Value> bool changed(Value& value, const Value& before, bool modified)
        {
            if (read_only_)
                value = before;
            return modified && !read_only_;
        }
        static Status connectionFailure(object::EConnectError);
        static Status constructionFailure();

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
            const bool is_wrong_field = active() && !ownsField(identity);
            if (!active() && interaction_.overlay())
                return reject(ESceneEditError::BUSY);
            const bool is_invalid = read_only_ || target != target_ || is_wrong_field ||
                                    schema_.cpp_type != cxx::typeToken<Component>() ||
                                    !TFieldValue<Value>::valid(value);
            if (is_invalid)
                return reject(ESceneEditError::INVALID_FIELD);
            if (change.cancelled)
            {
                status_ = cancel();
                return status_.has_value();
            }
            if (change.changed)
            {
                status_ = withRead([&]() -> Status {
                    auto* cached = const_cast<Component*>(read<Component>(target));
                    if (!cached || !access(*cached))
                        return cxx::unexpected(SceneEditError{ESceneEditError::INVALID_FIELD});
                    if (!active() && TFieldValue<Value>::equal(*access(*cached), value))
                        return {};
                    // Prepare first, then replace the local preview. Destruction stays under the gate.
                    std::vector<VSceneEdit> candidate;
                    candidate.emplace_back(SceneSetField::makeWithAccess<Component>(
                        {target_, schema_.id, identity},
                        value,
                        access,
                        schema_.code_lifetime
                    ));
                    pending_ = std::move(candidate);
                    *access(*cached) = value;
                    if (!active())
                    {
                        field_ = identity;
                        label_ = label;
                    }
                    ++version_;
                    return {};
                });
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
                return reject(ESceneEditError::BUSY);
            // Container values and custom copy callbacks remain under the same author read gate.
            status_ = withRead([&]() -> Status {
                const auto* cached = read<Component>(target);
                if (!cached || !access(*cached))
                    return cxx::unexpected(SceneEditError{ESceneEditError::INVALID_FIELD});
                auto next = *access(*cached);
                if (!mutate(next))
                    return cxx::unexpected(SceneEditError{ESceneEditError::INVALID_FIELD});
                pending_.emplace_back(SceneSetField::makeWithAccess<Component>(
                    {target_, schema_.id, identity},
                    std::move(next),
                    access,
                    schema_.code_lifetime
                ));
                field_ = identity;
                label_ = label;
                commit_ = true;
                return {};
            });
            return status_.has_value();
        }

    private:
        bool reject(ESceneEditError error)
        {
            status_ = cxx::unexpected(SceneEditError{error});
            return false;
        }
        sessions::TSessionAccess<SceneSession> sessions_;
        SceneInteractionGroup& interaction_;
        Target target_;
        simulation::ecs::ComponentSchema schema_;
        project::ProjectCatalogAccess catalog_;
        std::unique_ptr<simulation::ecs::Registry> display_;
        simulation::ecs::Entity entity_{simulation::ecs::NullEntity};
        std::optional<sessions::ContentStamp> content_;
        std::vector<VSceneEdit> pending_;
        std::function<bool()> structural_;
        std::string field_, label_, message_;
        std::uint64_t version_{};
        bool begun_{}, commit_{}, read_only_{};
        Status status_;
    };
}
