#pragma once

#include <any>
#include <array>
#include <imgui.h>
#include <lux/engine/editor/scene/FieldEdit.hpp>
#include <lux/engine/ui/Frame.hpp>
#include <unordered_map>
#include <variant>

namespace lux::editor::gui
{
    // Pane-local widget state. Generated controls edit the active Registry directly;
    // the document owns before/after history values.
    class InspectorInteraction final
    {
      public:
        InspectorInteraction(scene::SceneEditor &owner, std::string origin)
            : document(owner), origin_(std::move(origin))
        {
        }

        class ComponentDraw final
        {
          public:
            explicit ComponentDraw(InspectorInteraction &owner) : owner_(owner)
            {
                owner_.draw_active_ = true;
                owner_.borrow_valid_ = false;
            }
            ~ComponentDraw()
            {
                owner_.draw_active_ = false;
                owner_.borrow_valid_ = false;
            }
            ComponentDraw(const ComponentDraw &) = delete;
            ComponentDraw &operator=(const ComponentDraw &) = delete;

          private:
            InspectorInteraction &owner_;
        };

        [[nodiscard]] ComponentDraw componentDraw()
        {
            return ComponentDraw{*this};
        }

        template <class Value> Value &input(std::uint64_t id)
        {
            const ScratchKey key{id, lux::cxx::typeToken<Value>().hash()};
            auto [entry, inserted] = scratch_.try_emplace(key);
            if (inserted)
            {
                entry->second.emplace<Value>();
            }
            return *std::any_cast<Value>(&entry->second);
        }

        void fail(const char *message) noexcept
        {
            std::size_t index{};
            while (message[index] && index + 1 < error.size())
            {
                error[index] = message[index];
                ++index;
            }
            error[index] = '\0';
        }

        void fail(const editing::EditFailure &failure) noexcept
        {
            failure_ = failure;
            fail(failure.message[0] ? failure.message.data() : "The document rejected this edit");
        }

        [[nodiscard]] bool active() const noexcept
        {
            return gesture_.index() != 0;
        }

        [[nodiscard]] const editing::EditFailure &failure() const noexcept
        {
            return failure_;
        }

        // Finalize a completed interaction even if its widget no longer reports
        // deactivation. A rejected commit retains the token for retry.
        bool finishDraw()
        {
            if (active() && !ImGui::IsAnyItemActive())
            {
                return finish(document);
            }
            return true;
        }

        bool finish(scene::SceneEditor &document)
        {
            auto *gesture = std::get_if<Gesture>(&gesture_);
            if (!gesture)
            {
                return true;
            }
            borrow_valid_ = false;
            const auto finished = document.finishFieldEdit(gesture->token);
            if (!finished && finished.error().code != editing::EEditError::STALE_TARGET)
            {
                fail(finished.error());
                return false;
            }
            gesture_.emplace<std::monostate>();
            return true;
        }

        void reset()
        {
            scratch_.clear();
            error.fill('\0');
            invalidateContainerIterators();
        }

        void invalidateContainerIterators() noexcept
        {
            ++container_epoch_;
        }

        [[nodiscard]] std::uint64_t containerEpoch() const noexcept
        {
            return container_epoch_;
        }

        template <class Component> const Component *read(lux::world::WorldObjectId object)
        {
            return static_cast<const Component *>(readComponent(object, lux::cxx::typeToken<Component>()).value);
        }

        bool beginTree(const char *identity, const char *label)
        {
            auto &expansion = input<Expansion>(ImGui::GetID(identity));
            ImGui::SetNextItemOpen(expansion.open, ImGuiCond_Always);
            const bool requested = ImGui::TreeNodeEx(identity, ImGuiTreeNodeFlags_NoTreePushOnOpen, "%s", label);
            if (requested != expansion.open && finish(document))
            {
                expansion.open = requested;
            }
            if (expansion.open)
            {
                ImGui::TreePush(identity);
            }
            return expansion.open;
        }

        template <class Component, class Access, class Mutation>
        bool mutateField(lux::world::WorldObjectId object, const char *identity, const char *label, Access access,
                         Mutation mutate)
        {
            if (!finish(document))
            {
                return false;
            }
            const auto *component = read<Component>(object);
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
            const auto target = document.writeTarget(object);
            if (!target)
            {
                fail(target.error());
                return false;
            }
            borrow_valid_ = false;
            const auto result = document.setField<Component>(*target, identity, label, access, next);
            if (!result)
            {
                fail(result.error());
                return false;
            }
            invalidateContainerIterators();
            return true;
        }

        // Called before a widget applies a real change. The callback exists only
        // during this draw and captures the root field once for the gesture.
        class LocalInput final
        {
          public:
            explicit LocalInput(InspectorInteraction &owner) : owner_(owner), previous_(owner.local_input_)
            {
                owner_.local_input_ = true;
            }
            ~LocalInput()
            {
                owner_.local_input_ = previous_;
            }

