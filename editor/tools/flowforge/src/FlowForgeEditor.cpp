#include <lux/engine/editor/metadata/AssetEditorRegistration.hpp>
#include <lux/engine/editor/flowforge/FlowForgeEditorImpl.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/editor/ui/HistoryCommands.hpp>
#include <algorithm>
#include <cmath>
#include <lux/engine/editor/detail/AssetSave.hpp>
#include <lux/engine/editor/metadata/EditorReflection.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/editor/flowforge/FlowCompilation.hpp>
#include <lux/engine/editor/flowforge/FlowForgeEditor.hpp>
#include <lux/engine/flowforge/graph/ArithmeticNode.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>
#include <lux/engine/flowforge/script/ScriptEventAwaitNode.hpp>
#include <unordered_map>
#include <unordered_set>

namespace lux::editor::flowforge
{
    namespace
    {
        constexpr editing::HistoryLimits kLimits{1024, 64U * 1024U * 1024U, 16U * 1024U * 1024U, 256};
        bool registeredMetadata(
            const lux::flowforge::Node& node,
            const lux::flowforge::FlowSourceEnvironment& environment_
        )
        {
            using namespace lux::flowforge;
            if (node.operation() == ENodeOperation::NATIVE_FUNC_CALL)
            {
                const auto& call = static_cast<const NativeFuncCall&>(node);
                if (!call.ownerType())
                {
                    return std::ranges::any_of(environment_.functions, [&](const auto* function) {
                        return &function->invokable == &call.info();
                    });
                }
                return std::ranges::any_of(environment_.classes, [&](const auto* type) {
                    return &type->type == call.ownerType() &&
                           std::ranges::any_of(type->methods, [&](const auto& method) {
                               return &method.invokable == &call.info();
                           });
                });
            }
            if (node.operation() == ENodeOperation::GET_FIELD || node.operation() == ENodeOperation::SET_FIELD)
            {
                const bool reading = node.operation() == ENodeOperation::GET_FIELD;
                const auto* type = reading ? static_cast<const GetFieldNode&>(node).ownerClass()
                                           : static_cast<const SetFieldNode&>(node).ownerClass();
                const auto* field = reading ? static_cast<const GetFieldNode&>(node).field()
                                            : static_cast<const SetFieldNode&>(node).field();
                return std::ranges::find(environment_.classes, type) != environment_.classes.end() &&
                       std::ranges::any_of(
                           type->fields,
                           [&](const auto& registered) { return &registered == field; }
                       ) &&
                       field->visibility == lux::meta::EVisibility::PUBLIC && !field->is_volatile &&
                       (reading || !field->is_const);
            }
            if (node.operation() == ENodeOperation::SCRIPT_ABILITY_CALL)
            {
                const auto& ability = static_cast<const ScriptAbilityNode&>(node);
                const auto* registered = environment_.abilities.find(ability.contract(), ability.method());
                return registered && registered->schema_version == ability.expectedSchemaVersion() &&
                       registered->schema_hash == ability.expectedSchemaHash();
            }
            if (node.operation() == ENodeOperation::SCRIPT_EVENT_WAIT)
            {
                const auto& event = static_cast<const ScriptEventAwaitNode&>(node);
                return std::ranges::find(environment_.events, event.source()) != environment_.events.end();
            }
            return true;
        }

        std::size_t nodeStorageBytes(const lux::flowforge::Node& node)
        {
            using namespace lux::flowforge;
            constexpr auto object_bytes =
                std::max({sizeof(StartNode),           sizeof(BranchNode),      sizeof(SequenceNode),
                          sizeof(ForLoopNode),         sizeof(WhileLoopNode),   sizeof(ReturnNode),
                          sizeof(BreakNode),           sizeof(BinaryOpNode),    sizeof(UnaryOpNode),
                          sizeof(FuncDefNode),         sizeof(FuncReturnNode),  sizeof(OnEventNode),
                          sizeof(GraphFuncCallNode),   sizeof(NativeFuncCall),  sizeof(GetObjectNode),
                          sizeof(SetObjectNode),       sizeof(GetFieldNode),    sizeof(SetFieldNode),
                          sizeof(GetVariableNode),     sizeof(SetVariableNode), sizeof(ScriptAbilityNode),
                          sizeof(ScriptEventAwaitNode)});
            std::size_t bytes = object_bytes + sizeof(std::unique_ptr<Node>) + node.name().capacity() +
                                node.creatorName().capacity() + 2;
            bytes += (node.inPins().capacity() + node.outPins().capacity()) * sizeof(Pin*);
            const auto arguments = [&](const std::vector<FuncArgInfo>& values) {
                bytes += values.capacity() * sizeof(FuncArgInfo);
                for (const auto& value : values)
                {
                    bytes += value.name.capacity() + 1;
                }
            };
            for (const bool input : {true, false})
            {
                for (const auto* pin : input ? node.inPins() : node.outPins())
                {
                    bytes += std::max({sizeof(DataInPin), sizeof(DataOutPin), sizeof(ExecInPin), sizeof(ExecOutPin)}) +
                             pin->name().capacity() + 1;
                    if (pin->kind() == EPinKind::DATA_IN)
                    {
                        const auto& data = *static_cast<const DataInPin*>(pin);
                        bytes += data.info().name.capacity() + 1;
                        if (data.constantData().isValid())
                        {
                            bytes += data.constantData().type()->size;
                        }
                    }
                    else if (pin->kind() == EPinKind::DATA_OUT)
                    {
                        bytes += static_cast<const DataOutPin*>(pin)->info().name.capacity() + 1;
                    }
                }
            }
            if (const auto* exec = dynamic_cast<const ExecIntermediateNode*>(&node))
            {
                bytes += exec->extraOutPinStorageBytes();
            }
            switch (node.operation())
            {
            case ENodeOperation::FUNC_DEF_START: {
                const auto& value = static_cast<const FuncDefNode&>(node);
                arguments(value.argInfos());
                arguments(value.retInfos());
                bytes += value.argPins().capacity() * sizeof(void*) + value.extraOutPinStorageBytes();
                break;
            }
            case ENodeOperation::FUNC_RETURN:
                bytes += static_cast<const FuncReturnNode&>(node).retPins().capacity() * sizeof(void*);
                break;
            case ENodeOperation::ON_EVENT: {
                const auto& value = static_cast<const OnEventNode&>(node);
                arguments(value.paramInfos());
                bytes += value.paramPins().capacity() * sizeof(void*) + value.extraOutPinStorageBytes();
                break;
            }
            case ENodeOperation::GRAPH_FUNC_CALL: {
                const auto& value = static_cast<const GraphFuncCallNode&>(node);
                bytes += (value.argPins().capacity() + value.resultPins().capacity()) * sizeof(void*);
                break;
            }
            case ENodeOperation::NATIVE_FUNC_CALL:
                bytes += static_cast<const NativeFuncCall&>(node).dataInPins().capacity() * sizeof(void*);
                break;
            case ENodeOperation::SEQUENCE:
                bytes += static_cast<const SequenceNode&>(node).execOutPins().capacity() * sizeof(void*);
                break;
            case ENodeOperation::SCRIPT_ABILITY_CALL:
                bytes += static_cast<const ScriptAbilityNode&>(node).descriptionBytes();
                break;
            case ENodeOperation::SCRIPT_EVENT_WAIT:
                bytes += static_cast<const ScriptEventAwaitNode&>(node).descriptionBytes();
                break;
            default:
                break;
            }
            return bytes;
        }
        EditorFailure historyFailure(const editing::EditFailure& failure)
        {
            return {
                EEditorError::INVALID_STATE,
                "flowforge.history",
                static_cast<std::uint64_t>(failure.code),
                {},
                failure
            };
        }

        struct NameAccess final
        {
            using Value = std::string;
            bool exists(const Content&) const noexcept
            {
                return true;
            }
            Value read(const Content& source_) const
            {
                return source_.name;
            }
            void exchange(Content& source_, Value& value) const noexcept
            {
                source_.name.swap(value);
            }
            static bool equal(const Value& a, const Value& b) noexcept
            {
                return a == b;
            }
            static std::size_t bytes(const Value& value) noexcept
            {
                return value.capacity() + 1U;
            }
        };
        struct BusyGuard final
        {
            bool& busy_;
            explicit BusyGuard(bool& value) : busy_(value)
            {
                busy_ = true;
            }
            ~BusyGuard()
            {
                busy_ = false;
            }
        };
    } // namespace

    class FlowForgeEditor::Impl::VariableEdit final : public editing::EditOperation
    {
    public:
        struct Membership final
        {
            lux::flowforge::FlowSourceVariable value;
            std::size_t position;
            bool insert;
        };
        struct Replacement final
        {
            lux::flowforge::FlowSourceVariable before, after;
            std::size_t position;
        };
        using VMutation = std::variant<Membership, Replacement>;

    private:
        enum class EWrite
        {
            INSERT,
            ERASE,
            REPLACE
        };
        class Plan final : public editing::PreparedEdit
        {
        public:
            Plan(
                const VariableEdit& edit,
                EWrite write,
                std::size_t position,
                std::vector<lux::flowforge::FlowGraph::GraphVariable> staged,
                bool changed = true
            )
                : edit_(edit), write_(write), position_(position), staged_(std::move(staged)), changed_(changed)
            {}
            editing::EEditEffect effect() const noexcept override
            {
                return changed_ ? editing::EEditEffect::CHANGE : editing::EEditEffect::NO_CHANGE;
            }

