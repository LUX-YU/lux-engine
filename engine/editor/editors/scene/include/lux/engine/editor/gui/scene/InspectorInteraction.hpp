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
    // Pane-owned, versioned input drafts. Only SceneEditor can change author content or history.
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

        bool finish(scene::SceneEditor &document, bool commit)
        {
            auto *gesture = std::get_if<Gesture>(&gesture_);
            if (!gesture)
            {
                return true;
            }
            borrow_valid_ = false;
            const auto finished = commit ? [&]() -> editing::EditResult<void>
            {
                auto result = document.commitPreview(gesture->token);
                if (!result)
                {
                    return lux::cxx::unexpected(result.error());
                }
                return {};
            }()
                : document.cancelPreview(gesture->token);
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
            if (requested != expansion.open && finish(document, true))
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
            if (!finish(document, true))
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

        template <class Component, class Value, class Access, class Draw>
        void field(scene::SceneEditor &document, lux::world::WorldObjectId object, lux::ui::Frame &frame,
                   const char *identity, const char *label, Access access, Draw draw, bool immutable)
        {
            const auto snapshot = readComponent(object, lux::cxx::typeToken<Component>());
            const auto *component = static_cast<const Component *>(snapshot.value);
            if (!component)
            {
                return;
            }
            auto *live = access(*component);
            if (!live)
            {
                return;
            }
            auto &draft = input<Draft<Value>>(ImGui::GetID(identity));
            const auto sequence = snapshot.sequence;
            const auto *gesture = std::get_if<Gesture>(&gesture_);
            const bool owns_gesture = gesture && gesture->field == identity;
            if (!owns_gesture && (draft.sequence != sequence || draft.object != object || draft.reload))
            {
                draft.value = *live;
                invalidateContainerIterators();
                draft.object = object;
                draft.sequence = sequence;
                draft.reload = false;
            }

            frame.propertyRow(label);
            ImGui::PushID(identity);
            const bool disabled = immutable || !document.writeRestriction().empty() || (gesture && !owns_gesture);
            const auto previous_read_only = std::exchange(read_only, disabled);
            const auto previous_error = error;
            error[0] = '\0';
            const auto change = draw(draft.value, *this);
            read_only = previous_read_only;
            ImGui::PopID();
            if (error[0])
            {
                draft.reload = true;
                return;
            }
            error = previous_error;
            if (disabled)
            {
                return;
            }
            if ((change.began || change.changed) && !active())
            {
                auto target = document.writeTarget(object);
                if (!target)
                {
                    fail(target.error());
                    draft.reload = true;
                    return;
                }
                borrow_valid_ = false;
                auto begun = document.beginPreview<Component, Value>(*target, origin_, identity, label, access);
                if (!begun)
                {
                    fail(begun.error());
                    draft.reload = true;
                    return;
                }
                gesture_.emplace<Gesture>(std::move(*begun), identity);
            }
            if (auto *current = std::get_if<Gesture>(&gesture_); current && current->field == identity)
            {
                if (change.changed)
                {
                    borrow_valid_ = false;
                    const auto updated = document.updatePreview(current->token, draft.value);
                    if (!updated)
                    {
                        fail(updated.error());
                        // Notification callbacks end the earlier component borrow, even on failure.
                        const auto restored = readComponent(object, lux::cxx::typeToken<Component>());
                        if (restored.value)
                        {
                            if (const auto *value = access(*static_cast<const Component *>(restored.value)))
                            {
                                draft.value = *value;
                                invalidateContainerIterators();
                            }
                        }
                        draft.reload = true;
                        return;
                    }
                    error[0] = '\0';
                }
                if (change.cancelled || change.committed)
                {
                    if (finish(document, !change.cancelled))
                    {
                        draft.sequence = document.componentVersion(object, lux::cxx::typeToken<Component>());
                    }
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

        template <class Value> struct Draft final
        {
            Value value{};
            lux::world::WorldObjectId object;
            std::uint64_t sequence{};
            bool reload{true};
        };

        struct Gesture final
        {
            scene::PreviewToken token;
            std::string field;
        };

        std::string origin_;
        std::unordered_map<ScratchKey, std::any, ScratchHash> scratch_;
        std::variant<std::monostate, Gesture> gesture_;
        editing::EditFailure failure_;
        ComponentRead borrow_;
        bool draw_active_{}, borrow_valid_{};
        std::uint64_t container_epoch_{1};
    };
} // namespace lux::editor::gui
