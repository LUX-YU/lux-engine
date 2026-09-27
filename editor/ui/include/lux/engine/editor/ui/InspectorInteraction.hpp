#pragma once

#include <array>
#include <lux/engine/editor/editing/scene/FieldEdit.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <optional>

namespace lux::editor::ui
{
    // Coordinates the current field transaction. Element owners hold their input buffers;
    // SceneEditing and History own writes and before/after history values.
    class InspectorInteraction final
    {
    public:
        InspectorInteraction(scene::SceneEditing& owner, std::string origin)
            : editing_(owner), origin_(std::move(origin))
        {}

        void bind(scene::SceneEditing& owner) noexcept
        {
            editing_ = owner;
        }

        void fail(const char* message) noexcept
        {
            std::size_t index{};
            while (message[index] && index + 1 < error_.size())
            {
                error_[index] = message[index];
                ++index;
            }
            error_[index] = '\0';
        }

        void fail(const editing::EditFailure& failure) noexcept
        {
            failure_ = failure;
            fail(failure.message[0] ? failure.message.data() : "The scene rejected this edit");
        }

        [[nodiscard]] bool active() const noexcept
        {
            return gesture_.has_value();
        }

        [[nodiscard]] const editing::EditFailure& failure() const noexcept
        {
            return failure_;
        }

        // Structural commands are adopted by the Inspector owner during maintenance.
        // Captures own keys/indices and reacquire Registry fields; no Element is borrowed.
        bool queueEdit(std::function<editing::EditResult<void>()> edit)
        {
            if (pending_edit_)
            {
                fail("A structural edit is still pending.");
                return false;
            }
            pending_edit_ = std::move(edit);
            return true;
        }
        bool finishPending()
        {
            if (!pending_edit_)
                return true;
            const auto result = pending_edit_();
            if (!result)
            {
                fail(result.error());
                if (result.error().code != editing::EEditError::BUSY)
                    pending_edit_ = {};
                return false;
            }
            pending_edit_ = {};
            return true;
        }

        bool finish()
        {
            auto* gesture = (gesture_ ? &*gesture_ : nullptr);
            if (!gesture)
            {
                return true;
            }
            const auto finished = editing_.get().finishFieldEdit(gesture->token);
            if (!finished && finished.error().code != editing::EEditError::STALE_TARGET)
            {
                fail(finished.error());
                return false;
            }
            gesture_.reset();
            return true;
        }

        void reset()
        {
            error_.fill('\0');
        }

        template <class Component> const Component* read(lux::simulation::ecs::Entity object)
        {
            return static_cast<const Component*>(editing_.get().component(object, lux::cxx::typeToken<Component>()));
        }

        template <class Component, class Access, class Mutation>
        bool mutateField(
            lux::simulation::ecs::Entity object,
            const char* identity,
            const char* label,
            Access access,
            Mutation mutate
        )
        {
            if (!finish())
            {
                return false;
            }
            const auto* component = read<Component>(object);
            if (!component)
            {
                fail("The component is no longer available.");
                return false;
            }
            auto next = *access(*component);
            if (!mutate(next))
            {
                fail("The container structure changed before this action.");
                return false;
            }
            const auto target = editing_.get().writeTarget(object);
            if (!target)
            {
                fail(target.error());
                return false;
            }
            const auto result = editing_.get().setField<Component>(*target, identity, label, access, next);
            if (!result)
            {
                fail(result.error());
                return false;
            }
            return true;
        }

        template <class Component, class Value, class Access>
        bool apply(
            lux::simulation::ecs::Entity object,
            const char* identity,
            const char* label,
            Access access,
            const Value& value,
            lux::ui::EditResult change
        )
        {
            auto* gesture = (gesture_ ? &*gesture_ : nullptr);
            if (gesture && gesture->field != identity)
            {
                fail("Finish the active field before editing another field.");
                return false;
            }
            if (change.changed)
            {
                const auto* component =
                    static_cast<const Component*>(editing_.get().component(object, lux::cxx::typeToken<Component>()));
                if (!component || !access(*component))
                {
                    fail("The edited component is no longer available.");
                    return false;
                }
                if (!gesture && !scene::TFieldValue<Value>::equal(*access(*component), value))
                {
                    const auto target = editing_.get().writeTarget(object);
                    if (!target)
                    {
                        fail(target.error());
                        return false;
                    }
                    auto begun =
                        editing_.get().beginFieldEdit<Component, Value>(*target, origin_, identity, label, access);
                    if (!begun)
                    {
                        fail(begun.error());
                        return false;
                    }
                    gesture = &gesture_.emplace(std::move(*begun), identity);
                }
                if (gesture)
                {
                    if (!editing_.get().fieldEditWritable(gesture->token))
                    {
                        fail("The field is not writable at this scene safe point.");
                        return false;
                    }
                    // Reacquire after admission: no Registry field address survives this call.
                    auto* live = const_cast<Component*>(static_cast<const Component*>(
                        editing_.get().component(object, lux::cxx::typeToken<Component>())
                    ));
                    if (!live || !access(*live))
                        return false;
                    *access(*live) = value;
                    const auto updated = editing_.get().fieldEdited(gesture->token);
                    if (!updated)
                    {
                        fail(updated.error());
                        return false;
                    }
                }
            }
            return !(change.committed || change.cancelled) || finish();
        }

        [[nodiscard]] bool ownsField(std::string_view identity) const noexcept
        {
            const auto* gesture = (gesture_ ? &*gesture_ : nullptr);
            return gesture && gesture->field == identity;
        }

        // Specialized drawing algorithms edit only their Element's local value.
        template <class Value> bool changed(Value& value, const Value& before, bool modified)
        {
            if (!modified)
                return false;
            if (!read_only_)
                return true;
            value = before;
            return false;
        }

        [[nodiscard]] scene::SceneEditing& editing() const noexcept
        {
            return editing_.get();
        }

        [[nodiscard]] const char* errorMessage() const noexcept
        {
            return error_.data();
        }

        [[nodiscard]] bool readOnly() const noexcept
        {
            return read_only_;
        }

        void setReadOnly(bool value) noexcept
        {
            read_only_ = value;
        }

    private:
        std::array<char, 192> error_{};
        bool read_only_{};
        std::reference_wrapper<scene::SceneEditing> editing_;
        struct Gesture final
        {
            scene::FieldEditToken token;
            std::string field;
        };

        std::string origin_;
        std::optional<Gesture> gesture_;
        std::function<editing::EditResult<void>()> pending_edit_;
        editing::EditFailure failure_;
    };
} // namespace lux::editor::ui
