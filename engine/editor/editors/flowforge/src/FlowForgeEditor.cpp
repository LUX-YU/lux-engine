#include <algorithm>
#include <cmath>
#include <lux/engine/editor/detail/DocumentSave.hpp>
#include <lux/engine/editor/detail/DocumentSource.hpp>
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
        bool registeredMetadata(const lux::flowforge::Node &node,
                                const lux::flowforge::FlowSourceEnvironment &environment)
        {
            using namespace lux::flowforge;
            if (node.operation() == ENodeOperation::NATIVE_FUNC_CALL)
            {
                const auto &call = static_cast<const NativeFuncCall &>(node);
                if (!call.ownerType())
                {
                    return std::ranges::any_of(environment.functions, [&](const auto *function)
                                               { return &function->invokable == &call.info(); });
                }
                return std::ranges::any_of(environment.classes,
                                           [&](const auto *type)
                                           {
                                               return &type->type == call.ownerType() &&
                                                      std::ranges::any_of(
                                                          type->methods, [&](const auto &method)
                                                          { return &method.invokable == &call.info(); });
                                           });
            }
            if (node.operation() == ENodeOperation::GET_FIELD || node.operation() == ENodeOperation::SET_FIELD)
            {
                const bool reading = node.operation() == ENodeOperation::GET_FIELD;
                const auto *type = reading ? static_cast<const GetFieldNode &>(node).ownerClass()
                                           : static_cast<const SetFieldNode &>(node).ownerClass();
                const auto *field = reading ? static_cast<const GetFieldNode &>(node).field()
                                            : static_cast<const SetFieldNode &>(node).field();
                return std::ranges::find(environment.classes, type) != environment.classes.end() &&
                       std::ranges::any_of(type->fields,
                                           [&](const auto &registered) { return &registered == field; }) &&
                       field->visibility == lux::meta::EVisibility::Public && !field->is_volatile &&
                       (reading || !field->is_const);
            }
            if (node.operation() == ENodeOperation::SCRIPT_ABILITY_CALL)
            {
                const auto &ability = static_cast<const ScriptAbilityNode &>(node);
                const auto *registered = environment.abilities.find(ability.contract(), ability.method());
                return registered && registered->schema_version == ability.expectedSchemaVersion() &&
                       registered->schema_hash == ability.expectedSchemaHash();
            }
            if (node.operation() == ENodeOperation::SCRIPT_EVENT_WAIT)
            {
                const auto &event = static_cast<const ScriptEventAwaitNode &>(node);
                return std::ranges::find(environment.events, event.source()) != environment.events.end();
            }
            return true;
        }

        std::size_t nodeStorageBytes(const lux::flowforge::Node &node)
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
            bytes += (node.inPins().capacity() + node.outPins().capacity()) * sizeof(Pin *);
            const auto arguments = [&](const std::vector<FuncArgInfo> &values)
            {
                bytes += values.capacity() * sizeof(FuncArgInfo);
                for (const auto &value : values)
                {
                    bytes += value.name.capacity() + 1;
                }
            };
            for (const bool input : {true, false})
            {
                for (const auto *pin : input ? node.inPins() : node.outPins())
                {
                    bytes += std::max({sizeof(DataInPin), sizeof(DataOutPin), sizeof(ExecInPin), sizeof(ExecOutPin)}) +
                             pin->name().capacity() + 1;
                    if (pin->kind() == EPinKind::DATA_IN)
                    {
                        const auto &data = *static_cast<const DataInPin *>(pin);
                        bytes += data.info().name.capacity() + 1;
                        if (data.constantData().isValid())
                        {
                            bytes += data.constantData().type()->size;
                        }
                    }
                    else if (pin->kind() == EPinKind::DATA_OUT)
                    {
                        bytes += static_cast<const DataOutPin *>(pin)->info().name.capacity() + 1;
                    }
                }
            }
            if (const auto *exec = dynamic_cast<const ExecIntermediateNode *>(&node))
            {
                bytes += exec->extraOutPinStorageBytes();
            }
            switch (node.operation())
            {
            case ENodeOperation::FUNC_DEF_START:
            {
                const auto &value = static_cast<const FuncDefNode &>(node);
                arguments(value.argInfos());
                arguments(value.retInfos());
                bytes += value.argPins().capacity() * sizeof(void *) + value.extraOutPinStorageBytes();
                break;
            }
            case ENodeOperation::FUNC_RETURN:
                bytes += static_cast<const FuncReturnNode &>(node).retPins().capacity() * sizeof(void *);
                break;
            case ENodeOperation::ON_EVENT:
            {
                const auto &value = static_cast<const OnEventNode &>(node);
                arguments(value.paramInfos());
                bytes += value.paramPins().capacity() * sizeof(void *) + value.extraOutPinStorageBytes();
                break;
            }
            case ENodeOperation::GRAPH_FUNC_CALL:
            {
                const auto &value = static_cast<const GraphFuncCallNode &>(node);
                bytes += (value.argPins().capacity() + value.resultPins().capacity()) * sizeof(void *);
                break;
            }
            case ENodeOperation::NATIVE_FUNC_CALL:
                bytes += static_cast<const NativeFuncCall &>(node).dataInPins().capacity() * sizeof(void *);
                break;
            case ENodeOperation::SEQUENCE:
                bytes += static_cast<const SequenceNode &>(node).execOutPins().capacity() * sizeof(void *);
                break;
            case ENodeOperation::SCRIPT_ABILITY_CALL:
                bytes += static_cast<const ScriptAbilityNode &>(node).descriptionBytes();
                break;
            case ENodeOperation::SCRIPT_EVENT_WAIT:
                bytes += static_cast<const ScriptEventAwaitNode &>(node).descriptionBytes();
                break;
            default:
                break;
            }
            return bytes;
        }
        EditorFailure historyFailure(const editing::EditFailure &failure)
        {
            return {EEditorError::INVALID_STATE,
                    "flowforge.history",
                    static_cast<std::uint64_t>(failure.code),
                    {},
                    failure};
        }
        using FlowSave = detail::DocumentSave<lux::flowforge::FlowSourceDocument, FlowEncoder>;

        struct Content final
        {
            lux::asset::AssetId id;
            std::string name;
            lux::flowforge::FlowGraph graph;
        };
        struct NameAccess final
        {
            using Value = std::string;
            bool exists(const Content &) const noexcept
            {
                return true;
            }
            Value read(const Content &source) const
            {
                return source.name;
            }
            void exchange(Content &source, Value &value) const noexcept
            {
                source.name.swap(value);
            }
            static bool equal(const Value &a, const Value &b) noexcept
            {
                return a == b;
            }
            static std::size_t bytes(const Value &value) noexcept
            {
                return value.capacity() + 1U;
            }
        };
        struct BusyGuard final
        {
            bool &busy;
            explicit BusyGuard(bool &value) : busy(value)
            {
                busy = true;
            }
            ~BusyGuard()
            {
                busy = false;
            }
        };
    } // namespace

    struct FlowForgeEditor::Data final
    {
        using NodeIndex = std::unordered_map<lux::flowforge::NodeId, const lux::flowforge::Node *>;
        using PinIndex = std::unordered_map<lux::flowforge::PinId, const lux::flowforge::Pin *>;
        static bool variableReferenced(const lux::flowforge::FlowGraph &graph, std::uint64_t id) noexcept
        {
            for (const auto &storage : graph.nodes())
            {
                const auto &node = *storage.node;
                if ((node.operation() == lux::flowforge::ENodeOperation::GET_VARIABLE &&
                     static_cast<const lux::flowforge::GetVariableNode &>(node).variableId() == id) ||
                    (node.operation() == lux::flowforge::ENodeOperation::SET_VARIABLE &&
                     static_cast<const lux::flowforge::SetVariableNode &>(node).variableId() == id))
                {
                    return true;
                }
            }
            return false;
        }
        class VariableEdit final : public editing::EditOperation
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
            using Mutation = std::variant<Membership, Replacement>;

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
                Plan(const VariableEdit &edit, EWrite write, std::size_t position,
                     std::vector<lux::flowforge::FlowGraph::GraphVariable> staged, bool changed = true)
                    : edit_(edit), write_(write), position_(position), staged_(std::move(staged)), changed_(changed)
                {
                }
                editing::EEditEffect effect() const noexcept override
                {
                    return changed_ ? editing::EEditEffect::CHANGE : editing::EEditEffect::NO_CHANGE;
                }

              private:
                void apply() noexcept override
                {
                    auto &variables = edit_.owner_.source.graph.variables();
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
                    edit_.owner_.source.graph.exchangeVariables(staged_);
                }
                void publish(const editing::CommitInfo &info) noexcept override
                {
                    edit_.owner_.editor->notify<FlowForgeEditor::contentChanged>(info.revision);
                }
                const VariableEdit &edit_;
                EWrite write_;
                std::size_t position_;
                std::vector<lux::flowforge::FlowGraph::GraphVariable> staged_;
                bool changed_;
            };
            static std::size_t bytes(const lux::flowforge::FlowSourceVariable &value) noexcept
            {
                return value.name.capacity() + value.type.capacity() + value.value.value.capacity() + 3;
            }
            bool matches(const lux::flowforge::FlowSourceVariable &value, std::size_t position) const
            {
                const auto &variables = owner_.source.graph.variables();
                if (position >= variables.size() || variables[position].id != value.id)
                {
                    return false;
                }
                const auto current = lux::flowforge::captureFlowVariable(variables[position]);
                return current && *current == value;
            }
            editing::EditResult<editing::PreparedEditPtr> prepareValue(EWrite write, std::size_t position,
                                                                       const lux::flowforge::FlowSourceVariable &value,
                                                                       editing::EditPreparationBudget &budget) const
            {
                using Variable = lux::flowforge::FlowGraph::GraphVariable;
                const auto count = owner_.source.graph.variables().size();
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
                    auto variable = lux::flowforge::materializeFlowVariable(value, owner_.environment);
                    if (!variable)
                    {
                        return lux::cxx::unexpected(
                            editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED,
                                                     static_cast<std::uint64_t>(variable.error().code)));
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
            VariableEdit(Data &owner, Mutation mutation)
                : owner_(owner), mutation_(std::move(mutation)), base_(owner.history->view()->snapshot.current)
            {
            }
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
                                           [](const auto &value)
                                           {
                                               if constexpr (std::is_same_v<std::decay_t<decltype(value)>, Membership>)
                                               {
                                                   return bytes(value.value);
                                               }
                                               else
                                               {
                                                   return bytes(value.before) + bytes(value.after);
                                               }
                                           },
                                           mutation_);
            }
            editing::EditResult<editing::PreparedEditPtr> prepare(
                const editing::ApplyContext &context, editing::EditPreparationBudget &budget) const noexcept override
            {
                return std::visit(
                    [&](const auto &mutation) -> editing::EditResult<editing::PreparedEditPtr>
                    {
                        const auto rejected = []
                        {
                            return lux::cxx::unexpected(
                                editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
                        };
                        if constexpr (std::is_same_v<std::decay_t<decltype(mutation)>, Membership>)
                        {
                            const bool insert = mutation.insert == (context.direction == editing::EDirection::FORWARD);
                            if (insert)
                            {
                                if (owner_.source.graph.findVariable(mutation.value.id) ||
                                    mutation.position > owner_.source.graph.variables().size())
                                {
                                    return rejected();
                                }
                            }
                            else if (!matches(mutation.value, mutation.position) ||
                                     variableReferenced(owner_.source.graph, mutation.value.id))
                            {
                                return rejected();
                            }
                            return prepareValue(insert ? EWrite::INSERT : EWrite::ERASE, mutation.position,
                                                mutation.value, budget);
                        }
                        else
                        {
                            const auto &before =
                                context.direction == editing::EDirection::FORWARD ? mutation.before : mutation.after;
                            const auto &after =
                                context.direction == editing::EDirection::FORWARD ? mutation.after : mutation.before;
                            if (!matches(before, mutation.position) ||
                                (before.type != after.type && variableReferenced(owner_.source.graph, before.id)))
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
                                    new Plan(*this, EWrite::REPLACE, mutation.position, {}, false));
                            }
                            return prepareValue(EWrite::REPLACE, mutation.position, after, budget);
                        }
                    },
                    mutation_);
            }

          private:
            Data &owner_;
            Mutation mutation_;
            editing::StateId base_;
        };

        class LiteralEdit final : public editing::EditOperation
        {
            class Plan final : public editing::PreparedEdit
            {
              public:
                Plan(const LiteralEdit &edit, lux::flowforge::DataInPin &pin, lux::meta::RuntimeObject value)
                    : edit_(edit), pin_(pin), value_(std::move(value))
                {
                }
                editing::EEditEffect effect() const noexcept override
                {
                    return edit_.before_ == edit_.after_ ? editing::EEditEffect::NO_CHANGE
                                                         : editing::EEditEffect::CHANGE;
                }

              private:
                void apply() noexcept override
                {
                    swap(pin_.constantData(), value_);
                }
                void publish(const editing::CommitInfo &info) noexcept override
                {
                    edit_.owner_.editor->notify<FlowForgeEditor::contentChanged>(info.revision);
                }
                const LiteralEdit &edit_;
                lux::flowforge::DataInPin &pin_;
                lux::meta::RuntimeObject value_;
            };

          public:
            LiteralEdit(Data &owner, lux::flowforge::PinId pin, lux::flowforge::FlowSourceLiteral before,
                        lux::flowforge::FlowSourceLiteral after)
                : owner_(owner), pin_(pin), before_(std::move(before)), after_(std::move(after)),
                  base_(owner.history->view()->snapshot.current)
            {
            }
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
                const editing::ApplyContext &context, editing::EditPreparationBudget &budget) const noexcept override
            {
                auto *base = owner_.source.graph.findPin(pin_);
                if (!base || base->kind() != lux::flowforge::EPinKind::DATA_IN)
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
                }
                auto &pin = *static_cast<lux::flowforge::DataInPin *>(base);
                const auto &expected = context.direction == editing::EDirection::FORWARD ? before_ : after_;
                const auto &next = context.direction == editing::EDirection::FORWARD ? after_ : before_;
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
                        editing::EEditError::PRECONDITION_FAILED, static_cast<std::uint64_t>(value.error().code)));
                }
                return editing::PreparedEditPtr(new Plan(*this, pin, std::move(*value)));
            }

          private:
            Data &owner_;
            lux::flowforge::PinId pin_;
            lux::flowforge::FlowSourceLiteral before_, after_;
            editing::StateId base_;
        };
        template <class Access> class ValueEdit final : public editing::EditOperation
        {
            using Value = typename Access::Value;
            class Plan final : public editing::PreparedEdit
            {
              public:
                Plan(const ValueEdit &edit, Value value, bool changed)
                    : edit_(edit), value_(std::move(value)), changed_(changed)
                {
                }
                editing::EEditEffect effect() const noexcept override
                {
                    return changed_ ? editing::EEditEffect::CHANGE : editing::EEditEffect::NO_CHANGE;
                }

              private:
                void apply() noexcept override
                {
                    edit_.access_.exchange(edit_.owner_.source, value_);
                }
                void publish(const editing::CommitInfo &info) noexcept override
                {
                    edit_.owner_.editor->notify<FlowForgeEditor::contentChanged>(info.revision);
                }
                const ValueEdit &edit_;
                Value value_;
                bool changed_;
            };

          public:
            ValueEdit(Data &owner, Access access, Value after, std::string label, editing::StateId base)
                : owner_(owner), access_(access), before_(access.read(owner.source)), after_(std::move(after)),
                  label_(std::move(label)), base_(base)
            {
            }
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
                const editing::ApplyContext &context, editing::EditPreparationBudget &budget) const noexcept override
            {
                const auto &expected = context.direction == editing::EDirection::FORWARD ? before_ : after_;
                const auto &next = context.direction == editing::EDirection::FORWARD ? after_ : before_;
                if (!access_.exists(owner_.source) || !Access::equal(access_.read(owner_.source), expected))
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
            Data &owner_;
            Access access_;
            Value before_, after_;
            std::string label_;
            editing::StateId base_;
        };

        struct GraphDelta final
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
        struct ExportsAccess final
        {
            using Value = std::vector<lux::flowforge::ExportMethodNode>;
            bool exists(const Content &) const noexcept
            {
                return true;
            }
            Value read(const Content &source) const
            {
                return source.graph.exports();
            }
            void exchange(Content &source, Value &value) const noexcept
            {
                source.graph.exchangeExports(value);
            }
            static bool equal(const Value &a, const Value &b) noexcept
            {
                return a == b;
            }
            static std::size_t bytes(const Value &value) noexcept
            {
                std::size_t bytes = value.capacity() * sizeof(Value::value_type);
                for (const auto &item : value)
                {
                    bytes += item.binding_hints.capacity() * sizeof(lux::script::ScriptBindingHintTarget);
                    for (const auto &hint : item.binding_hints)
                    {
                        bytes += hint.qualified_name.capacity() + 1;
                    }
                }
                return bytes;
            }
        };
        class GraphEdit final : public editing::EditOperation
        {
            class Plan final : public editing::PreparedEdit
            {
              public:
                Plan(const GraphEdit &edit, lux::flowforge::FlowGraphEdit patch, const GraphDelta &delta,
                     std::span<std::unique_ptr<lux::flowforge::Node> *const> insert)
                    : edit_(edit), patch_(std::move(patch)), changed_(!delta.empty()),
                      structural_(!delta.erase.empty() || !insert.empty())
                {
                    if (!structural_)
                    {
                        return;
                    }
                    nodes_ = edit.owner_.read_nodes;
                    pins_ = edit.owner_.read_pins;
                    for (const auto id : delta.erase)
                    {
                        const auto *node = nodes_.at(id);
                        for (const auto *pin : node->inPins())
                        {
                            pins_.erase(pin->id());
                        }
                        for (const auto *pin : node->outPins())
                        {
                            pins_.erase(pin->id());
                        }
                        nodes_.erase(id);
                    }
                    for (std::size_t i = 0; i < insert.size(); ++i)
                    {
                        nodes_.emplace(patch_.insertedIds()[i], insert[i]->get());
                    }
                    for (const auto &[pin, id] : patch_.assignedPins())
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
                        edit_.owner_.read_nodes.swap(nodes_);
                        edit_.owner_.read_pins.swap(pins_);
                    }
                    edit_.parked_ = patch_.takeRemoved();
                    edit_.input_ = nullptr;
                }
                void publish(const editing::CommitInfo &info) noexcept override
                {
                    edit_.owner_.editor->notify<FlowForgeEditor::contentChanged>(info.revision);
                }
                const GraphEdit &edit_;
                lux::flowforge::FlowGraphEdit patch_;
                bool changed_;
                bool structural_;
                NodeIndex nodes_;
                PinIndex pins_;
            };

          public:
            GraphEdit(Data &owner, GraphDelta before, GraphDelta after, std::string label, std::size_t node_bytes,
                      std::unique_ptr<lux::flowforge::Node> *input = nullptr,
                      std::vector<std::unique_ptr<lux::flowforge::Node>> staged = {})
                : owner_(owner), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)),
                  base_(owner.history->view()->snapshot.current), node_bytes_(node_bytes), input_(input),
                  parked_(std::move(staged))
            {
            }
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
                const editing::ApplyContext &context, editing::EditPreparationBudget &budget) const noexcept override
            {
                const auto &delta = context.direction == editing::EDirection::FORWARD ? after_ : before_;
                const auto &topology = owner_.source.graph.topology();
                std::size_t slots{};
                for (const auto &storage : owner_.source.graph.nodes())
                {
                    slots = (std::max)(slots, storage.index + 1);
                }
                const auto structural =
                    topology.nodes().size() * sizeof(lux::graph::NodeRecord) +
                    topology.pins().size() * sizeof(lux::graph::PinRecord) +
                    topology.links().size() * sizeof(lux::graph::LinkRecord) +
                    owner_.source.graph.layout().all().size() * sizeof(lux::graph::GraphLayoutEntry) + slots * 64;
                auto reserved =
                    budget.reserve(sizeof(Plan) + 4 * (structural + before_.bytes() + after_.bytes() + node_bytes_));
                if (!reserved)
                {
                    return lux::cxx::unexpected(reserved.error());
                }
                std::vector<std::unique_ptr<lux::flowforge::Node> *> insert;
                if (delta.restore)
                {
                    if (input_)
                    {
                        insert.push_back(input_);
                    }
                    else
                    {
                        for (auto &node : parked_)
                        {
                            insert.push_back(&node);
                        }
                    }
                }
                auto patch = lux::flowforge::FlowGraphEdit::prepare(
                    owner_.source.graph,
                    {insert, delta.erase, delta.connect, delta.disconnect,
                     input_ ? std::span<const lux::graph::GraphLayoutEntry>{} : std::span{delta.place}, delta.unplace,
                     !input_});
                if (!patch)
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(
                        editing::EEditError::PRECONDITION_FAILED, static_cast<std::uint64_t>(patch.error().code)));
                }
                if (input_)
                {
                    auto placed = patch->place(patch->insertedIds().front(), after_.place.front().layout);
                    if (!placed)
                    {
                        return lux::cxx::unexpected(editing::makeEditFailure(
                            editing::EEditError::PRECONDITION_FAILED, static_cast<std::uint64_t>(placed.error().code)));
                    }
                }
                return editing::PreparedEditPtr(new Plan(*this, std::move(*patch), delta, insert));
            }
            lux::flowforge::NodeId inserted() const noexcept
            {
                return before_.erase.front();
            }

          private:
            Data &owner_;
            mutable GraphDelta before_, after_;
            std::string label_;
            editing::StateId base_;
            std::size_t node_bytes_;
            mutable std::unique_ptr<lux::flowforge::Node> *input_;
            mutable std::vector<std::unique_ptr<lux::flowforge::Node>> parked_;
        };

        editing::EditResult<editing::ApplyResult> editGraph(GraphDelta before, GraphDelta after, std::string label,
                                                            std::size_t node_bytes = 0)
        {
            if (auto allowed = canEdit(); !allowed)
            {
                return lux::cxx::unexpected(allowed.error());
            }
            BusyGuard guard(busy);
            editing::EditOperationPtr operation =
                std::make_unique<GraphEdit>(*this, std::move(before), std::move(after), std::move(label), node_bytes);
            return history->execute(operation);
        }

        editing::EditResult<void> canEdit() const noexcept
        {
            if (close != ECloseState::OPEN || close_requested)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::CLOSED));
            }
            if (busy || !project.writable())
            {
                return lux::cxx::unexpected(
                    editing::makeEditFailure(busy ? editing::EEditError::BUSY : editing::EEditError::BLOCKED_BY_HOST));
            }
            return {};
        }
        Data(Content value, Project &project_owner, process::ExecutionRuntime &execution,
             std::unique_ptr<editing::EditHistory> edits)
            : source(std::move(value)), project(project_owner), runtime(execution), history(std::move(edits))
        {
            read_nodes.reserve(source.graph.nodes().size());
            read_pins.reserve(source.graph.topology().pins().size());
            for (const auto &storage : source.graph.nodes())
            {
                const auto *node = storage.node.get();
                read_nodes.emplace(node->id(), node);
                for (const auto *pin : node->inPins())
                {
                    read_pins.emplace(pin->id(), pin);
                }
                for (const auto *pin : node->outPins())
                {
                    read_pins.emplace(pin->id(), pin);
                }
            }
        }

        template <class Access>
        editing::EditResult<editing::ApplyResult> change(Access access, typename Access::Value value, std::string label)
        {
            if (close != ECloseState::OPEN || close_requested)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::CLOSED));
            }
            if (busy || !project.writable())
            {
                return lux::cxx::unexpected(
                    editing::makeEditFailure(busy ? editing::EEditError::BUSY : editing::EEditError::BLOCKED_BY_HOST));
            }
            if (!access.exists(source))
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
            }
            auto view = history->view();
            if (!view)
            {
                return lux::cxx::unexpected(view.error());
            }
            BusyGuard guard(busy);
            editing::EditOperationPtr operation = std::make_unique<ValueEdit<Access>>(
                *this, access, std::move(value), std::move(label), view->snapshot.current);
            return history->execute(operation);
        }

        FlowForgeEditor *editor{}; // Bound once, before publishing the complete document.
        // Declared before content: metadata/module leases outlive nodes, history and in-flight compilation.
        lux::flowforge::FlowSourceEnvironment environment;
        Content source;
        NodeIndex read_nodes;
        PinIndex read_pins;
        Project &project;
        process::ExecutionRuntime &runtime;
        std::unique_ptr<editing::EditHistory> history;
        std::vector<std::unique_ptr<DocumentView>> views;
        std::variant<std::monostate, FlowSave> save;
        std::variant<std::monostate, FlowCompilation> compilation;
        std::uint64_t next_save{1}, next_compile{1};
        ECloseState close{ECloseState::OPEN};
        bool close_requested{}, busy{};
    };

    FlowForgeEditor::FlowForgeEditor(object::ObjectDispatcherRef dispatcher, std::unique_ptr<Data> data)
        : Object(dispatcher), data_(std::move(data))
    {
        data_->editor = this;
    }
    FlowForgeEditor::~FlowForgeEditor() = default;

    EditorResult<std::unique_ptr<FlowForgeEditor>> FlowForgeEditor::open(
        const lux::flowforge::FlowSourceDocument &source, Project &project, process::ExecutionRuntime &runtime,
        lux::flowforge::FlowSourceEnvironment environment)
    {
        auto graph = lux::flowforge::materializeFlowSource(source, environment);
        if (!graph)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "flowforge.materialize",
                                                      static_cast<std::uint64_t>(graph.error().code),
                                                      graph.error().field, graph.error()});
        }
        return adopt(source.id, source.name, *graph, project, runtime, environment);
    }
    EditorResult<std::unique_ptr<FlowForgeEditor>> FlowForgeEditor::adopt(
        lux::asset::AssetId id, std::string name, lux::flowforge::FlowGraph &graph, Project &project,
        process::ExecutionRuntime &runtime, lux::flowforge::FlowSourceEnvironment environment)
    {
        const auto *asset = project.asset(id);
        if (!asset || asset->kind != EProjectAssetKind::FLOW_GRAPH)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "flowforge.identity"});
        }
        auto history = editing::EditHistory::create({kLimits, {}, true});
        if (!history)
        {
            return lux::cxx::unexpected(historyFailure(history.error()));
        }
        auto data = std::make_unique<Data>(Content{id, std::move(name), std::move(graph)}, project, runtime,
                                           std::move(*history));
        data->environment = environment;
        return std::unique_ptr<FlowForgeEditor>(new FlowForgeEditor(project.dispatcherRef(), std::move(data)));
    }

    DocumentSummary FlowForgeEditor::summary() const
    {
        return {handle(),
                {data_->project.manifest().id, data_->source.id, std::string(kFlowForgeDocumentType)},
                data_->source.name,
                !data_->project.writable()};
    }
    EditorResult<lux::flowforge::FlowSourceDocument> FlowForgeEditor::capture() const
    {
        auto result = lux::flowforge::captureFlowSource(data_->source.id, data_->source.name, data_->source.graph);
        if (!result)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "flowforge.capture",
                                                      static_cast<std::uint64_t>(result.error().code),
                                                      result.error().field, result.error()});
        }
        return std::move(*result);
    }
    Project &FlowForgeEditor::project() noexcept
    {
        return data_->project;
    }
    const lux::flowforge::FlowSourceEnvironment &FlowForgeEditor::metadata() const noexcept
    {
        return data_->environment;
    }
    std::span<const lux::graph::NodeRecord> FlowForgeEditor::nodes() const noexcept
    {
        return data_->source.graph.topology().nodes();
    }
    std::span<const lux::graph::PinRecord> FlowForgeEditor::pins() const noexcept
    {
        return data_->source.graph.topology().pins();
    }
    std::span<const lux::graph::LinkRecord> FlowForgeEditor::links() const noexcept
    {
        return data_->source.graph.topology().links();
    }
    std::string_view FlowForgeEditor::nodeName(lux::flowforge::NodeId id) const noexcept
    {
        const auto found = data_->read_nodes.find(id);
        const auto *node = found == data_->read_nodes.end() ? nullptr : found->second;
        return node ? std::string_view(node->name()) : std::string_view{};
    }
    lux::flowforge::ENodeOperation FlowForgeEditor::nodeOperation(lux::flowforge::NodeId id) const noexcept
    {
        const auto found = data_->read_nodes.find(id);
        const auto *node = found == data_->read_nodes.end() ? nullptr : found->second;
        return node ? node->operation() : lux::flowforge::ENodeOperation::INVALID;
    }
    std::string_view FlowForgeEditor::pinName(lux::flowforge::PinId id) const noexcept
    {
        const auto found = data_->read_pins.find(id);
        const auto *pin = found == data_->read_pins.end() ? nullptr : found->second;
        return pin ? std::string_view(pin->name()) : std::string_view{};
    }
    std::string_view FlowForgeEditor::pinType(lux::flowforge::PinId id) const noexcept
    {
        const auto found = data_->read_pins.find(id);
        const auto *pin = found == data_->read_pins.end() ? nullptr : found->second;
        if (pin && pin->kind() == lux::flowforge::EPinKind::DATA_IN)
        {
            return static_cast<const lux::flowforge::DataInPin *>(pin)->info().type->name;
        }
        if (pin && pin->kind() == lux::flowforge::EPinKind::DATA_OUT)
        {
            return static_cast<const lux::flowforge::DataOutPin *>(pin)->info().type->name;
        }
        return {};
    }
    lux::graph::GraphNodeLayout FlowForgeEditor::nodeLayout(lux::flowforge::NodeId id) const noexcept
    {
        const auto *layout = data_->source.graph.layout().find(id);
        return layout ? *layout : lux::graph::GraphNodeLayout{};
    }

    EditorResult<lux::flowforge::FlowSourceLiteral> FlowForgeEditor::pinLiteral(lux::flowforge::PinId id) const
    {
        const auto found = data_->read_pins.find(id);
        if (found == data_->read_pins.end() || found->second->kind() != lux::flowforge::EPinKind::DATA_IN)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "flowforge.literal"});
        }
        const auto &pin = *static_cast<const lux::flowforge::DataInPin *>(found->second);
        auto result = lux::flowforge::captureFlowLiteral(pin.constantData());
        if (!result)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "flowforge.literal",
                                                      static_cast<std::uint64_t>(result.error().code),
                                                      result.error().field, result.error()});
        }
        return *result;
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::setPinLiteral(
        lux::flowforge::PinId id, const lux::flowforge::FlowSourceLiteral &literal)
    {
        if (auto allowed = data_->canEdit(); !allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        const auto found = data_->read_pins.find(id);
        if (found == data_->read_pins.end() || found->second->kind() != lux::flowforge::EPinKind::DATA_IN)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        const auto &pin = *static_cast<const lux::flowforge::DataInPin *>(found->second);
        auto value = lux::flowforge::materializeFlowLiteral(literal, *pin.info().type);
        if (!value)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED,
                                                                 static_cast<std::uint64_t>(value.error().code)));
        }
        auto before = lux::flowforge::captureFlowLiteral(pin.constantData());
        auto after = lux::flowforge::captureFlowLiteral(*value);
        if (!before || !after)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        editing::EditOperationPtr operation =
            std::make_unique<Data::LiteralEdit>(*data_, id, std::move(*before), std::move(*after));
        BusyGuard guard(data_->busy);
        return data_->history->execute(operation);
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::rename(std::string_view name)
    {
        if (name.empty() || name.size() > 4096)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        lux::flowforge::FlowSourceDocument candidate{data_->source.id, std::string(name), {}};
        const auto valid = lux::flowforge::validateFlowSource(candidate);
        if (!valid)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT,
                                                                 static_cast<std::uint64_t>(valid.error().code)));
        }
        return data_->change(NameAccess{}, std::move(candidate.name), "Rename FlowForge");
    }
    editing::EditResult<lux::flowforge::NodeId> FlowForgeEditor::insertNode(
        std::unique_ptr<lux::flowforge::Node> &input, lux::graph::GraphNodeLayout placement)
    {
        if (auto allowed = data_->canEdit(); !allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        if (!input || input->graph() || !std::isfinite(placement.x) || !std::isfinite(placement.y))
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        if (!registeredMetadata(*input, data_->environment))
        {
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::PRECONDITION_FAILED,
                static_cast<std::uint64_t>(lux::flowforge::EFlowSourceError::UNKNOWN_REFLECTION_MEMBER)));
        }
        auto captured = lux::flowforge::captureFlowNode(*input);
        if (!captured)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED,
                                                                 static_cast<std::uint64_t>(captured.error().code)));
        }
        Data::GraphDelta before, after;
        before.erase.resize(1);
        after.restore = true;
        after.place.push_back({{}, placement});
        auto command = std::make_unique<Data::GraphEdit>(*data_, std::move(before), std::move(after),
                                                         "Insert FlowForge node", nodeStorageBytes(*input), &input);
        auto *result = command.get();
        editing::EditOperationPtr operation = std::move(command);
        BusyGuard guard(data_->busy);
        auto applied = data_->history->execute(operation);
        if (!applied)
        {
            return lux::cxx::unexpected(applied.error());
        }
        return result->inserted();
    }
    EditorResult<lux::flowforge::FlowSourceNode> FlowForgeEditor::captureNode(lux::flowforge::NodeId id) const
    {
        const auto found = data_->read_nodes.find(id);
        if (found == data_->read_nodes.end())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "flowforge.node"});
        }
        auto value = lux::flowforge::captureFlowNode(*found->second);
        if (!value)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE,
                                                      "flowforge.node",
                                                      static_cast<std::uint64_t>(value.error().code),
                                                      {},
                                                      value.error()});
        }
        value->layout = nodeLayout(id);
        return std::move(*value);
    }

    editing::EditResult<lux::flowforge::NodeId> FlowForgeEditor::insertFunctionUse(lux::flowforge::NodeId id,
                                                                                   bool return_node,
                                                                                   lux::graph::GraphNodeLayout layout)
    {
        using namespace lux::flowforge;
        if (auto allowed = data_->canEdit(); !allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        const auto found = data_->read_nodes.find(id);
        if (found == data_->read_nodes.end() || found->second->operation() != ENodeOperation::FUNC_DEF_START)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        const auto &definition = static_cast<const FuncDefNode &>(*found->second);
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

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::setFunctionSignature(
        editing::StateId base, lux::flowforge::NodeId id, std::string_view name,
        const lux::flowforge::FlowSourceSignature &signature)
    {
        using namespace lux::flowforge;
        const auto failure = [](EFlowSourceError code)
        {
            return lux::cxx::unexpected(
                editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED, static_cast<std::uint64_t>(code)));
        };
        if (auto allowed = data_->canEdit(); !allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        if (base != data_->history->view()->snapshot.current)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_BASE));
        }
        const auto found = data_->read_nodes.find(id);
        if (found == data_->read_nodes.end())
        {
            return failure(EFlowSourceError::INVALID_IDENTITY);
        }
        const auto &entry = *found->second;
        const bool function = entry.operation() == ENodeOperation::FUNC_DEF_START;
        const bool event = entry.operation() == ENodeOperation::ON_EVENT;
        const bool invalid_name = name.empty() || name.size() > FlowSourceLimits{}.max_string_bytes ||
                                  name.find('\0') != std::string_view::npos;
        if ((!function && !event) || invalid_name || (event && !signature.results.empty()))
        {
            return failure(EFlowSourceError::INVALID_VALUE);
        }
        auto args = materializeFlowArguments(signature.arguments, data_->environment);
        auto results = materializeFlowArguments(signature.results, data_->environment);
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
            return data_->editGraph({}, {}, "Edit function signature");
        }

        std::vector<std::unique_ptr<Node>> replacements;
        if (function)
        {
            replacements.push_back(
                std::make_unique<FuncDefNode>(id.value, name, std::move(*args), std::move(*results)));
            const auto &definition = static_cast<const FuncDefNode &>(*replacements.front());
            for (const auto &storage : data_->source.graph.nodes())
            {
                const auto &node = *storage.node;
                if (node.operation() == ENodeOperation::FUNC_RETURN &&
                    static_cast<const FuncReturnNode &>(node).def() == &entry)
                {
                    replacements.push_back(std::make_unique<FuncReturnNode>(node.id().value, definition));
                }
                else if (node.operation() == ENodeOperation::GRAPH_FUNC_CALL &&
                         static_cast<const GraphFuncCallNode &>(node).callee() == &entry)
                {
                    replacements.push_back(std::make_unique<GraphFuncCallNode>(node.id().value, definition));
                }
            }
        }
        else
        {
            replacements.push_back(std::make_unique<OnEventNode>(id.value, name, std::move(*args)));
        }
        Data::GraphDelta before, after;
        before.restore = after.restore = true;
        std::size_t bytes{};
        for (const auto &replacement : replacements)
        {
            const auto &old = *data_->read_nodes.at(replacement->id());
            if (replacement->id() != id)
            {
                replacement->setName(old.name());
            }
            // Argument/result positions are stable identities. Removing a linked position is rejected below.
            for (const bool input : {true, false})
            {
                const auto &prior = input ? old.inPins() : old.outPins();
                const auto &next = input ? replacement->inPins() : replacement->outPins();
                for (std::size_t index{}; index < next.size(); ++index)
                {
                    auto *pin = next[index];
                    if (index < prior.size() && !FlowGraph::assignDetachedPinId(*pin, prior[index]->id()))
                    {
                        return failure(EFlowSourceError::INVALID_IDENTITY);
                    }
                    if (input && index < prior.size() && pin->kind() == EPinKind::DATA_IN)
                    {
                        const auto &old_input = static_cast<const DataInPin &>(*prior[index]);
                        auto &new_input = static_cast<DataInPin &>(*pin);
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
        for (const auto &link : links())
        {
            const auto from = data_->source.graph.topology().findPin(link.from)->owner;
            const auto to = data_->source.graph.topology().findPin(link.to)->owner;
            if (std::ranges::find(before.erase, from) != before.erase.end() ||
                std::ranges::find(before.erase, to) != before.erase.end())
            {
                before.connect.push_back(link);
                after.connect.push_back(link);
            }
        }
        editing::EditOperationPtr edit =
            std::make_unique<Data::GraphEdit>(*data_, std::move(before), std::move(after), "Edit function signature",
                                              bytes, nullptr, std::move(replacements));
        BusyGuard guard(data_->busy);
        return data_->history->execute(edit);
    }

    std::span<const lux::flowforge::ExportMethodNode> FlowForgeEditor::exports() const noexcept
    {
        return data_->source.graph.exports();
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::setExports(
        std::vector<lux::flowforge::ExportMethodNode> exports)
    {
        if (auto allowed = data_->canEdit(); !allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        const auto limits = lux::flowforge::FlowSourceLimits{};
        if (exports.size() > limits.max_exports)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        std::unordered_set<std::uint64_t> identities, symbols;
        for (const auto &value : exports)
        {
            const auto found = data_->read_nodes.find(value.entry_node_id);
            const bool invalid_entry = found == data_->read_nodes.end() ||
                                       found->second->operation() != lux::flowforge::ENodeOperation::ON_EVENT;
            if (invalid_entry || !value.id.value || !value.symbol || !identities.insert(value.id.value).second ||
                !symbols.insert(value.symbol).second || value.binding_hints.size() > limits.max_exports)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
            }
            for (const auto &hint : value.binding_hints)
            {
                if (hint.kind > lux::script::EScriptBindingHintKind::EVENT || hint.qualified_name.empty() ||
                    hint.qualified_name.size() > limits.max_string_bytes ||
                    hint.qualified_name.find('\0') != std::string::npos)
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
                }
            }
        }
        return data_->change(Data::ExportsAccess{}, std::move(exports), "Set FlowForge exports");
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::removeNodes(
        std::span<const lux::flowforge::NodeId> nodes, std::span<const lux::graph::LinkRecord> links)
    {
        Data::GraphDelta before, after;
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
            const auto *node = data_->source.graph.findNodeById(id);
            if (!node)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
            }
            bytes += nodeStorageBytes(*node);
            if (const auto *layout = data_->source.graph.layout().find(id))
            {
                before.place.push_back({id, *layout});
            }
        }
        for (const auto &link : data_->source.graph.topology().links())
        {
            const auto &topology = data_->source.graph.topology();
            const auto from = topology.findPin(link.from)->owner;
            const auto to = topology.findPin(link.to)->owner;
            if (std::ranges::binary_search(after.erase, from) || std::ranges::binary_search(after.erase, to))
            {
                before.connect.push_back(link);
            }
        }
        for (const auto &link : links)
        {
            if (std::ranges::find(before.connect, link) == before.connect.end())
            {
                before.connect.push_back(link);
                after.disconnect.push_back(link);
            }
        }
        return data_->editGraph(std::move(before), std::move(after), "Remove FlowForge nodes", bytes);
    }
    editing::EditResult<editing::ApplyResult> FlowForgeEditor::connect(lux::flowforge::PinId from,
                                                                       lux::flowforge::PinId to)
    {
        Data::GraphDelta before, after;
        const auto &topology = data_->source.graph.topology();
        if (!topology.findLink(from, to))
        {
            for (const auto &link : topology.links())
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
        return data_->editGraph(std::move(before), std::move(after), "Connect pins");
    }
    editing::EditResult<editing::ApplyResult> FlowForgeEditor::disconnect(lux::flowforge::PinId from,
                                                                          lux::flowforge::PinId to)
    {
        Data::GraphDelta before, after;
        after.disconnect.push_back({from, to});
        before.connect.push_back({from, to});
        return data_->editGraph(std::move(before), std::move(after), "Disconnect pins");
    }
    editing::EditResult<editing::ApplyResult> FlowForgeEditor::moveNodes(
        std::span<const lux::graph::GraphLayoutEntry> entries)
    {
        Data::GraphDelta before, after;
        for (const auto &entry : entries)
        {
            const auto *current = data_->source.graph.layout().find(entry.node);
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
        return data_->editGraph(std::move(before), std::move(after), "Move nodes");
    }

    std::span<const lux::flowforge::FlowGraph::GraphVariable> FlowForgeEditor::variables() const noexcept
    {
        return data_->source.graph.variables();
    }

    editing::EditResult<std::uint64_t> FlowForgeEditor::addVariable(std::string_view name, std::string_view type,
                                                                    const lux::flowforge::FlowSourceLiteral &initial)
    {
        if (auto allowed = data_->canEdit(); !allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        const auto id = data_->source.graph.nextVariableId();
        if (id == UINT64_MAX || variables().size() >= lux::flowforge::FlowSourceLimits{}.max_variables)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::ID_EXHAUSTED));
        }
        if (std::ranges::find(variables(), name, &lux::flowforge::FlowGraph::GraphVariable::name) != variables().end())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        auto variable = lux::flowforge::materializeFlowVariable({id, std::string(name), std::string(type), initial},
                                                                data_->environment);
        if (!variable)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED,
                                                                 static_cast<std::uint64_t>(variable.error().code)));
        }
        auto captured = lux::flowforge::captureFlowVariable(*variable);
        if (!captured)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        editing::EditOperationPtr edit = std::make_unique<Data::VariableEdit>(
            *data_, Data::VariableEdit::Membership{std::move(*captured), variables().size(), true});
        BusyGuard guard(data_->busy);
        auto applied = data_->history->execute(edit);
        if (!applied)
        {
            return lux::cxx::unexpected(applied.error());
        }
        return id;
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::setVariable(
        const lux::flowforge::FlowSourceVariable &value)
    {
        if (auto allowed = data_->canEdit(); !allowed)
        {
            return lux::cxx::unexpected(allowed.error());
        }
        const auto found = std::ranges::find(variables(), value.id, &lux::flowforge::FlowGraph::GraphVariable::id);
        const bool duplicate_name = std::ranges::any_of(
            variables(), [&](const auto &variable) { return variable.id != value.id && variable.name == value.name; });
        if (found == variables().end() || duplicate_name)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        auto before = lux::flowforge::captureFlowVariable(*found);
        auto variable = lux::flowforge::materializeFlowVariable(value, data_->environment);
        if (!before || !variable)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        auto after = lux::flowforge::captureFlowVariable(*variable);
        if (!after)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        editing::EditOperationPtr edit = std::make_unique<Data::VariableEdit>(
            *data_, Data::VariableEdit::Replacement{std::move(*before), std::move(*after),
                                                    static_cast<std::size_t>(found - variables().begin())});
        BusyGuard guard(data_->busy);
        return data_->history->execute(edit);
    }

    editing::EditResult<editing::ApplyResult> FlowForgeEditor::removeVariable(std::uint64_t id)
    {
        if (auto allowed = data_->canEdit(); !allowed)
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
        editing::EditOperationPtr edit = std::make_unique<Data::VariableEdit>(
            *data_, Data::VariableEdit::Membership{std::move(*before),
                                                   static_cast<std::size_t>(found - variables().begin()), false});
        BusyGuard guard(data_->busy);
        return data_->history->execute(edit);
    }

    EditorResult<SaveRequestId> FlowForgeEditor::requestSave(std::string origin)
    {
        if (!data_->project.writable())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "flowforge.save"});
        }
        if (origin.empty() || !data_->runtime.blocking())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "flowforge.save"});
        }
        if (data_->close != ECloseState::OPEN || data_->close_requested || data_->busy || data_->save.index() != 0)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.save"});
        }
        if (data_->next_save == UINT64_MAX)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "flowforge.save"});
        }
        auto capture = this->capture();
        if (!capture)
        {
            return lux::cxx::unexpected(capture.error());
        }
        auto ticket = data_->history->beginSave();
        if (!ticket)
        {
            return lux::cxx::unexpected(historyFailure(ticket.error()));
        }
        const SaveRequestId id{handle(), data_->next_save++};
        data_->save.emplace<FlowSave>(id, *ticket, data_->history->view()->snapshot.revision, data_->source.id,
                                      std::move(*capture), data_->project, data_->runtime, *data_->history);
        return id;
    }
    std::span<const SaveRequestId> FlowForgeEditor::saveRequests() const noexcept
    {
        const auto *save = std::get_if<FlowSave>(&data_->save);
        return save ? save->requests() : std::span<const SaveRequestId>{};
    }
    EditorResult<SaveRequestStatus> FlowForgeEditor::saveStatus(SaveRequestId id) const
    {
        const auto *save = std::get_if<FlowSave>(&data_->save);
        if (!save || save->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "flowforge.save"});
        }
        return save->status();
    }
    EditorResult<void> FlowForgeEditor::retrySave(SaveRequestId id)
    {
        if (data_->busy)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "document.save.retry"});
        }

        auto *save = std::get_if<FlowSave>(&data_->save);
        if (!save || save->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "flowforge.save"});
        }
        return save->retry(!data_->close_requested && data_->close == ECloseState::OPEN);
    }
    EditorResult<void> FlowForgeEditor::abandonSave(SaveRequestId id)
    {
        if (data_->busy)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "document.save.abandonSave"});
        }

        auto *save = std::get_if<FlowSave>(&data_->save);
        if (!save || save->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "flowforge.save"});
        }
        save->abandon();
        return {};
    }
    EditorResult<void> FlowForgeEditor::acknowledgeSave(SaveRequestId id)
    {
        if (data_->busy)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "document.save.acknowledgeSave"});
        }

        auto *save = std::get_if<FlowSave>(&data_->save);
        if (!save || save->id() != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "flowforge.save"});
        }
        if (!save->terminal())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.save"});
        }
        data_->save.emplace<std::monostate>();
        return {};
    }

    EditorResult<FlowCompileId> FlowForgeEditor::requestCompile(std::filesystem::path linker)
    {
        if (!data_->runtime.blocking() || data_->close != ECloseState::OPEN || data_->close_requested || data_->busy ||
            data_->compilation.index() != 0)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.compile"});
        }
        if (data_->next_compile == UINT64_MAX)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "flowforge.compile"});
        }
        auto view = data_->history->view();
        if (!view)
        {
            return lux::cxx::unexpected(historyFailure(view.error()));
        }
        auto capture = this->capture();
        if (!capture)
        {
            return lux::cxx::unexpected(capture.error());
        }
        const FlowCompileId id{handle(), data_->next_compile++};
        data_->compilation.emplace<FlowCompilation>(id, view->snapshot, std::move(*capture), data_->environment,
                                                    data_->runtime, std::move(linker),
                                                    std::string(data_->project.assetName(data_->source.id)));
        return id;
    }
    EditorResult<SaveRequestId> FlowForgeEditor::requestPublish(FlowCompileId compile, std::string origin)
    {
        auto result = compiled(compile);
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        if (!data_->project.writable())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::READ_ONLY, "flowforge.publish"});
        }
        if (origin.empty() || !data_->runtime.blocking())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "flowforge.publish"});
        }
        if (data_->close != ECloseState::OPEN || data_->close_requested || data_->busy || data_->save.index() != 0)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.publish"});
        }
        if (data_->next_save == UINT64_MAX)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "flowforge.publish"});
        }
        auto ticket = data_->history->beginSave();
        if (!ticket)
        {
            return lux::cxx::unexpected(historyFailure(ticket.error()));
        }
        const SaveRequestId id{handle(), data_->next_save++};
        const auto &job = std::get<FlowCompilation>(data_->compilation);
        const auto &image = std::get<FlowCompiled>(job.output).image;
        data_->save.emplace<FlowSave>(id, *ticket, job.revision, data_->source.id, image, data_->project,
                                      data_->runtime, *data_->history);
        return id;
    }

    EditorResult<FlowCompileStatus> FlowForgeEditor::compileStatus(FlowCompileId id) const
    {
        const auto *job = std::get_if<FlowCompilation>(&data_->compilation);
        if (!job || job->id != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "flowforge.compile"});
        }
        if (const auto *success = std::get_if<FlowCompileSucceeded>(&job->status))
        {
            auto value = *success;
            value.current = data_->history->view()->snapshot.current == value.captured;
            return FlowCompileStatus{value};
        }
        return job->status;
    }
    EditorResult<std::reference_wrapper<const lux::script::ScriptArtifact>> FlowForgeEditor::compiled(
        FlowCompileId id) const
    {
        auto status = compileStatus(id);
        if (!status)
        {
            return lux::cxx::unexpected(status.error());
        }
        if (const auto *failed = std::get_if<FlowCompileFailed>(&*status))
        {
            return lux::cxx::unexpected(failed->failure);
        }
        const auto *success = std::get_if<FlowCompileSucceeded>(&*status);
        if (!success || !success->current)
        {
            return lux::cxx::unexpected(
                EditorFailure{success ? EEditorError::STALE_REQUEST : EEditorError::BUSY, "flowforge.compile"});
        }
        return std::cref(std::get<FlowCompiled>(std::get<FlowCompilation>(data_->compilation).output).artifact->data());
    }
    EditorResult<void> FlowForgeEditor::acknowledgeCompile(FlowCompileId id)
    {
        if (data_->busy)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flow.compile"});
        }
        auto *job = std::get_if<FlowCompilation>(&data_->compilation);
        if (!job || job->id != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "flowforge.compile"});
        }
        if (std::holds_alternative<FlowCompilePending>(job->status))
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.compile"});
        }
        data_->compilation.emplace<std::monostate>();
        return {};
    }

    EditorResult<void> FlowForgeEditor::retryLink(FlowCompileId id, std::filesystem::path linker)
    {
        auto *job = std::get_if<FlowCompilation>(&data_->compilation);
        if (!job || job->id != id)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "flowforge.link.retry"});
        }
        const auto *failure = std::get_if<FlowCompileFailed>(&job->status);
        if (!failure || !failure->retryable || data_->close_requested || data_->busy)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "flowforge.link.retry"});
        }
        job->linker = std::move(linker);
        job->startLink();
        return {};
    }

    EditorResult<void> FlowForgeEditor::addViews(std::vector<std::unique_ptr<DocumentView>> &views)
    {
        if (data_->close != ECloseState::OPEN || data_->close_requested || data_->busy)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::CLOSING, "flowforge.views"});
        }
        for (const auto &view : views)
        {
            if (!view || view->closeStatus().state != ECloseState::OPEN)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "flowforge.views"});
            }
        }
        data_->views.reserve(data_->views.size() + views.size());
        for (auto &view : views)
        {
            data_->views.push_back(std::move(view));
        }
        views.clear();
        return {};
    }
    std::span<const std::unique_ptr<DocumentView>> FlowForgeEditor::views() const noexcept
    {
        return data_->views;
    }
    editing::HistoryId FlowForgeEditor::historyId() const noexcept
    {
        return data_->history->id();
    }
    editing::EditResult<editing::HistoryTargetView> FlowForgeEditor::historyView() const noexcept
    {
        auto value = data_->history->view();
        if (!value)
        {
            return lux::cxx::unexpected(value.error());
        }
        using Availability = editing::EHistoryActionAvailability;
        if (data_->close != ECloseState::OPEN || data_->close_requested)
        {
            return editing::HistoryTargetView{value->snapshot, Availability::CLOSED, Availability::CLOSED, {}, {}};
        }
        if (data_->busy)
        {
            return editing::HistoryTargetView{value->snapshot, Availability::BUSY, Availability::BUSY, {}, {}};
        }
        return editing::HistoryTargetView{value->snapshot, value->can_undo ? Availability::READY : Availability::EMPTY,
                                          value->can_redo ? Availability::READY : Availability::EMPTY,
                                          value->undo_label, value->redo_label};
    }
    editing::EditResult<editing::HistoryTargetResult> FlowForgeEditor::undo() noexcept
    {
        if (data_->close != ECloseState::OPEN || data_->close_requested || data_->busy)
        {
            return lux::cxx::unexpected(
                editing::makeEditFailure(data_->busy ? editing::EEditError::BUSY : editing::EEditError::CLOSED));
        }
        BusyGuard guard(data_->busy);
        auto result = data_->history->undo();
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        return editing::HistoryTargetResult{editing::EHistoryTargetOutcome::CONTENT_APPLIED, *result};
    }
    editing::EditResult<editing::HistoryTargetResult> FlowForgeEditor::redo() noexcept
    {
        if (data_->close != ECloseState::OPEN || data_->close_requested || data_->busy)
        {
            return lux::cxx::unexpected(
                editing::makeEditFailure(data_->busy ? editing::EEditError::BUSY : editing::EEditError::CLOSED));
        }
        BusyGuard guard(data_->busy);
        auto result = data_->history->redo();
        if (!result)
        {
            return lux::cxx::unexpected(result.error());
        }
        return editing::HistoryTargetResult{editing::EHistoryTargetOutcome::CONTENT_APPLIED, *result};
    }
    void FlowForgeEditor::requestClose() noexcept
    {
        data_->close_requested = true;
    }

    void FlowForgeEditor::beginClose() noexcept
    {
        if (data_->busy || data_->close != ECloseState::OPEN)
        {
            return;
        }
        if (auto *save = std::get_if<FlowSave>(&data_->save); save && !save->terminal())
        {
            save->abandon();
        }
        if (auto *job = std::get_if<FlowCompilation>(&data_->compilation))
        {
            job->stop.request_stop();
        }
        for (const auto &view : data_->views)
        {
            view->requestClose();
        }
        data_->close = ECloseState::CLOSING;
    }
    CloseStatus FlowForgeEditor::closeStatus() const
    {
        if (data_->close_requested && data_->close != ECloseState::CLOSED)
        {
            if (const auto *save = std::get_if<FlowSave>(&data_->save))
            {
                if (const auto *failed = std::get_if<SaveRetryable>(&save->status()))
                {
                    return {ECloseState::CLOSING, "Resolve the retained FlowForge save",
                            lux::cxx::unexpected(failed->failure)};
                }
            }
            return {ECloseState::CLOSING, "FlowForge views, save and compilation"};
        }
        return {data_->close, {}};
    }
    void FlowForgeEditor::poll(PollBudget &budget)
    {
        if (data_->busy)
        {
            return;
        }
        if (auto *save = std::get_if<FlowSave>(&data_->save))
        {
            save->poll();
        }
        if (auto *job = std::get_if<FlowCompilation>(&data_->compilation); job && job->poll())
        {
            const auto completed_id = job->id;
            BusyGuard guard(data_->busy);
            notify<compileFinished>(completed_id);
        }
        if (data_->close_requested)
        {
            beginClose();
        }
        for (const auto &view : data_->views)
        {
            view->poll(budget);
        }
        std::erase_if(data_->views, [](const auto &view) { return view->closeStatus().state == ECloseState::CLOSED; });
        if (data_->close != ECloseState::CLOSING || !data_->views.empty())
        {
            return;
        }
        if (const auto *save = std::get_if<FlowSave>(&data_->save); save && !save->terminal())
        {
            return;
        }
        if (const auto *job = std::get_if<FlowCompilation>(&data_->compilation); job && !job->settled())
        {
            return;
        }
        data_->compilation.emplace<std::monostate>();
        data_->save.emplace<std::monostate>();
        if (data_->history->close())
        {
            data_->close = ECloseState::CLOSED;
        }
    }
    struct FlowCodec final
    {
        using Source = Content;
        lux::flowforge::FlowSourceEnvironment environment;
        static constexpr std::size_t max_bytes = 16U * 1024U * 1024U;
        static lux::asset::AssetId identity(const Source &source) noexcept
        {
            return source.id;
        }
        EditorResult<Source> decode(const lux::cxx::SharedBytes<> &bytes, std::stop_token) const noexcept
        {
            auto source =
                lux::flowforge::decodeFlowSource({reinterpret_cast<const char *>(bytes.data()), bytes.size()});
            if (!source)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "flowforge.decode",
                                                          static_cast<std::uint64_t>(source.error().code),
                                                          source.error().field, source.error()});
            }
            auto graph = lux::flowforge::materializeFlowSource(*source, environment);
            if (!graph)
            {
                return lux::cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "flowforge.materialize",
                                                          static_cast<std::uint64_t>(graph.error().code),
                                                          graph.error().field, graph.error()});
            }
            return Content{source->id, std::move(source->name), std::move(*graph)};
        }
        EditorResult<std::unique_ptr<DocumentEditor>> adopt(Source &source, Project &project,
                                                            process::ExecutionRuntime &runtime) const
        {
            auto result = FlowForgeEditor::adopt(source.id, source.name, source.graph, project, runtime, environment);
            if (!result)
            {
                return lux::cxx::unexpected(result.error());
            }
            return std::unique_ptr<DocumentEditor>(std::move(*result));
        }
    };

    EditorResult<std::unique_ptr<DocumentOpening>> openFlowForgeDocument(
        Project &project, const OpenDocumentRequest &request, process::ExecutionRuntime &runtime,
        lux::flowforge::FlowSourceEnvironment environment)
    {
        const auto *entry = project.asset(request.key.source);
        if (!entry || entry->kind != EProjectAssetKind::FLOW_GRAPH || !runtime.blocking() ||
            request.key.project != project.manifest().id || request.key.type != kFlowForgeDocumentType)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "flowforge.open"});
        }
        if (auto valid = lux::flowforge::validateFlowSourceEnvironment(environment); !valid)
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "flowforge.metadata",
                                                      static_cast<std::uint64_t>(valid.error().code),
                                                      valid.error().field, valid.error()});
        }
        return std::unique_ptr<DocumentOpening>(
            new detail::SourceOpening<FlowCodec>(project, *entry, runtime, FlowCodec{std::move(environment)}));
    }
} // namespace lux::editor::flowforge