          private:
            InspectorInteraction &owner_;
            bool previous_;
        };

        bool beforeWrite()
        {
            if (read_only)
            {
                return false;
            }
            if (local_input_)
            {
                return true;
            }
            if (!capture_)
            {
                return false;
            }
            return capture_(capture_context_);
        }

        // ImGui scalar widgets write through the real field pointer. Their small
        // pre-call value is also the undo value; capture it before publishing the
        // mutation. No persistent widget draft or per-frame field copy is kept.
        template <class Value> bool changed(Value &value, const Value &before, bool changed)
        {
            if (!changed)
            {
                return false;
            }
            Value next(value);
            value = before;
            if (!beforeWrite())
            {
                return false;
            }
            value = std::move(next);
            return true;
        }

        template <class Component, class Value, class Access, class Draw>
        void field(scene::SceneEditor &document, lux::world::WorldObjectId object, lux::ui::Frame &frame,
                   const char *identity, const char *label, Access access, Draw draw, bool immutable)
        {
            const auto snapshot = readComponent(object, lux::cxx::typeToken<Component>());
            auto *component = const_cast<Component *>(static_cast<const Component *>(snapshot.value));
            if (!component)
            {
                return;
            }
            auto *live = access(*component);
            if (!live)
            {
                return;
            }
            const auto *gesture = std::get_if<Gesture>(&gesture_);
            const bool owns_gesture = gesture && gesture->field == identity;
            const bool disabled = immutable || !document.writeRestriction().empty() || (gesture && !owns_gesture);
            auto capture = [&]() -> bool
            {
                if (active())
                {
                    return document.fieldEditWritable(std::get<Gesture>(gesture_).token);
                }
                auto target = document.writeTarget(object);
                if (!target)
                {
                    fail(target.error());
                    return false;
                }
                auto begun = document.beginFieldEdit<Component, Value>(*target, origin_, identity, label, access);
                if (!begun)
                {
                    fail(begun.error());
                    return false;
                }
                gesture_.emplace<Gesture>(std::move(*begun), identity);
                return true;
            };
            capture_context_ = &capture;
            capture_ = [](void *context) { return (*static_cast<decltype(capture) *>(context))(); };
            frame.propertyRow(label);
            ImGui::PushID(identity);
            const auto previous_read_only = std::exchange(read_only, disabled);
            const auto change = draw(*live, *this);
            read_only = previous_read_only;
            ImGui::PopID();
            capture_ = nullptr;
            capture_context_ = nullptr;
            if (disabled)
            {
                return;
            }
            if (auto *current = std::get_if<Gesture>(&gesture_); current && current->field == identity)
            {
                if (change.changed)
                {
                    borrow_valid_ = false;
                    const auto updated = document.fieldEdited(current->token);
                    if (!updated)
                    {
                        fail(updated.error());
                        return;
                    }
                }
                if (change.committed || change.cancelled)
                {
                    static_cast<void>(finish(document));
                }
            }
        }

        std::array<char, 192> error{};
        bool read_only{};
        scene::SceneEditor &document;

      private:
        struct Expansion final
        {
            bool open{true};
        };

        struct ComponentRead final
        {
            lux::world::WorldObjectId object;
            lux::cxx::TypeToken type;
            const void *value{};
            std::uint64_t sequence{};
        };

        ComponentRead readComponent(lux::world::WorldObjectId object, lux::cxx::TypeToken type)
        {
            if (draw_active_ && borrow_valid_ && borrow_.object == object && borrow_.type == type)
            {
                return borrow_;
            }
            ComponentRead result{object, type, document.component(object, type),
                                 document.componentVersion(object, type)};
            if (draw_active_)
            {
                borrow_ = result;
                borrow_valid_ = true;
            }
            return result;
        }

        struct ScratchKey final
        {
            std::uint64_t item;
            std::uint64_t type;
            friend bool operator==(ScratchKey, ScratchKey) = default;
        };

        struct ScratchHash final
        {
            std::size_t operator()(ScratchKey key) const noexcept
            {
                return static_cast<std::size_t>(key.item ^ (key.type + (key.item << 6) + (key.item >> 2)));
            }
        };

        struct Gesture final
        {
            scene::FieldEditToken token;
            std::string field;
        };

        void *capture_context_{};
        bool (*capture_)(void *){};
        std::string origin_;
        std::unordered_map<ScratchKey, std::any, ScratchHash> scratch_;
        std::variant<std::monostate, Gesture> gesture_;
        editing::EditFailure failure_;
        ComponentRead borrow_;
        bool draw_active_{}, borrow_valid_{}, local_input_{};
        std::uint64_t container_epoch_{1};
    };
} // namespace lux::editor::gui
