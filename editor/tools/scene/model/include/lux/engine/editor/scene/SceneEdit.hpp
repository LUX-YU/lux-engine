#pragma once

#include <lux/engine/editor/scene/FieldValue.hpp>
#include <lux/engine/editor/scene/SceneSnapshot.hpp>
#include <charconv>

namespace lux::editor::scene
{
    namespace detail
    {
        using SwapSceneComponent = void (*)(
            simulation::ecs::Registry&,
            simulation::ecs::Entity,
            simulation::ecs::Registry&,
            simulation::ecs::Entity
        ) noexcept;

        class SceneFieldChange
        {
        public:
            virtual ~SceneFieldChange() = default;
            [[nodiscard]] virtual lux::cxx::TypeToken type() const noexcept = 0;
            [[nodiscard]] virtual std::size_t bytes() const noexcept = 0;
            [[nodiscard]] virtual SwapSceneComponent swap() const noexcept = 0;
            [[nodiscard]] virtual bool apply(void* component, std::string_view path) const = 0;
        };

        template <class Value, class Object>
        bool assignSceneField(Object& object, std::string_view path, const Value& value)
        {
            if constexpr (lux::meta::HasTypeStaticInfo<Object>)
            {
                const auto dot = path.find('.');
                const auto name = path.substr(0, dot);
                bool assigned{};
                std::apply(
                    [&](const auto&... field) {
                        const auto visit = [&](const auto& entry) {
                            if (name != entry.name)
                                return;
                            auto& target = object.*entry.pointer;
                            using Target = std::remove_cvref_t<decltype(target)>;
                            if (dot != std::string_view::npos)
                                assigned = assignSceneField(target, path.substr(dot + 1), value);
                            else if constexpr (std::same_as<Target, Value>)
                            {
                                target = value;
                                assigned = true;
                            }
                        };
                        (visit(field), ...);
                    },
                    lux::meta::TTypeStaticInfo<Object>::fields
                );
                return assigned;
            }
            else if constexpr (requires { std::variant_size<Object>::value; })
            {
                return !object.valueless_by_exception() &&
                       std::visit(
                           [&](auto& value_object) { return assignSceneField(value_object, path, value); },
                           object
                       );
            }
            else if constexpr (requires {
                                   object.has_value();
                                   *object;
                               })
                return object.has_value() && assignSceneField(*object, path, value);
            else if constexpr (std::ranges::random_access_range<Object> && !std::same_as<Object, std::string> &&
                               !requires { Object::SizeAtCompileTime; })
            {
                const auto dot = path.find('.');
                const auto part = path.substr(0, dot);
                std::size_t index{};
                const auto parsed = std::from_chars(part.data(), part.data() + part.size(), index);
                const bool is_valid_index = parsed.ec == std::errc{} && parsed.ptr == part.data() + part.size() &&
                                            index < std::ranges::size(object);
                if (!is_valid_index)
                    return false;
                auto& item = object[index];
                if (dot != std::string_view::npos)
                    return assignSceneField(item, path.substr(dot + 1), value);
                if constexpr (std::same_as<std::remove_cvref_t<decltype(item)>, Value>)
                {
                    item = value;
                    return true;
                }
            }
            return false;
        }

        template <class Component, class Value> class TSceneFieldChange final : public SceneFieldChange
        {
        public:
            explicit TSceneFieldChange(Value value) : value_(std::move(value)) {}
            lux::cxx::TypeToken type() const noexcept override
            {
                return lux::cxx::typeToken<Component>();
            }
            std::size_t bytes() const noexcept override
            {
                return TFieldValue<Value>::bytes(value_);
            }
            SwapSceneComponent swap() const noexcept override
            {
                return [](auto& left, auto entity, auto& right, auto other) noexcept {
                    TFieldValue<Component>::swap(
                        left.template get<Component>(entity),
                        right.template get<Component>(other)
                    );
                };
            }
            bool apply(void* component, std::string_view path) const override
            {
                return TFieldValue<Value>::valid(value_) &&
                       assignSceneField(*static_cast<Component*>(component), path, value_) &&
                       TFieldValue<Component>::valid(*static_cast<Component*>(component));
            }

        private:
            Value value_;
        };
    }

    struct SceneCreateObject final
    {
        SceneObjectData value;
    };
    struct SceneEraseObject final
    {
        SceneObjectRef target;
    };
    struct SceneReparentObject final
    {
        SceneObjectRef target;
        world::WorldObjectId parent;
    };
    struct SceneAddComponent final
    {
        SceneObjectRef target;
        SceneComponentData value;
    };
    struct SceneRemoveComponent final
    {
        SceneObjectLocator target;
    };
    struct SceneSetConfiguration final
    {
        SceneConfiguration value;
    };

    struct SceneSetField final
    {
        SceneObjectLocator target;
        // This outer lease survives the erased edit's destructor, including plugin template instantiations.
        std::shared_ptr<const void> code;
        std::unique_ptr<const detail::SceneFieldChange> value;

        template <class Component, class Value>
        [[nodiscard]] static SceneSetField make(
            SceneObjectLocator target,
            Value value,
            std::shared_ptr<const void> code = {}
        )
        {
            return {
                std::move(target),
                std::move(code),
                std::make_unique<detail::TSceneFieldChange<Component, Value>>(std::move(value))
            };
        }
    };

    using VSceneEdit = std::variant<
        SceneCreateObject,
        SceneEraseObject,
        SceneReparentObject,
        SceneAddComponent,
        SceneRemoveComponent,
        SceneSetField,
        SceneSetConfiguration>;

    struct SceneEditBatch final
    {
        sessions::ContentStamp expected;
        std::string label;
        std::vector<VSceneEdit> edits;
    };

    struct SceneEditReceipt final
    {
        editing::EEditEffect effect{};
        sessions::ContentStamp content;
        SceneChangeCursor cursor;
    };
}