        private:
            void apply() noexcept override
            {
                auto& variables = edit_.owner_.source_.graph.variables();
                if (write_ == EWrite::REPLACE)
                {
                    std::swap(variables[position_], staged_.front());
                    return;
                }
                for (std::size_t index{}; index < variables.size(); ++index)
                {
                    if (write_ == EWrite::INSERT || index != position_)
                    {
                        staged_.push_back(std::move(variables[index]));
                    }
                }
                if (write_ != EWrite::ERASE)
                {
                    std::rotate(staged_.begin(), staged_.begin() + 1, staged_.begin() + position_ + 1);
                }
                edit_.owner_.source_.graph.exchangeVariables(staged_);
            }
            void publish(const editing::CommitInfo& info) noexcept override
            {
                lux::editor::detail::reportSignalDelivery(
                    edit_.owner_.editor_->emit(edit_.owner_.editor_->contentChanged, info.revision),
                    "FlowForgeEditor::contentChanged"
                );
            }
            const VariableEdit& edit_;
            EWrite write_;
            std::size_t position_;
            std::vector<lux::flowforge::FlowGraph::GraphVariable> staged_;
            bool changed_;
        };
        static std::size_t bytes(const lux::flowforge::FlowSourceVariable& value) noexcept
        {
            return value.name.capacity() + value.type.capacity() + value.value.value.capacity() + 3;
        }
        bool matches(const lux::flowforge::FlowSourceVariable& value, std::size_t position) const
        {
            const auto& variables = owner_.source_.graph.variables();
            if (position >= variables.size() || variables[position].id != value.id)
            {
                return false;
            }
            const auto current = lux::flowforge::captureFlowVariable(variables[position]);
            return current && *current == value;
        }
        editing::EditResult<editing::PreparedEditPtr> prepareValue(
            EWrite write,
            std::size_t position,
            const lux::flowforge::FlowSourceVariable& value,
            editing::EditPreparationBudget& budget
        ) const
        {
            using Variable = lux::flowforge::FlowGraph::GraphVariable;
            const auto count = owner_.source_.graph.variables().size();
            const auto capacity = write == EWrite::INSERT ? count + 1 : write == EWrite::ERASE ? count - 1 : 1;
            const auto charge = sizeof(Plan) + capacity * sizeof(Variable) + bytes(value);
            auto reserved = budget.reserve(charge);
            if (!reserved)
            {
                return lux::cxx::unexpected(reserved.error());
            }
            std::vector<Variable> staged;
            staged.reserve(capacity);
            if (write != EWrite::ERASE)
            {
                auto variable = lux::flowforge::materializeFlowVariable(value, owner_.environment_);
                if (!variable)
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(
                        editing::EEditError::PRECONDITION_FAILED,
                        static_cast<std::uint64_t>(variable.error().code)
                    ));
                }
                auto payload = budget.reserve(variable->type->size);
                if (!payload)
                {
                    return lux::cxx::unexpected(payload.error());
                }
                staged.push_back(std::move(*variable));
            }
            return editing::PreparedEditPtr(new Plan(*this, write, position, std::move(staged)));
        }

    public:
        VariableEdit(Impl& owner, VMutation mutation)
            : owner_(owner), mutation_(std::move(mutation)), base_(owner.history_->view()->snapshot.current)
        {}
        editing::HistoryId historyId() const noexcept override
        {
            return base_.history;
        }
        editing::StateId baseState() const noexcept override
        {
            return base_;
        }
        std::string_view label() const noexcept override
        {
            return "Edit FlowForge variable";
        }
        std::size_t retainedBytesUpperBound() const noexcept override
        {
            return sizeof(*this) + std::visit(
                                       [](const auto& value) {
                                           if constexpr (std::is_same_v<std::decay_t<decltype(value)>, Membership>)
                                           {
                                               return bytes(value.value);
                                           }
                                           else
                                           {
                                               return bytes(value.before) + bytes(value.after);
                                           }
                                       },
                                       mutation_
                                   );
        }
        editing::EditResult<editing::PreparedEditPtr> prepare(
            const editing::ApplyContext& context,
            editing::EditPreparationBudget& budget
        ) const noexcept override
        {
            return std::visit(
                [&](const auto& mutation) -> editing::EditResult<editing::PreparedEditPtr> {
                    const auto rejected = [] {
                        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
                    };
                    if constexpr (std::is_same_v<std::decay_t<decltype(mutation)>, Membership>)
                    {
                        const bool insert = mutation.insert == (context.direction == editing::EDirection::FORWARD);
                        if (insert)
                        {
                            if (owner_.source_.graph.findVariable(mutation.value.id) ||
                                mutation.position > owner_.source_.graph.variables().size())
                            {
                                return rejected();
                            }
                        }
                        else if (!matches(mutation.value, mutation.position) ||
                                 variableReferenced(owner_.source_.graph, mutation.value.id))
                        {
                            return rejected();
                        }
                        return prepareValue(
                            insert ? EWrite::INSERT : EWrite::ERASE,
                            mutation.position,
                            mutation.value,
                            budget
                        );
                    }
                    else
                    {
                        const auto& before =
                            context.direction == editing::EDirection::FORWARD ? mutation.before : mutation.after;
                        const auto& after =
                            context.direction == editing::EDirection::FORWARD ? mutation.after : mutation.before;
                        if (!matches(before, mutation.position) ||
                            (before.type != after.type && variableReferenced(owner_.source_.graph, before.id)))
                        {
                            return rejected();
                        }
                        if (before == after)
                        {
                            auto reserved = budget.reserve(sizeof(Plan));
                            if (!reserved)
                            {
                                return lux::cxx::unexpected(reserved.error());
                            }
                            return editing::PreparedEditPtr(
                                new Plan(*this, EWrite::REPLACE, mutation.position, {}, false)
                            );
                        }
                        return prepareValue(EWrite::REPLACE, mutation.position, after, budget);
                    }
                },
                mutation_
            );
        }

    private:
        Impl& owner_;
        VMutation mutation_;
        editing::StateId base_;
    };

    class FlowForgeEditor::Impl::LiteralEdit final : public editing::EditOperation
    {
        class Plan final : public editing::PreparedEdit
        {
        public:
            Plan(const LiteralEdit& edit, lux::flowforge::DataInPin& pin, lux::meta::RuntimeObject value)
                : edit_(edit), pin_(pin), value_(std::move(value))
            {}
            editing::EEditEffect effect() const noexcept override
            {
                return edit_.before_ == edit_.after_ ? editing::EEditEffect::NO_CHANGE : editing::EEditEffect::CHANGE;
            }

        private:
            void apply() noexcept override
            {
                swap(pin_.constantData(), value_);
            }
            void publish(const editing::CommitInfo& info) noexcept override
            {
                lux::editor::detail::reportSignalDelivery(
                    edit_.owner_.editor_->emit(edit_.owner_.editor_->contentChanged, info.revision),
                    "FlowForgeEditor::contentChanged"
                );
            }
            const LiteralEdit& edit_;
            lux::flowforge::DataInPin& pin_;
            lux::meta::RuntimeObject value_;
        };

    public:
        LiteralEdit(
            Impl& owner,
            lux::flowforge::PinId pin,
            lux::flowforge::FlowSourceLiteral before,
            lux::flowforge::FlowSourceLiteral after
        )
            : owner_(owner), pin_(pin), before_(std::move(before)), after_(std::move(after)),
              base_(owner.history_->view()->snapshot.current)
        {}
        editing::HistoryId historyId() const noexcept override
        {
            return base_.history;
        }
        editing::StateId baseState() const noexcept override
        {
            return base_;
        }
        std::string_view label() const noexcept override
        {
            return "Set FlowForge literal";
        }
        std::size_t retainedBytesUpperBound() const noexcept override
        {
            return sizeof(*this) + before_.value.capacity() + after_.value.capacity() + 2;
        }
        editing::EditResult<editing::PreparedEditPtr> prepare(
            const editing::ApplyContext& context,
            editing::EditPreparationBudget& budget
        ) const noexcept override
        {
            auto* base = owner_.source_.graph.findPin(pin_);
            if (!base || base->kind() != lux::flowforge::EPinKind::DATA_IN)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
            }
            auto& pin = *static_cast<lux::flowforge::DataInPin*>(base);
            const auto& expected = context.direction == editing::EDirection::FORWARD ? before_ : after_;
            const auto& next = context.direction == editing::EDirection::FORWARD ? after_ : before_;
            const auto current = lux::flowforge::captureFlowLiteral(pin.constantData());
            if (!current || *current != expected)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
            }
            auto reserved = budget.reserve(sizeof(Plan) + pin.info().type->size);
            if (!reserved)
            {
                return lux::cxx::unexpected(reserved.error());
            }
            auto value = lux::flowforge::materializeFlowLiteral(next, *pin.info().type);
            if (!value)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(
                    editing::EEditError::PRECONDITION_FAILED,
                    static_cast<std::uint64_t>(value.error().code)
                ));
            }
            return editing::PreparedEditPtr(new Plan(*this, pin, std::move(*value)));
        }

    private:
        Impl& owner_;
        lux::flowforge::PinId pin_;
        lux::flowforge::FlowSourceLiteral before_, after_;
        editing::StateId base_;
    };

    template <class Access> class FlowForgeEditor::Impl::TValueEdit final : public editing::EditOperation
    {
        using Value = typename Access::Value;
        class Plan final : public editing::PreparedEdit
        {
        public:
            Plan(const TValueEdit& edit, Value value, bool changed)
                : edit_(edit), value_(std::move(value)), changed_(changed)
            {}
            editing::EEditEffect effect() const noexcept override
            {
                return changed_ ? editing::EEditEffect::CHANGE : editing::EEditEffect::NO_CHANGE;
            }

        private:
            void apply() noexcept override
            {
                edit_.access_.exchange(edit_.owner_.source_, value_);
            }
            void publish(const editing::CommitInfo& info) noexcept override
            {
                lux::editor::detail::reportSignalDelivery(
                    edit_.owner_.editor_->emit(edit_.owner_.editor_->contentChanged, info.revision),
                    "FlowForgeEditor::contentChanged"
                );
            }
            const TValueEdit& edit_;
            Value value_;
            bool changed_;
        };

    public:
        TValueEdit(Impl& owner, Access access, Value after, std::string label, editing::StateId base)
            : owner_(owner), access_(access), before_(access.read(owner.source_)), after_(std::move(after)),
              label_(std::move(label)), base_(base)
        {}
        editing::HistoryId historyId() const noexcept override
        {
            return base_.history;
        }
        editing::StateId baseState() const noexcept override
        {
            return base_;
        }
        std::string_view label() const noexcept override
        {
            return label_;
        }
        std::size_t retainedBytesUpperBound() const noexcept override
        {
            return sizeof(*this) + label_.capacity() + 1U + Access::bytes(before_) + Access::bytes(after_);
        }
        editing::EditResult<editing::PreparedEditPtr> prepare(
            const editing::ApplyContext& context,
            editing::EditPreparationBudget& budget
        ) const noexcept override
        {
            const auto& expected = context.direction == editing::EDirection::FORWARD ? before_ : after_;
            const auto& next = context.direction == editing::EDirection::FORWARD ? after_ : before_;
            if (!access_.exists(owner_.source_) || !Access::equal(access_.read(owner_.source_), expected))
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
            }
            auto reserved = budget.reserve(sizeof(Plan) + Access::bytes(next));
            if (!reserved)
            {
                return lux::cxx::unexpected(reserved.error());
            }
            return editing::PreparedEditPtr(new Plan(*this, next, !Access::equal(before_, after_)));
        }

    private:
        Impl& owner_;
        Access access_;
        Value before_, after_;
        std::string label_;
        editing::StateId base_;
    };

    struct FlowForgeEditor::Impl::GraphDelta final
    {
        std::vector<lux::flowforge::NodeId> erase, unplace;
        std::vector<lux::graph::LinkRecord> connect, disconnect;
        std::vector<lux::graph::GraphLayoutEntry> place;
        bool restore{};
        bool empty() const noexcept
        {
            return erase.empty() && unplace.empty() && connect.empty() && disconnect.empty() && place.empty() &&
                   !restore;
        }
        std::size_t bytes() const noexcept
        {
            return (erase.capacity() + unplace.capacity()) * sizeof(lux::flowforge::NodeId) +
                   (connect.capacity() + disconnect.capacity()) * sizeof(lux::graph::LinkRecord) +
                   place.capacity() * sizeof(lux::graph::GraphLayoutEntry);
        }
    };

    struct FlowForgeEditor::Impl::ExportsAccess final
    {
        using Value = std::vector<lux::flowforge::ExportMethodNode>;
        bool exists(const Content&) const noexcept
        {
            return true;
        }
        Value read(const Content& source_) const
        {
            return source_.graph.exports();
        }
        void exchange(Content& source_, Value& value) const noexcept
        {
            source_.graph.exchangeExports(value);
        }
        static bool equal(const Value& a, const Value& b) noexcept
        {
            return a == b;
        }
        static std::size_t bytes(const Value& value) noexcept
        {
            std::size_t bytes = value.capacity() * sizeof(Value::value_type);
            for (const auto& item : value)
            {
                bytes += item.binding_hints.capacity() * sizeof(lux::script::ScriptBindingHintTarget);
                for (const auto& hint : item.binding_hints)
                {
                    bytes += hint.qualified_name.capacity() + 1;
                }
            }
            return bytes;
        }
    };

    class FlowForgeEditor::Impl::GraphEdit final : public editing::EditOperation
    {
        class Plan final : public editing::PreparedEdit
        {
        public:
            Plan(
                const GraphEdit& edit,
                lux::flowforge::FlowGraphEdit patch,
                const GraphDelta& delta,
                std::span<std::unique_ptr<lux::flowforge::Node>* const> insert
            )
                : edit_(edit), patch_(std::move(patch)), changed_(!delta.empty()),
                  structural_(!delta.erase.empty() || !insert.empty())
            {
                if (!structural_)
                {
                    return;
                }
                nodes_ = edit.owner_.read_nodes_;
                pins_ = edit.owner_.read_pins_;
                for (const auto id : delta.erase)
                {
                    const auto* node = nodes_.at(id);
                    for (const auto* pin : node->inPins())
                    {
                        pins_.erase(pin->id());
                    }
                    for (const auto* pin : node->outPins())
                    {
                        pins_.erase(pin->id());
                    }
                    nodes_.erase(id);
                }
                for (std::size_t i = 0; i < insert.size(); ++i)
                {
                    nodes_.emplace(patch_.insertedIds()[i], insert[i]->get());
                }
                for (const auto& [pin, id] : patch_.assignedPins())
                {
                    pins_.emplace(id, pin);
                }
            }
            editing::EEditEffect effect() const noexcept override
            {
                return changed_ ? editing::EEditEffect::CHANGE : editing::EEditEffect::NO_CHANGE;
            }

        private:
            void apply() noexcept override
            {
                if (edit_.input_)
                {
                    const auto id = patch_.insertedIds().front();
                    edit_.before_.erase.front() = id;
                    edit_.after_.place.front().node = id;
                }
                patch_.commit();
                if (structural_)
                {
                    edit_.owner_.read_nodes_.swap(nodes_);
                    edit_.owner_.read_pins_.swap(pins_);
                }
                edit_.parked_ = patch_.takeRemoved();
                edit_.input_ = nullptr;
            }
            void publish(const editing::CommitInfo& info) noexcept override
            {
                lux::editor::detail::reportSignalDelivery(
                    edit_.owner_.editor_->emit(edit_.owner_.editor_->contentChanged, info.revision),
                    "FlowForgeEditor::contentChanged"
                );
            }
            const GraphEdit& edit_;
            lux::flowforge::FlowGraphEdit patch_;
            bool changed_;
            bool structural_;
            NodeIndex nodes_;
            PinIndex pins_;
        };

    public:
        GraphEdit(
            Impl& owner,
            GraphDelta before,
            GraphDelta after,
            std::string label,
            std::size_t node_bytes,
            std::unique_ptr<lux::flowforge::Node>* input = nullptr,
            std::vector<std::unique_ptr<lux::flowforge::Node>> staged = {}
        )
            : owner_(owner), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)),
              base_(owner.history_->view()->snapshot.current), node_bytes_(node_bytes), input_(input),
              parked_(std::move(staged))
        {}
        editing::HistoryId historyId() const noexcept override
        {
            return base_.history;
        }
        editing::StateId baseState() const noexcept override
        {
            return base_;
        }
        std::string_view label() const noexcept override
        {
            return label_;
        }
        std::size_t retainedBytesUpperBound() const noexcept override
        {
            return sizeof(*this) + label_.capacity() + 1 + before_.bytes() + after_.bytes() + node_bytes_;
        }
        editing::EditResult<editing::PreparedEditPtr> prepare(
            const editing::ApplyContext& context,
            editing::EditPreparationBudget& budget
        ) const noexcept override
        {
            const auto& delta = context.direction == editing::EDirection::FORWARD ? after_ : before_;
            const auto& topology = owner_.source_.graph.topology();
            std::size_t slots{};
            for (const auto& storage : owner_.source_.graph.nodes())
            {
                slots = (std::max)(slots, storage.index + 1);
            }
            const auto structural = topology.nodes().size() * sizeof(lux::graph::NodeRecord) +
                                    topology.pins().size() * sizeof(lux::graph::PinRecord) +
                                    topology.links().size() * sizeof(lux::graph::LinkRecord) +
                                    owner_.source_.graph.layout().all().size() * sizeof(lux::graph::GraphLayoutEntry) +
                                    slots * 64;
            auto reserved =
                budget.reserve(sizeof(Plan) + 4 * (structural + before_.bytes() + after_.bytes() + node_bytes_));
            if (!reserved)
            {
                return lux::cxx::unexpected(reserved.error());
            }
            std::vector<std::unique_ptr<lux::flowforge::Node>*> insert;
            if (delta.restore)
            {
                if (input_)
                {
                    insert.push_back(input_);
                }
                else
                {
                    for (auto& node : parked_)
                    {
                        insert.push_back(&node);
                    }
                }
            }
            auto patch = lux::flowforge::FlowGraphEdit::prepare(
                owner_.source_.graph,
                {insert,
                 delta.erase,
                 delta.connect,
                 delta.disconnect,
                 input_ ? std::span<const lux::graph::GraphLayoutEntry>{} : std::span{delta.place},
                 delta.unplace,
                 !input_}
            );
            if (!patch)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(
                    editing::EEditError::PRECONDITION_FAILED,
                    static_cast<std::uint64_t>(patch.error().code)
                ));
            }
            if (input_)
            {
                auto placed = patch->place(patch->insertedIds().front(), after_.place.front().layout);
                if (!placed)
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(
                        editing::EEditError::PRECONDITION_FAILED,
                        static_cast<std::uint64_t>(placed.error().code)
                    ));
                }
            }
            return editing::PreparedEditPtr(new Plan(*this, std::move(*patch), delta, insert));
        }
        lux::flowforge::NodeId inserted() const noexcept
        {
            return before_.erase.front();
        }

    private:
        Impl& owner_;
        mutable GraphDelta before_, after_;
        std::string label_;
        editing::StateId base_;
        std::size_t node_bytes_;
        mutable std::unique_ptr<lux::flowforge::Node>* input_;
        mutable std::vector<std::unique_ptr<lux::flowforge::Node>> parked_;
    };

    template <class Access>
    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::change(
        Access access,
        typename Access::Value value,
        std::string label
    )
    {
        if (const auto admitted = canEdit(); !admitted)
            return lux::cxx::unexpected(admitted.error());
        if (!access.exists(source_))
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        auto view = history_->view();
        if (!view)
        {
            return lux::cxx::unexpected(view.error());
        }
        BusyGuard guard(busy_);
        editing::EditOperationPtr operation = std::make_unique<TValueEdit<Access>>(
            *this,
            access,
            std::move(value),
            std::move(label),
            view->snapshot.current
        );
        return history_->execute(operation);
    }

    void FlowForgeEditor::Impl::indexContent()
    {
        read_nodes_.clear();
        read_pins_.clear();
        read_nodes_.reserve(source_.graph.nodes().size());
        read_pins_.reserve(source_.graph.topology().pins().size());
        for (const auto& storage : source_.graph.nodes())
        {
            const auto* node = storage.node.get();
            read_nodes_.emplace(node->id(), node);
            for (const auto* pin : node->inPins())
            {
                read_pins_.emplace(pin->id(), pin);
            }
            for (const auto* pin : node->outPins())
            {
                read_pins_.emplace(pin->id(), pin);
            }
        }
    }

    editing::EditResult<void> FlowForgeEditor::Impl::canEdit() const noexcept
    {
        if (!history_)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::NO_ACTIVE_TARGET));
        const bool is_changing = asset_status_.phase != EAssetEditPhase::IDLE || bool(saved_history_);
        const bool is_busy = busy_ || (is_changing && !finishing_interaction_);
        const bool is_read_only = !editor_context_.project().writable();
        if (is_busy || is_read_only)
            return lux::cxx::unexpected(
                editing::makeEditFailure(is_busy ? editing::EEditError::BUSY : editing::EEditError::BLOCKED_BY_HOST)
            );
        return {};
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::editGraph(
        GraphDelta before,
        GraphDelta after,
        std::string label,
        std::size_t node_bytes
    )
    {
        if (auto allowed = canEdit(); !allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        BusyGuard guard(busy_);
        editing::EditOperationPtr operation =
            std::make_unique<GraphEdit>(*this, std::move(before), std::move(after), std::move(label), node_bytes);
        return history_->execute(operation);
    }

    bool FlowForgeEditor::Impl::variableReferenced(const lux::flowforge::FlowGraph& graph, std::uint64_t id) noexcept
    {
        for (const auto& storage : graph.nodes())
        {
            const auto& node = *storage.node;
            if ((node.operation() == lux::flowforge::ENodeOperation::GET_VARIABLE &&
                 static_cast<const lux::flowforge::GetVariableNode&>(node).variableId() == id) ||
                (node.operation() == lux::flowforge::ENodeOperation::SET_VARIABLE &&
                 static_cast<const lux::flowforge::SetVariableNode&>(node).variableId() == id))
            {
                return true;
            }
        }
        return false;
    }

    FlowForgeEditor::FlowForgeEditor(lux::ui::Root& parent, lux::ui::PaneId id, std::unique_ptr<Impl> data, EditorResult<void>& status)
        : lux::ui::Pane(
              parent,
              std::move(id),
              lux::ui::PaneTypeId{kFlowForgeEditorType},
              "FlowForge Editor"
          ),
          impl_(std::move(data))
    {
        impl_->editor_ = this;
        impl_->createContent(status);
        impl_->close_connection_ = lux::editor::detail::takeConnection(
            lux::object::LuxObject::connect(
                this,
                &lux::ui::Pane::closeRequested,
                [this]() noexcept { impl_->hide_requested_ = true; }
            ),
            status
        );
    }
    FlowForgeEditor::~FlowForgeEditor() = default;

    EditorResult<std::unique_ptr<FlowForgeEditor>> FlowForgeEditor::create(
        lux::ui::Root& parent,
        lux::ui::PaneId id,
        EditorContext& context
    ) noexcept
    try
    {
        struct Selection final
        {
            std::shared_ptr<const void> code{acquireEditorReflection()};
            std::vector<const lux::meta::RefClass*> classes;
            std::vector<const lux::meta::RefFunction*> functions;
        };
        auto selected = std::make_shared<Selection>();
        const auto& registry = lux::meta::ReflectionRegistry::instance();
        for (const auto& type : registry.classes())
            if (type && type->type.size)
                selected->classes.push_back(type.get());
        for (const auto& function : registry.functions())
            if (function)
                selected->functions.push_back(function.get());
        auto data = std::make_unique<Impl>(context);
        data->environment_.classes = selected->classes;
        data->environment_.functions = selected->functions;
        data->environment_.code_lifetime = selected;
        auto valid = lux::flowforge::validateFlowSourceEnvironment(data->environment_);
        if (!valid)
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::INVALID_ARGUMENT, "flowforge.metadata", 0, {}, valid.error()}
            );
        EditorResult<void> status;
        auto result = std::unique_ptr<FlowForgeEditor>(new FlowForgeEditor(parent, std::move(id), std::move(data), status));
        if (!status)
            return lux::cxx::unexpected(status.error());
        return result;
    }
    catch (const std::bad_alloc&)
    {
        std::terminate();
    }
    catch (...)
    {
        return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "flowforge.create"});
    }

    EditorResult<lux::flowforge::FlowSource> FlowForgeEditor::Impl::capture() const
    {
        auto result = lux::flowforge::captureFlowSource(this->source_.id, this->source_.name, this->source_.graph);
        if (!result)
        {
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "flowforge.capture",
                static_cast<std::uint64_t>(result.error().code),
                result.error().field,
                result.error()
            });
        }
        return std::move(*result);
    }
    ProjectStorage& FlowForgeEditor::Impl::project() noexcept
    {
        return editor_context_.project();
    }
    const lux::flowforge::FlowSourceEnvironment& FlowForgeEditor::Impl::metadata() const noexcept
    {
        return this->environment_;
    }
    std::span<const lux::graph::NodeRecord> FlowForgeEditor::Impl::nodes() const noexcept
    {
        return this->source_.graph.topology().nodes();
    }
    std::span<const lux::graph::PinRecord> FlowForgeEditor::Impl::pins() const noexcept
    {
        return this->source_.graph.topology().pins();
    }
    std::span<const lux::graph::LinkRecord> FlowForgeEditor::Impl::links() const noexcept
    {
        return this->source_.graph.topology().links();
    }
    std::string_view FlowForgeEditor::Impl::nodeName(lux::flowforge::NodeId id) const noexcept
    {
        const auto found = this->read_nodes_.find(id);
        const auto* node = found == this->read_nodes_.end() ? nullptr : found->second;
        return node ? std::string_view(node->name()) : std::string_view{};
    }
    lux::flowforge::ENodeOperation FlowForgeEditor::Impl::nodeOperation(lux::flowforge::NodeId id) const noexcept
    {
        const auto found = this->read_nodes_.find(id);
        const auto* node = found == this->read_nodes_.end() ? nullptr : found->second;
        return node ? node->operation() : lux::flowforge::ENodeOperation::INVALID;
    }
    std::string_view FlowForgeEditor::Impl::pinName(lux::flowforge::PinId id) const noexcept
    {
        const auto found = this->read_pins_.find(id);
        const auto* pin = found == this->read_pins_.end() ? nullptr : found->second;
        return pin ? std::string_view(pin->name()) : std::string_view{};
    }
    std::string_view FlowForgeEditor::Impl::pinType(lux::flowforge::PinId id) const noexcept
    {
        const auto found = this->read_pins_.find(id);
        const auto* pin = found == this->read_pins_.end() ? nullptr : found->second;
        if (pin && pin->kind() == lux::flowforge::EPinKind::DATA_IN)
        {
            return static_cast<const lux::flowforge::DataInPin*>(pin)->info().type->name;
        }
        if (pin && pin->kind() == lux::flowforge::EPinKind::DATA_OUT)
        {
            return static_cast<const lux::flowforge::DataOutPin*>(pin)->info().type->name;
        }
        return {};
    }
    lux::graph::GraphNodeLayout FlowForgeEditor::Impl::nodeLayout(lux::flowforge::NodeId id) const noexcept
    {
        const auto* layout = this->source_.graph.layout().find(id);
        return layout ? *layout : lux::graph::GraphNodeLayout{};
    }

    EditorResult<lux::flowforge::FlowSourceLiteral> FlowForgeEditor::Impl::pinLiteral(lux::flowforge::PinId id) const
    {
        const auto found = this->read_pins_.find(id);
        if (found == this->read_pins_.end() || found->second->kind() != lux::flowforge::EPinKind::DATA_IN)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "flowforge.literal"});
        }
        const auto& pin = *static_cast<const lux::flowforge::DataInPin*>(found->second);
        auto result = lux::flowforge::captureFlowLiteral(pin.constantData());
        if (!result)
        {
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "flowforge.literal",
                static_cast<std::uint64_t>(result.error().code),
                result.error().field,
                result.error()
            });
        }
        return *result;
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::setPinLiteral(
        lux::flowforge::PinId id,
        const lux::flowforge::FlowSourceLiteral& literal
    )
    {
        if (auto allowed = this->canEdit(); !allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        const auto found = this->read_pins_.find(id);
        if (found == this->read_pins_.end() || found->second->kind() != lux::flowforge::EPinKind::DATA_IN)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        const auto& pin = *static_cast<const lux::flowforge::DataInPin*>(found->second);
        auto value = lux::flowforge::materializeFlowLiteral(literal, *pin.info().type);
        if (!value)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::PRECONDITION_FAILED,
                static_cast<std::uint64_t>(value.error().code)
            ));
        }
        auto before = lux::flowforge::captureFlowLiteral(pin.constantData());
        auto after = lux::flowforge::captureFlowLiteral(*value);
        if (!before || !after)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        editing::EditOperationPtr operation =
            std::make_unique<Impl::LiteralEdit>(*this, id, std::move(*before), std::move(*after));
        BusyGuard guard(this->busy_);
        return this->history_->execute(operation);
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::rename(std::string_view name)
    {
        if (name.empty() || name.size() > 4096)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        lux::flowforge::FlowSource candidate{this->source_.id, std::string(name), {}};
        const auto valid = lux::flowforge::validateFlowSource(candidate);
        if (!valid)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::INVALID_ARGUMENT,
                static_cast<std::uint64_t>(valid.error().code)
            ));
        }
        return this->change(NameAccess{}, std::move(candidate.name), "Rename FlowForge");
    }
    editing::EditResult<lux::flowforge::NodeId> FlowForgeEditor::Impl::insertNode(
        std::unique_ptr<lux::flowforge::Node>& input,
        lux::graph::GraphNodeLayout placement
    )
    {
        if (auto allowed = this->canEdit(); !allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        if (!input || input->graph() || !std::isfinite(placement.x) || !std::isfinite(placement.y))
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        if (!registeredMetadata(*input, this->environment_))
        {
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::PRECONDITION_FAILED,
                static_cast<std::uint64_t>(lux::flowforge::EFlowSourceError::UNKNOWN_REFLECTION_MEMBER)
            ));
        }
        auto captured = lux::flowforge::captureFlowNode(*input);
        if (!captured)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::PRECONDITION_FAILED,
                static_cast<std::uint64_t>(captured.error().code)
            ));
        }
        Impl::GraphDelta before, after;
        before.erase.resize(1);
        after.restore = true;
        after.place.push_back({{}, placement});
        auto command = std::make_unique<Impl::GraphEdit>(
            *this,
            std::move(before),
            std::move(after),
            "Insert FlowForge node",
            nodeStorageBytes(*input),
            &input
        );
        auto* result = command.get();
        editing::EditOperationPtr operation = std::move(command);
        BusyGuard guard(this->busy_);
        auto applied = this->history_->execute(operation);
        if (!applied)
        {
            return lux::cxx::unexpected(applied.error());
        }
        return result->inserted();
    }
    EditorResult<lux::flowforge::FlowSourceNode> FlowForgeEditor::Impl::captureNode(lux::flowforge::NodeId id) const
    {
        const auto found = this->read_nodes_.find(id);
        if (found == this->read_nodes_.end())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "flowforge.node"});
        }
        auto value = lux::flowforge::captureFlowNode(*found->second);
        if (!value)
        {
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "flowforge.node",
                static_cast<std::uint64_t>(value.error().code),
                {},
                value.error()
            });
        }
        value->layout = nodeLayout(id);
        return std::move(*value);
    }

    editing::EditResult<lux::flowforge::NodeId> FlowForgeEditor::Impl::insertFunctionUse(
        lux::flowforge::NodeId id,
        bool return_node,
        lux::graph::GraphNodeLayout layout
    )
    {
        using namespace lux::flowforge;
        if (auto allowed = this->canEdit(); !allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        const auto found = this->read_nodes_.find(id);
        if (found == this->read_nodes_.end() || found->second->operation() != ENodeOperation::FUNC_DEF_START)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        const auto& definition = static_cast<const FuncDefNode&>(*found->second);
        std::unique_ptr<Node> node;
        if (return_node)
        {
            node = std::make_unique<FuncReturnNode>(0, definition);
        }
        else
        {
            node = std::make_unique<GraphFuncCallNode>(0, definition);
        }
        return insertNode(node, layout);
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::setFunctionSignature(
        editing::StateId base,
        lux::flowforge::NodeId id,
        std::string_view name,
        const lux::flowforge::FlowSourceSignature& signature
    )
    {
        using namespace lux::flowforge;
        const auto failure = [](EFlowSourceError code) {
            return lux::cxx::unexpected(
                editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED, static_cast<std::uint64_t>(code))
            );
        };
        if (auto allowed = this->canEdit(); !allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        if (base != this->history_->view()->snapshot.current)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_BASE));
        }
        const auto found = this->read_nodes_.find(id);
        if (found == this->read_nodes_.end())
        {
            return failure(EFlowSourceError::INVALID_IDENTITY);
        }
        const auto& entry = *found->second;
        const bool function = entry.operation() == ENodeOperation::FUNC_DEF_START;
        const bool event = entry.operation() == ENodeOperation::ON_EVENT;
        const bool invalid_name = name.empty() || name.size() > FlowSourceLimits{}.max_string_bytes ||
                                  name.find('\0') != std::string_view::npos;
        if ((!function && !event) || invalid_name || (event && !signature.results.empty()))
        {
            return failure(EFlowSourceError::INVALID_VALUE);
        }
        auto args = materializeFlowArguments(signature.arguments, this->environment_);
        auto results = materializeFlowArguments(signature.results, this->environment_);
        if (!args || !results)
        {
            return failure(!args ? args.error().code : results.error().code);
        }
        const auto before_source = captureFlowNode(entry);
        if (!before_source)
        {
            return failure(before_source.error().code);
        }
        if (entry.name() == name && std::get<FlowSourceSignature>(before_source->parameters) == signature)
        {
            return this->editGraph({}, {}, "Edit function signature");
        }

        std::vector<std::unique_ptr<Node>> replacements;
        if (function)
        {
            replacements.push_back(std::make_unique<FuncDefNode>(id.value, name, std::move(*args), std::move(*results))
            );
            const auto& definition = static_cast<const FuncDefNode&>(*replacements.front());
            for (const auto& storage : this->source_.graph.nodes())
            {
                const auto& node = *storage.node;
                if (node.operation() == ENodeOperation::FUNC_RETURN &&
                    static_cast<const FuncReturnNode&>(node).def() == &entry)
                {
                    replacements.push_back(std::make_unique<FuncReturnNode>(node.id().value, definition));
                }
                else if (node.operation() == ENodeOperation::GRAPH_FUNC_CALL &&
                         static_cast<const GraphFuncCallNode&>(node).callee() == &entry)
                {
                    replacements.push_back(std::make_unique<GraphFuncCallNode>(node.id().value, definition));
                }
            }
        }
        else
        {
            replacements.push_back(std::make_unique<OnEventNode>(id.value, name, std::move(*args)));
        }
        Impl::GraphDelta before, after;
        before.restore = after.restore = true;
        std::size_t bytes{};
        for (const auto& replacement : replacements)
        {
            const auto& old = *this->read_nodes_.at(replacement->id());
            if (replacement->id() != id)
            {
                replacement->setName(old.name());
            }
            // Argument/result positions are stable identities. Removing a linked position is rejected below.
            for (const bool input : {true, false})
            {
                const auto& prior = input ? old.inPins() : old.outPins();
                const auto& next = input ? replacement->inPins() : replacement->outPins();
                for (std::size_t index{}; index < next.size(); ++index)
                {
                    auto* pin = next[index];
                    if (index < prior.size() && !FlowGraph::assignDetachedPinId(*pin, prior[index]->id()))
                    {
                        return failure(EFlowSourceError::INVALID_IDENTITY);
                    }
                    if (input && index < prior.size() && pin->kind() == EPinKind::DATA_IN)
                    {
                        const auto& old_input = static_cast<const DataInPin&>(*prior[index]);
                        auto& new_input = static_cast<DataInPin&>(*pin);
                        if (old_input.constantData().isValid())
                        {
                            auto literal = captureFlowLiteral(old_input.constantData());
                            if (!literal)
                            {
                                return failure(literal.error().code);
                            }
                            auto value = materializeFlowLiteral(*literal, *new_input.info().type);
                            if (!value || !new_input.setConstantData(std::move(*value)))
                            {
                                return failure(EFlowSourceError::INVALID_VALUE);
                            }
                        }
                    }
                }
            }
            bytes += nodeStorageBytes(old) + nodeStorageBytes(*replacement);
            before.erase.push_back(old.id());
            after.erase.push_back(old.id());
            const auto layout = nodeLayout(old.id());
            before.place.push_back({old.id(), layout});
            after.place.push_back({old.id(), layout});
        }
        for (const auto& link : links())
        {
            const auto from = this->source_.graph.topology().findPin(link.from)->owner;
            const auto to = this->source_.graph.topology().findPin(link.to)->owner;
            if (std::ranges::find(before.erase, from) != before.erase.end() ||
                std::ranges::find(before.erase, to) != before.erase.end())
            {
                before.connect.push_back(link);
                after.connect.push_back(link);
            }
        }
        editing::EditOperationPtr edit = std::make_unique<Impl::GraphEdit>(
            *this,
            std::move(before),
            std::move(after),
            "Edit function signature",
            bytes,
            nullptr,
            std::move(replacements)
        );
        BusyGuard guard(this->busy_);
        return this->history_->execute(edit);
    }

    std::span<const lux::flowforge::ExportMethodNode> FlowForgeEditor::Impl::exports() const noexcept
    {
        return this->source_.graph.exports();
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::setExports(
        std::vector<lux::flowforge::ExportMethodNode> exports
    )
    {
        if (auto allowed = this->canEdit(); !allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        const auto limits = lux::flowforge::FlowSourceLimits{};
        if (exports.size() > limits.max_exports)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        std::unordered_set<std::uint64_t> identities, symbols;
        for (const auto& value : exports)
        {
            const auto found = this->read_nodes_.find(value.entry_node_id);
            const bool invalid_entry = found == this->read_nodes_.end() ||
                                       found->second->operation() != lux::flowforge::ENodeOperation::ON_EVENT;
            if (invalid_entry || !value.id.value || !value.symbol || !identities.insert(value.id.value).second ||
                !symbols.insert(value.symbol).second || value.binding_hints.size() > limits.max_exports)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
            }
            for (const auto& hint : value.binding_hints)
            {
                if (hint.kind > lux::script::EScriptBindingHintKind::EVENT || hint.qualified_name.empty() ||
                    hint.qualified_name.size() > limits.max_string_bytes ||
                    hint.qualified_name.find('\0') != std::string::npos)
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
                }
            }
        }
        return this->change(Impl::ExportsAccess{}, std::move(exports), "Set FlowForge exports");
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::removeNodes(
        std::span<const lux::flowforge::NodeId> nodes,
        std::span<const lux::graph::LinkRecord> links
    )
    {
        Impl::GraphDelta before, after;
        after.erase.assign(nodes.begin(), nodes.end());
        std::ranges::sort(after.erase);
        if (std::adjacent_find(after.erase.begin(), after.erase.end()) != after.erase.end())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        std::size_t bytes{};
        before.restore = !nodes.empty();
        for (const auto id : nodes)
        {
            const auto* node = this->source_.graph.findNodeById(id);
            if (!node)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
            }
            bytes += nodeStorageBytes(*node);
            if (const auto* layout = this->source_.graph.layout().find(id))
            {
                before.place.push_back({id, *layout});
            }
        }
        for (const auto& link : this->source_.graph.topology().links())
        {
            const auto& topology = this->source_.graph.topology();
            const auto from = topology.findPin(link.from)->owner;
            const auto to = topology.findPin(link.to)->owner;
            if (std::ranges::binary_search(after.erase, from) || std::ranges::binary_search(after.erase, to))
            {
                before.connect.push_back(link);
            }
        }
        for (const auto& link : links)
        {
            if (std::ranges::find(before.connect, link) == before.connect.end())
            {
                before.connect.push_back(link);
                after.disconnect.push_back(link);
            }
        }
        return this->editGraph(std::move(before), std::move(after), "Remove FlowForge nodes", bytes);
    }
    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::connect(
        lux::flowforge::PinId from,
        lux::flowforge::PinId to
    )
    {
        Impl::GraphDelta before, after;
        const auto& topology = this->source_.graph.topology();
        if (!topology.findLink(from, to))
        {
            for (const auto& link : topology.links())
            {
                if ((link.to == to && topology.findPin(to) && topology.findPin(to)->fan_cap == 1) ||
                    (link.from == from && topology.findPin(from) && topology.findPin(from)->fan_cap == 1))
                {
                    after.disconnect.push_back(link);
                    before.connect.push_back(link);
                }
            }
            after.connect.push_back({from, to});
            before.disconnect.push_back({from, to});
        }
        return this->editGraph(std::move(before), std::move(after), "Connect pins");
    }
    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::disconnect(
        lux::flowforge::PinId from,
        lux::flowforge::PinId to
    )
    {
        Impl::GraphDelta before, after;
        after.disconnect.push_back({from, to});
        before.connect.push_back({from, to});
        return this->editGraph(std::move(before), std::move(after), "Disconnect pins");
    }
    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::moveNodes(
        std::span<const lux::graph::GraphLayoutEntry> entries
    )
    {
        Impl::GraphDelta before, after;
        for (const auto& entry : entries)
        {
            const auto* current = this->source_.graph.layout().find(entry.node);
            if (!current || *current != entry.layout)
            {
                if (std::ranges::find(after.place, entry.node, &lux::graph::GraphLayoutEntry::node) !=
                    after.place.end())
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
                }
                after.place.push_back(entry);
                if (current)
                {
                    before.place.push_back({entry.node, *current});
                }
                else
                {
                    before.unplace.push_back(entry.node);
                }
            }
        }
        return this->editGraph(std::move(before), std::move(after), "Move nodes");
    }

    std::span<const lux::flowforge::FlowGraph::GraphVariable> FlowForgeEditor::Impl::variables() const noexcept
    {
        return this->source_.graph.variables();
    }

    editing::EditResult<std::uint64_t> FlowForgeEditor::Impl::addVariable(
        std::string_view name,
        std::string_view type,
        const lux::flowforge::FlowSourceLiteral& initial
    )
    {
        if (auto allowed = this->canEdit(); !allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        const auto id = this->source_.graph.nextVariableId();
        if (id == UINT64_MAX || variables().size() >= lux::flowforge::FlowSourceLimits{}.max_variables)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::ID_EXHAUSTED));
        }
        if (std::ranges::find(variables(), name, &lux::flowforge::FlowGraph::GraphVariable::name) != variables().end())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        auto variable = lux::flowforge::materializeFlowVariable(
            {id, std::string(name), std::string(type), initial},
            this->environment_
        );
        if (!variable)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::PRECONDITION_FAILED,
                static_cast<std::uint64_t>(variable.error().code)
            ));
        }
        auto captured = lux::flowforge::captureFlowVariable(*variable);
        if (!captured)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        editing::EditOperationPtr edit = std::make_unique<Impl::VariableEdit>(
            *this,
            Impl::VariableEdit::Membership{std::move(*captured), variables().size(), true}
        );
        BusyGuard guard(this->busy_);
        auto applied = this->history_->execute(edit);
        if (!applied)
        {
            return lux::cxx::unexpected(applied.error());
        }
        return id;
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::setVariable(
        const lux::flowforge::FlowSourceVariable& value
    )
    {
        if (auto allowed = this->canEdit(); !allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        const auto found = std::ranges::find(variables(), value.id, &lux::flowforge::FlowGraph::GraphVariable::id);
        const bool duplicate_name = std::ranges::any_of(variables(), [&](const auto& variable) {
            return variable.id != value.id && variable.name == value.name;
        });
        if (found == variables().end() || duplicate_name)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        auto before = lux::flowforge::captureFlowVariable(*found);
        auto variable = lux::flowforge::materializeFlowVariable(value, this->environment_);
        if (!before || !variable)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        auto after = lux::flowforge::captureFlowVariable(*variable);
        if (!after)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        editing::EditOperationPtr edit = std::make_unique<Impl::VariableEdit>(
            *this,
            Impl::VariableEdit::Replacement{
                std::move(*before),
                std::move(*after),
                static_cast<std::size_t>(found - variables().begin())
            }
        );
        BusyGuard guard(this->busy_);
        return this->history_->execute(edit);
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::Impl::removeVariable(std::uint64_t id)
    {
        if (auto allowed = this->canEdit(); !allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        const auto found = std::ranges::find(variables(), id, &lux::flowforge::FlowGraph::GraphVariable::id);
        if (found == variables().end())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        auto before = lux::flowforge::captureFlowVariable(*found);
        if (!before)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        editing::EditOperationPtr edit = std::make_unique<Impl::VariableEdit>(
            *this,
            Impl::VariableEdit::Membership{
                std::move(*before),
                static_cast<std::size_t>(found - variables().begin()),
                false
            }
        );
        BusyGuard guard(this->busy_);
        return this->history_->execute(edit);
    }

    EditorResult<SaveRequestId> FlowForgeEditor::Impl::requestSave(std::string origin)
    {
        if (!history_)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "flowforge.empty"});
        if (asset_status_.phase != EAssetEditPhase::IDLE && asset_status_.phase != EAssetEditPhase::REVIEW)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.save"});
        if (!editor_context_.project().writable())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "flowforge.save"});
        }
        if (origin.empty() || !editor_context_.execution().blocking())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "flowforge.save"});
        }
        if (this->busy_ || this->save_.index() != 0)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.save"});
        }
        if (this->next_save_ == UINT64_MAX)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "flowforge.save"});
        }
        auto capture = this->capture();
        if (!capture)
        {
            return lux::cxx::unexpected(capture.error());
        }
        auto target = lux::editor::detail::captureAssetSaveTarget(editor_context_.project(), this->source_.id);
        if (!target)
            return lux::cxx::unexpected(target.error());
        auto ticket = this->persistence_.capture();
        if (!ticket)
        {
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::BUSY, "asset.save.ticket", static_cast<std::uint64_t>(ticket.error())
            });
        }
        const SaveRequestId id{this->history_->id(), this->next_save_++};
        this->save_.emplace<FlowSave>(
            id,
            *ticket,
            this->history_->view()->snapshot.revision,
            std::move(*target),
            std::move(*capture),
            editor_context_.project(),
            editor_context_.execution(),
            this->persistence_,
            completion_work_.requester()
        );
        return id;
    }
    std::span<const SaveRequestId> FlowForgeEditor::Impl::saveRequests() const noexcept
    {
        const auto* save_ = std::get_if<FlowSave>(&this->save_);
        return save_ ? save_->requests() : std::span<const SaveRequestId>{};
    }
    EditorResult<VSaveRequestStatus> FlowForgeEditor::Impl::saveStatus(SaveRequestId id) const
    {
        const auto* save_ = std::get_if<FlowSave>(&this->save_);
        if (!save_ || save_->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "flowforge.save"});
        }
        return save_->status();
    }
    EditorResult<void> FlowForgeEditor::Impl::retrySave(SaveRequestId id)
    {
        if (this->busy_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "asset.save.retry"});
        }

        auto* save_ = std::get_if<FlowSave>(&this->save_);
        if (!save_ || save_->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "flowforge.save"});
        }
        return save_->retry(true);
    }
    EditorResult<void> FlowForgeEditor::Impl::abandonSave(SaveRequestId id)
    {
        if (this->busy_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "asset.save.abandonSave"});
        }

        auto* save_ = std::get_if<FlowSave>(&this->save_);
        if (!save_ || save_->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "flowforge.save"});
        }
        save_->abandon();
        return {};
    }
    EditorResult<void> FlowForgeEditor::Impl::acknowledgeSave(SaveRequestId id)
    {
        if (this->busy_ || saved_history_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "asset.save.acknowledgeSave"});
        }

        auto* save_ = std::get_if<FlowSave>(&this->save_);
        if (!save_ || save_->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "flowforge.save"});
        }
        if (!save_->terminal())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.save"});
        }
        this->save_.emplace<std::monostate>();
        return {};
    }

    EditorResult<lux::process::TaskId> FlowForgeEditor::Impl::requestCompile(std::filesystem::path linker)
    {
        if (!history_ || saved_history_ || asset_status_.phase != EAssetEditPhase::IDLE)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.compile"});
        if (!editor_context_.execution().blocking() || this->busy_ || compile_task_ || compile_result_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.compile"});
        }
        auto view = this->history_->view();
        if (!view)
        {
            return lux::cxx::unexpected(historyFailure(view.error()));
        }
        auto capture = this->capture();
        if (!capture)
        {
            return lux::cxx::unexpected(capture.error());
        }
        auto& execution = editor_context_.execution();
        auto admitted = execution.submit(
            {"Compile Flow", "compiler", {}, environment_.code_lifetime},
            [source_ = std::move(*capture),
             environment_ = environment_,
             path = std::string(editor_context_.project().assetName(source_.id)),
             linker = std::move(linker),
             cpu = execution.cpu(),
             blocking = *execution.blocking()](process::TaskReporter reporter) mutable noexcept {
                return compileFlowAsset(
                    std::move(source_),
                    environment_,
                    std::move(path),
                    std::move(linker),
                    cpu,
                    blocking,
                    reporter
                );
            },
            [this](process::TTaskResult<FlowCompiled, FlowCompilationFailure>&& result) noexcept {
                acceptCompilation(std::move(result));
            }
        );
        if (!admitted)
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::EXECUTION_FAILURE,
                "flowforge.compile",
                static_cast<std::uint64_t>(admitted.error()),
                {},
                admitted.error()
            });
        const auto id = admitted->id();
        compilation_.emplace<Compilation>(id, history_->id(), view->snapshot.current, view->snapshot.revision);
        compile_task_ = std::move(*admitted);
        return id;
    }
    EditorResult<SaveRequestId> FlowForgeEditor::Impl::requestPublish(lux::process::TaskId compile, std::string origin)
    {
        auto result = compiled(compile);
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        if (!editor_context_.project().writable())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "flowforge.publish"});
        }
        if (origin.empty() || !editor_context_.execution().blocking())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "flowforge.publish"});
        }
        if (this->busy_ || this->save_.index() != 0)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.publish"});
        }
        if (this->next_save_ == UINT64_MAX)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "flowforge.publish"});
        }
        auto target = lux::editor::detail::captureAssetSaveTarget(editor_context_.project(), this->source_.id);
        if (!target)
            return lux::cxx::unexpected(target.error());
        auto ticket = this->persistence_.capture();
        if (!ticket)
        {
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::BUSY, "asset.save.ticket", static_cast<std::uint64_t>(ticket.error())
            });
        }
        const SaveRequestId id{this->history_->id(), this->next_save_++};
        const auto& job = std::get<Compilation>(this->compilation_);
        const auto& image = job.output->publication;
        this->save_.emplace<FlowSave>(
            id,
            *ticket,
            job.revision,
            std::move(*target),
            image,
            editor_context_.project(),
            editor_context_.execution(),
            this->persistence_,
            completion_work_.requester()
        );
        return id;
    }

    EditorResult<VFlowCompileStatus> FlowForgeEditor::Impl::compileStatus(lux::process::TaskId id) const
    {
        const auto* job = std::get_if<Compilation>(&this->compilation_);
        if (!job || job->id != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "flowforge.compile"});
        }
        if (const auto* success = std::get_if<FlowCompileSucceeded>(&job->status))
        {
            auto value = *success;
            value.current = this->history_->view()->snapshot.current == value.captured;
            return VFlowCompileStatus{value};
        }
        return job->status;
    }
    EditorResult<std::reference_wrapper<const lux::script::ScriptArtifact>> FlowForgeEditor::Impl::compiled(
        lux::process::TaskId id
    ) const
    {
        auto status = compileStatus(id);
        if (!status)
        {
            return lux::cxx::unexpected(status.error());
        }
        if (const auto* failed = std::get_if<FlowCompileFailed>(&*status))
        {
            return lux::cxx::unexpected(failed->failure);
        }
        const auto* success = std::get_if<FlowCompileSucceeded>(&*status);
        if (!success || !success->current)
        {
            return lux::cxx::unexpected(
                EditorFailure{success ? EEditorError::STALE_REQUEST : EEditorError::BUSY, "flowforge.compile"}
            );
        }
        return std::cref(std::get<Compilation>(this->compilation_).output->artifact->data());
    }

    EditorResult<void> FlowForgeEditor::Impl::retryLink(lux::process::TaskId id, std::filesystem::path linker)
    {
        auto* job = std::get_if<Compilation>(&this->compilation_);
        if (!job || job->id != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "flowforge.link.retry"});
        }
        const auto* failure = std::get_if<FlowCompileFailed>(&job->status);
        if (!failure || !failure->retryable || this->busy_)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.link.retry"});
        }
        auto& execution = editor_context_.execution();
        auto admitted = execution.submit(
            {"Retry Flow link", "compiler", job->id, environment_.code_lifetime},
            [&, linker = std::move(linker)](process::TaskReporter reporter) mutable noexcept {
                return linkFlowAsset(
                    std::move(*job->retry),
                    std::move(linker),
                    execution.cpu(),
                    *execution.blocking(),
                    reporter
                );
            },
            [this](process::TTaskResult<FlowCompiled, FlowCompilationFailure>&& result) noexcept {
                acceptCompilation(std::move(result));
            }
        );
        if (!admitted)
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::EXECUTION_FAILURE,
                "flowforge.link.retry",
                static_cast<std::uint64_t>(admitted.error()),
                {},
                admitted.error()
            });
        job->retry.reset();
        job->status = FlowCompilePending{EFlowCompileStage::LINKING};
        compile_task_ = std::move(*admitted);
        return {};
    }

    editing::HistoryId FlowForgeEditor::Impl::historyId() const noexcept
    {
        return history_ ? history_->id() : editing::HistoryId{};
    }
    editing::EditResult<editing::HistoryTargetView> FlowForgeEditor::Impl::historyView() const noexcept
    {
        if (!history_)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::NO_ACTIVE_TARGET));
        auto value = this->history_->view();
        if (!value)
        {
            return lux::cxx::unexpected(value.error());
        }
        using Availability = editing::EHistoryActionAvailability;
        if (this->busy_ || saved_history_ || asset_status_.phase != EAssetEditPhase::IDLE)
        {
            return editing::HistoryTargetView{value->snapshot, Availability::BUSY, Availability::BUSY, {}, {}};
        }
        return editing::HistoryTargetView{
            value->snapshot,
            value->can_undo ? Availability::READY : Availability::EMPTY,
            value->can_redo ? Availability::READY : Availability::EMPTY,
            value->undo_label,
            value->redo_label
        };
    }
    editing::EditResult<editing::HistoryTargetResult> FlowForgeEditor::Impl::undo() noexcept
    {
        if (auto allowed = canEdit(); !allowed)
            return lux::cxx::unexpected(allowed.error());
        BusyGuard guard(this->busy_);
        auto result = this->history_->undo();
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        return editing::HistoryTargetResult{editing::EHistoryTargetOutcome::CONTENT_APPLIED, *result};
    }
    editing::EditResult<editing::HistoryTargetResult> FlowForgeEditor::Impl::redo() noexcept
    {
        if (auto allowed = canEdit(); !allowed)
            return lux::cxx::unexpected(allowed.error());
        BusyGuard guard(this->busy_);
        auto result = this->history_->redo();
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        return editing::HistoryTargetResult{editing::EHistoryTargetOutcome::CONTENT_APPLIED, *result};
    }
    void FlowForgeEditor::Impl::event(object::EventView& event) noexcept
    {
        if (auto* request = event.getIf<FinishEditingRequest>())
        {
            event.accept();
            request->result = finishEditing();
            return;
        }
        contentCommand(event);
        if (event.accepted())
            return;
        if (auto* query = event.getIf<AssetEditorQuery>())
        {
            event.accept();
            query->matches = !query->asset.isNull() &&
                (editor_->assetId() == query->asset || editor_->assetStatus().target == query->asset);
            return;
        }

        if (lux::editor::ui::receiveCloseRequest(*editor_, event, close_request_, close_prepared_, close_decision_))
            return;
        ui::dispatchHistoryCommand(*editor_, event);
    }

    EditorResult<void> FlowForgeEditor::Impl::finishEditing()
    {
        if (!history_)
            return {};
        if (this->busy_ || this->finishing_interaction_)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flow.finish-editing"});
        BusyGuard finishing(this->finishing_interaction_);
        return finishContentEditing();
    }

    void FlowForgeEditor::Impl::acceptCompilation(process::TTaskResult<FlowCompiled, FlowCompilationFailure>&& result
    ) noexcept
    {
        if (result)
            compile_result_.emplace(std::move(*result));
        else if (auto* domain_failure = result.error().domainFailure())
            compile_result_.emplace(lux::cxx::unexpected(std::move(*domain_failure)));
        else
        {
            EditorFailure failure{EEditorError::CANCELLED, "flowforge.compile"};
            if (const auto* error = result.error().executionFailure())
                failure = {
                    EEditorError::EXECUTION_FAILURE,
                    "flowforge.compile",
                    static_cast<std::uint64_t>(*error),
                    {},
                    *error
                };
            compile_result_.emplace(lux::cxx::unexpected(FlowCompilationFailure{std::move(failure)}));
        }
        compile_task_ = {};
        completion_work_.request();
    }

    void FlowForgeEditor::Impl::adoptCompletions() noexcept
    {
        if (busy_)
            return;
        completion_pending_ = false;
        if (auto* job = std::get_if<Compilation>(&this->compilation_); job && compile_result_)
        {
            auto result = std::move(*compile_result_);
            compile_result_.reset();
            if (result)
            {
                job->output.emplace(std::move(*result));
                job->status = FlowCompileSucceeded{job->state, job->revision, true};
            }
            else
            {
                auto error = std::move(result.error());
                job->retry = std::move(error.retry);
                job->status =
                    FlowCompileFailed{job->state, job->revision, std::move(error.failure), job->retry.has_value()};
            }
            const auto completed_id = job->id;
            BusyGuard guard(this->busy_);
            lux::editor::detail::reportSignalDelivery(
                editor_->emit(editor_->compileFinished, completed_id),
                "compileFinished"
            );
        }
        adoptAssetResults();
        ui::reportCloseDecision(*editor_, close_request_, close_prepared_, close_decision_);
    }
    void FlowForgeEditor::Impl::update() noexcept
    {
        if (this->busy_)
        {
            return;
        }
        const auto title = source_.id.isNull() ? std::string("FlowForge Editor") : source_.name;
        if (editor_->title() != title)
            editor_->setTitle(title);
        if (std::exchange(hide_requested_, false))
        {
            PaneCloseRequest request{editor_};
            static_cast<void>(object::routeEvent(*editor_, editor_->root(), request));
        }
        if (history_ &&
            asset_status_.phase == EAssetEditPhase::IDLE && !saved_history_)
            applyContentIntents();
        const bool has_pending_change = completion_pending_ || asset_status_.phase != EAssetEditPhase::IDLE;
        if (has_pending_change)
            editor_->root().deferChange(*editor_, [](object::LuxObject& target) noexcept {
                static_cast<FlowForgeEditor&>(target).impl_->applyChanges();
            });
    }

} // namespace lux::editor::flowforge

