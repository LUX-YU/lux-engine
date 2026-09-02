#pragma once

#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/inspector/visibility.h>
#include <lux/engine/scene/Scene.hpp>

#include <lux/cxx/compile_time/TypeToken.hpp>

#include <cstddef>
#include <memory>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace lux::editor::inspector
{
    class LUX_EDITOR_INSPECTOR_PUBLIC InspectorUndoJournal final
    {
    public:
        InspectorUndoJournal() noexcept = default;
        InspectorUndoJournal(const InspectorUndoJournal&) = delete;
        InspectorUndoJournal& operator=(const InspectorUndoJournal&) = delete;
        ~InspectorUndoJournal();

        template<class Component, class Value>
        using ApplyValueFn = bool (*)(simulation::ecs::Registry&, simulation::ecs::Entity, const Value&);

        template<class Component, class Value>
        [[nodiscard]] bool begin(
            EditorSelectionValue target,
            std::string_view field,
            const Value& before,
            ApplyValueFn<Component, Value> apply
        )
        {
            if (poisoned_ || active_ != nullptr || !target.scene.valid() ||
                target.entity == simulation::ecs::NullEntity || field.empty() || apply == nullptr)
            {
                return false;
            }
            try
            {
                active_ = std::make_unique<Operation<Component, Value>>(target, field, before, apply);
                return true;
            }
            catch (const std::bad_alloc&)
            {
                return false;
            }
        }

        template<class Component, class Value>
        [[nodiscard]] bool commit(EditorSelectionValue target, std::string_view field, const Value& after)
        {
            if (poisoned_ || active_ == nullptr || !active_->matches(
                    target,
                    lux::cxx::typeToken<Component>(),
                    lux::cxx::typeToken<Value>(),
                    field
                ))
            {
                return false;
            }
            auto* operation = static_cast<Operation<Component, Value>*>(active_.get());
            try
            {
                operation->after = after;
                undo_.reserve(undo_.size() + 1U);
                undo_.push_back(std::move(active_));
                redo_.clear();
                return true;
            }
            catch (const std::bad_alloc&)
            {
                active_.reset();
                return false;
            }
        }

        [[nodiscard]] bool cancel(EditorContext& context);
        [[nodiscard]] bool undo(EditorContext& context);
        [[nodiscard]] bool redo(EditorContext& context);
        void clear() noexcept;

        [[nodiscard]] bool hasActiveGesture() const noexcept;
        [[nodiscard]] bool canUndo() const noexcept;
        [[nodiscard]] bool canRedo() const noexcept;
        [[nodiscard]] bool poisoned() const noexcept;
        [[nodiscard]] std::size_t undoDepth() const noexcept;

    private:
        struct OperationBase
        {
            OperationBase(
                EditorSelectionValue operation_target,
                lux::cxx::TypeToken operation_component,
                lux::cxx::TypeToken operation_value,
                std::string_view operation_field
            ) noexcept
                : target(operation_target),
                  component(operation_component),
                  value(operation_value),
                  field(operation_field)
            {
            }

            virtual ~OperationBase() = default;
            [[nodiscard]] virtual bool apply(EditorContext& context, bool use_after) = 0;

            [[nodiscard]] bool matches(
                EditorSelectionValue candidate,
                lux::cxx::TypeToken candidate_component,
                lux::cxx::TypeToken candidate_value,
                std::string_view candidate_field
            ) const noexcept
            {
                return target == candidate && component == candidate_component && value == candidate_value &&
                    field == candidate_field;
            }

            EditorSelectionValue target;
            lux::cxx::TypeToken component;
            lux::cxx::TypeToken value;
            std::string_view field;
        };

        template<class Component, class Value>
        struct Operation final : OperationBase
        {
            Operation(
                EditorSelectionValue target,
                std::string_view field,
                const Value& initial,
                ApplyValueFn<Component, Value> apply_value
            )
                : OperationBase(
                      target,
                      lux::cxx::typeToken<Component>(),
                      lux::cxx::typeToken<Value>(),
                      field
                  ),
                  before(initial),
                  after(initial),
                  apply_value(apply_value)
            {
            }

            [[nodiscard]] bool apply(EditorContext& context, bool use_after) override
            {
                auto* scene = context.selection().resolve(this->target.scene);
                if (scene == nullptr)
                    return false;
                auto& registry = scene->registry();
                if (!registry.valid(this->target.entity))
                    return false;
                return apply_value(registry, this->target.entity, use_after ? after : before);
            }

            Value before;
            Value after;
            ApplyValueFn<Component, Value> apply_value{};
        };

        [[nodiscard]] bool transfer(
            EditorContext& context,
            std::vector<std::unique_ptr<OperationBase>>& from,
            std::vector<std::unique_ptr<OperationBase>>& to,
            bool use_after
        );

        std::vector<std::unique_ptr<OperationBase>> undo_;
        std::vector<std::unique_ptr<OperationBase>> redo_;
        std::unique_ptr<OperationBase> active_;
        bool poisoned_{};
    };
} // namespace lux::editor::inspector