namespace lux::editor::flowforge
{
    EditorResult<SaveRequestId> FlowForgeEditor::requestSave(std::string origin)
    {
        return impl_->requestSave(std::move(origin));
    }

    std::span<const SaveRequestId> FlowForgeEditor::saveRequests() const noexcept
    {
        return impl_->saveRequests();
    }

    EditorResult<VSaveRequestStatus> FlowForgeEditor::saveStatus(SaveRequestId id) const
    {
        return impl_->saveStatus(std::move(id));
    }

    EditorResult<void> FlowForgeEditor::retrySave(SaveRequestId id)
    {
        return impl_->retrySave(std::move(id));
    }

    EditorResult<void> FlowForgeEditor::abandonSave(SaveRequestId id)
    {
        return impl_->abandonSave(std::move(id));
    }

    EditorResult<void> FlowForgeEditor::acknowledgeSave(SaveRequestId id)
    {
        return impl_->acknowledgeSave(std::move(id));
    }

    EditorResult<lux::process::TaskId> FlowForgeEditor::requestCompile(std::filesystem::path linker)
    {
        return impl_->requestCompile(std::move(linker));
    }

    EditorResult<SaveRequestId> FlowForgeEditor::requestPublish(lux::process::TaskId compile, std::string origin)
    {
        return impl_->requestPublish(std::move(compile), std::move(origin));
    }

    EditorResult<VFlowCompileStatus> FlowForgeEditor::compileStatus(lux::process::TaskId id) const
    {
        return impl_->compileStatus(std::move(id));
    }

    EditorResult<std::reference_wrapper<const lux::script::ScriptArtifact>> FlowForgeEditor::compiled(
        lux::process::TaskId id
    ) const
    {
        return impl_->compiled(std::move(id));
    }

    EditorResult<void> FlowForgeEditor::retryLink(lux::process::TaskId id, std::filesystem::path linker)
    {
        return impl_->retryLink(std::move(id), std::move(linker));
    }

    editing::HistoryId FlowForgeEditor::historyId() const noexcept
    {
        return impl_->historyId();
    }

    editing::EditResult<editing::HistoryTargetView> FlowForgeEditor::historyView() const noexcept
    {
        return impl_->historyView();
    }

    editing::EditResult<editing::HistoryTargetResult> FlowForgeEditor::undo() noexcept
    {
        return impl_->undo();
    }

    editing::EditResult<editing::HistoryTargetResult> FlowForgeEditor::redo() noexcept
    {
        return impl_->redo();
    }

    void FlowForgeEditor::event(object::EventView& event) noexcept
    {
        return impl_->event(event);
    }

    EditorResult<void> FlowForgeEditor::finishEditing()
    {
        return impl_->finishEditing();
    }

    void FlowForgeEditor::update() noexcept
    {
        impl_->update();
        lux::editor::ui::reportCloseDecision(
            *this,
            impl_->close_request_,
            impl_->close_prepared_,
            impl_->close_decision_
        );
    }
}

namespace lux::editor::flowforge
{
    void FlowForgeEditor::Impl::applyChanges() noexcept
    {
        if (busy_)
            return;
        adoptCompletions();
        applyAssetChange();
    }
    bool FlowForgeEditor::hasUnsavedChanges() const noexcept
    {
        return impl_->history_ && !impl_->persistence_.clean();
    }
    std::optional<sessions::PersistedState> FlowForgeEditor::persistedState() const noexcept
    {
        return impl_->history_ ? impl_->persistence_.persisted() : std::nullopt;
    }
}
