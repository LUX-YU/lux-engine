#include "FlowEditPreparation.hpp"
#include <lux/engine/flowforge/graph/ArithmeticNode.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/ObjectNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>
#include <lux/engine/flowforge/script/ScriptEventAwaitNode.hpp>
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace lux::editor::flowforge::detail
{
    bool registeredMetadata(const lux::flowforge::Node& node, const lux::flowforge::FlowSourceEnvironment& environment_)
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
                return &type->type == call.ownerType() && std::ranges::any_of(type->methods, [&](const auto& method) {
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
                   std::ranges::any_of(type->fields, [&](const auto& registered) { return &registered == field; }) &&
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
        constexpr auto object_bytes = std::max(
            {sizeof(StartNode),         sizeof(BranchNode),          sizeof(SequenceNode),    sizeof(ForLoopNode),
             sizeof(WhileLoopNode),     sizeof(ReturnNode),          sizeof(BreakNode),       sizeof(BinaryOpNode),
             sizeof(UnaryOpNode),       sizeof(FuncDefNode),         sizeof(FuncReturnNode),  sizeof(OnEventNode),
             sizeof(GraphFuncCallNode), sizeof(NativeFuncCall),      sizeof(GetObjectNode),   sizeof(SetObjectNode),
             sizeof(GetFieldNode),      sizeof(SetFieldNode),        sizeof(GetVariableNode), sizeof(SetVariableNode),
             sizeof(ScriptAbilityNode), sizeof(ScriptEventAwaitNode)}
        );
        std::size_t bytes =
            object_bytes + sizeof(std::unique_ptr<Node>) + node.name().capacity() + node.creatorName().capacity() + 2;
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
    struct NameAccess final
    {
        using Value = std::string;
        bool exists(const FlowAuthoringSource&) const noexcept
        {
            return true;
        }
        Value read(const FlowAuthoringSource& source_) const
        {
            return source_.name;
        }
        void exchange(FlowAuthoringSource& source_, Value& value) const noexcept
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

    struct FlowEditContext final
    {
        FlowAuthoringSource& source_;
        lux::flowforge::FlowSourceEnvironment environment_;
        contracts::CodeLease code_;
        editing::StateId base_;
        FlowEditObserver observer_;
        void publish(const editing::CommitInfo& info, bool structural = false) const noexcept
        {
            if (observer_.changed)
                observer_.changed(observer_.owner, info, structural);
        }
    };
    class FlowOperation : public editing::EditOperation
    {
    public:
        mutable FlowEditIds ids_;
    };
    class FlowPlan : public editing::PreparedEdit
    {
    public:
        // Used only on a private candidate. History alone applies a live graph.
        void commitCandidate() noexcept
        {
            applyContent();
        }

    private:
        void apply() noexcept final
        {
            applyContent();
        }
        virtual void applyContent() noexcept = 0;
    };
    PreparedFlowEdit prepared(editing::EditOperationPtr operation)
    {
        const auto* ids = &static_cast<const FlowOperation&>(*operation).ids_;
        return {std::move(operation), ids};
    }
    bool variableReferenced(const lux::flowforge::FlowGraph& graph, std::uint64_t id) noexcept
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
    class VariableEdit final : public FlowOperation
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
        class Plan final : public FlowPlan
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
            void applyContent() noexcept override
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
                edit_.owner_.publish(info);
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
        VariableEdit(FlowEditContext owner, VMutation mutation)
            : owner_(owner), mutation_(std::move(mutation)), base_(owner.base_)
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
        FlowEditContext owner_;
        VMutation mutation_;
        editing::StateId base_;
    };

    class LiteralEdit final : public FlowOperation
    {
        class Plan final : public FlowPlan
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
            void applyContent() noexcept override
            {
                swap(pin_.constantData(), value_);
            }
            void publish(const editing::CommitInfo& info) noexcept override
            {
                edit_.owner_.publish(info);
            }
            const LiteralEdit& edit_;
            lux::flowforge::DataInPin& pin_;
            lux::meta::RuntimeObject value_;
        };

    public:
        LiteralEdit(
            FlowEditContext owner,
            lux::flowforge::PinId pin,
            lux::flowforge::FlowSourceLiteral before,
            lux::flowforge::FlowSourceLiteral after
        )
            : owner_(owner), pin_(pin), before_(std::move(before)), after_(std::move(after)), base_(owner.base_)
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
        FlowEditContext owner_;
        lux::flowforge::PinId pin_;
        lux::flowforge::FlowSourceLiteral before_, after_;
        editing::StateId base_;
    };

    template <class Access> class TValueEdit final : public FlowOperation
    {
        using Value = typename Access::Value;
        class Plan final : public FlowPlan
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
            void applyContent() noexcept override
            {
                edit_.access_.exchange(edit_.owner_.source_, value_);
            }
            void publish(const editing::CommitInfo& info) noexcept override
            {
                edit_.owner_.publish(info);
            }
            const TValueEdit& edit_;
            Value value_;
            bool changed_;
        };

    public:
        TValueEdit(FlowEditContext owner, Access access, Value after, std::string label, editing::StateId base)
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
        FlowEditContext owner_;
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
        bool exists(const FlowAuthoringSource&) const noexcept
        {
            return true;
        }
        Value read(const FlowAuthoringSource& source_) const
        {
            return source_.graph.exports();
        }
        void exchange(FlowAuthoringSource& source_, Value& value) const noexcept
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

    class GraphEdit final : public FlowOperation
    {
        class Plan final : public FlowPlan
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
            {}
            editing::EEditEffect effect() const noexcept override
            {
                return changed_ ? editing::EEditEffect::CHANGE : editing::EEditEffect::NO_CHANGE;
            }

        private:
            void applyContent() noexcept override
            {
                if (edit_.input_)
                {
                    const auto id = patch_.insertedIds().front();
                    edit_.ids_.nodes.front() = id;
                    edit_.before_.erase.front() = id;
                    edit_.after_.place.front().node = id;
                }
                patch_.commit();
                edit_.parked_ = patch_.takeRemoved();
                edit_.input_ = nullptr;
            }
            void publish(const editing::CommitInfo& info) noexcept override
            {
                edit_.owner_.publish(info, structural_);
            }
            const GraphEdit& edit_;
            lux::flowforge::FlowGraphEdit patch_;
            bool changed_;
            bool structural_;
        };

    public:
        GraphEdit(
            FlowEditContext owner,
            GraphDelta before,
            GraphDelta after,
            std::string label,
            std::size_t node_bytes,
            std::unique_ptr<lux::flowforge::Node>* input = nullptr,
            std::vector<std::unique_ptr<lux::flowforge::Node>> staged = {}
        )
            : owner_(owner), before_(std::move(before)), after_(std::move(after)), label_(std::move(label)),
              base_(owner.base_), node_bytes_(node_bytes), input_(input), parked_(std::move(staged))
        {
            if (input_)
                ids_.nodes.resize(1);
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
        void ownInput()
        {
            initial_ = std::move(*input_);
            input_ = &initial_;
        }
        lux::flowforge::NodeId inserted() const noexcept
        {
            return before_.erase.front();
        }

    private:
        FlowEditContext owner_;
        mutable GraphDelta before_, after_;
        std::string label_;
        editing::StateId base_;
        std::size_t node_bytes_;
        mutable std::unique_ptr<lux::flowforge::Node> initial_;
        mutable std::unique_ptr<lux::flowforge::Node>* input_;
        mutable std::vector<std::unique_ptr<lux::flowforge::Node>> parked_;
    };

    class FlowEditFactory final
    {
    public:
        explicit FlowEditFactory(FlowEditContext context) : context_(std::move(context)) {}
        editing::EditResult<PreparedFlowEdit> setPinLiteral(
            lux::flowforge::PinId id,
            const lux::flowforge::FlowSourceLiteral& literal
        );
        editing::EditResult<PreparedFlowEdit> rename(std::string_view name);
        editing::EditResult<PreparedFlowEdit> insertNode(
            std::unique_ptr<lux::flowforge::Node>& input,
            lux::graph::GraphNodeLayout placement,
            bool own,
            bool preserve_ids
        );
        editing::EditResult<PreparedFlowEdit> insertFunctionUse(
            lux::flowforge::NodeId id,
            bool return_node,
            lux::graph::GraphNodeLayout layout
        );
        editing::EditResult<PreparedFlowEdit> setFunctionSignature(
            editing::StateId base,
            lux::flowforge::NodeId id,
            std::string_view name,
            const lux::flowforge::FlowSourceSignature& signature
        );
        editing::EditResult<PreparedFlowEdit> setExports(std::vector<lux::flowforge::ExportMethodNode> exports);
        editing::EditResult<PreparedFlowEdit> removeNodes(
            std::span<const lux::flowforge::NodeId> nodes,
            std::span<const lux::graph::LinkRecord> links
        );
        editing::EditResult<PreparedFlowEdit> connect(lux::flowforge::PinId from, lux::flowforge::PinId to);
        editing::EditResult<PreparedFlowEdit> disconnect(lux::flowforge::PinId from, lux::flowforge::PinId to);
        editing::EditResult<PreparedFlowEdit> moveNodes(std::span<const lux::graph::GraphLayoutEntry> entries);
        editing::EditResult<PreparedFlowEdit> addVariable(
            std::string_view name,
            std::string_view type,
            const lux::flowforge::FlowSourceLiteral& initial
        );
        editing::EditResult<PreparedFlowEdit> setVariable(const lux::flowforge::FlowSourceVariable& value);
        editing::EditResult<PreparedFlowEdit> removeVariable(std::uint64_t id);
        editing::EditResult<PreparedFlowEdit> make(VFlowEdit& edit);

    private:
        template <class Access>
        editing::EditResult<PreparedFlowEdit> change(Access access, typename Access::Value value, std::string label)
        {
            if (!access.exists(context_.source_))
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
            return prepared(std::make_unique<
                            TValueEdit<Access>>(context_, access, std::move(value), std::move(label), context_.base_));
        }
        editing::EditResult<PreparedFlowEdit> editGraph(
            GraphDelta before,
            GraphDelta after,
            std::string label,
            std::size_t bytes = 0
        )
        {
            return prepared(
                std::make_unique<GraphEdit>(context_, std::move(before), std::move(after), std::move(label), bytes)
            );
        }
        auto variables() const noexcept
        {
            return std::span{context_.source_.graph.variables()};
        }
        auto links() const noexcept
        {
            return context_.source_.graph.topology().links();
        }
        lux::graph::GraphNodeLayout nodeLayout(lux::flowforge::NodeId id) const noexcept
        {
            const auto* layout = context_.source_.graph.layout().find(id);
            return layout ? *layout : lux::graph::GraphNodeLayout{};
        }
        FlowEditContext context_;
    };
    editing::EditResult<PreparedFlowEdit> FlowEditFactory::setPinLiteral(
        lux::flowforge::PinId id,
        const lux::flowforge::FlowSourceLiteral& literal
    )
    {
        const auto* found = context_.source_.graph.findPin(id);
        if (!found || found->kind() != lux::flowforge::EPinKind::DATA_IN)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        const auto& pin = *static_cast<const lux::flowforge::DataInPin*>(found);
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
            std::make_unique<LiteralEdit>(context_, id, std::move(*before), std::move(*after));
        return prepared(std::move(operation));
    }
    editing::EditResult<PreparedFlowEdit> FlowEditFactory::rename(std::string_view name)
    {
        if (name.empty() || name.size() > 4096)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        lux::flowforge::FlowSource candidate{context_.source_.id, std::string(name), {}};
        const auto valid = lux::flowforge::validateFlowSource(candidate);
        if (!valid)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::INVALID_ARGUMENT,
                static_cast<std::uint64_t>(valid.error().code)
            ));
        }
        return change(NameAccess{}, std::move(candidate.name), "Rename FlowForge");
    }
    editing::EditResult<PreparedFlowEdit> FlowEditFactory::insertNode(
        std::unique_ptr<lux::flowforge::Node>& input,
        lux::graph::GraphNodeLayout placement,
        bool own,
        bool preserve_ids
    )
    {
        const bool is_invalid_input =
            !input || input->graph() || !std::isfinite(placement.x) || !std::isfinite(placement.y);
        if (is_invalid_input)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        if (!registeredMetadata(*input, context_.environment_))
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
        if (preserve_ids)
        {
            const auto id = input->id();
            GraphDelta before, after;
            before.erase.push_back(id);
            after.restore = true;
            after.place.push_back({id, placement});
            const auto bytes = nodeStorageBytes(*input);
            std::vector<std::unique_ptr<lux::flowforge::Node>> nodes;
            nodes.push_back(std::move(input));
            auto operation = std::make_unique<GraphEdit>(
                context_,
                std::move(before),
                std::move(after),
                "Restore FlowForge node",
                bytes,
                nullptr,
                std::move(nodes)
            );
            operation->ids_.nodes.push_back(id);
            return prepared(std::move(operation));
        }
        GraphDelta before, after;
        before.erase.resize(1);
        after.restore = true;
        after.place.push_back({{}, placement});
        auto command = std::make_unique<GraphEdit>(
            context_,
            std::move(before),
            std::move(after),
            "Insert FlowForge node",
            nodeStorageBytes(*input),
            &input
        );
        if (own)
            command->ownInput();
        return prepared(std::move(command));
    }
    editing::EditResult<PreparedFlowEdit> FlowEditFactory::insertFunctionUse(
        lux::flowforge::NodeId id,
        bool return_node,
        lux::graph::GraphNodeLayout layout
    )
    {
        using namespace lux::flowforge;
        const auto* found = context_.source_.graph.findNodeById(id);
        if (!found || found->operation() != ENodeOperation::FUNC_DEF_START)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        const auto& definition = static_cast<const FuncDefNode&>(*found);
        std::unique_ptr<Node> node;
        if (return_node)
        {
            node = std::make_unique<FuncReturnNode>(0, definition);
        }
        else
        {
            node = std::make_unique<GraphFuncCallNode>(0, definition);
        }
        return insertNode(node, layout, true, false);
    }
    editing::EditResult<PreparedFlowEdit> FlowEditFactory::setFunctionSignature(
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
        if (base != context_.base_)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_BASE));
        }
        const auto* found = context_.source_.graph.findNodeById(id);
        if (!found)
        {
            return failure(EFlowSourceError::INVALID_IDENTITY);
        }
        const auto& entry = *found;
        const bool function = entry.operation() == ENodeOperation::FUNC_DEF_START;
        const bool event = entry.operation() == ENodeOperation::ON_EVENT;
        const bool invalid_name = name.empty() || name.size() > FlowSourceLimits{}.max_string_bytes ||
                                  name.find('\0') != std::string_view::npos;
        const bool is_invalid_signature =
            (!function && !event) || invalid_name || (event && !signature.results.empty());
        if (is_invalid_signature)
        {
            return failure(EFlowSourceError::INVALID_VALUE);
        }
        auto args = materializeFlowArguments(signature.arguments, context_.environment_);
        auto results = materializeFlowArguments(signature.results, context_.environment_);
        if (!args || !results)
        {
            return failure(!args ? args.error().code : results.error().code);
        }
        const auto before_source = captureFlowNode(entry);
        if (!before_source)
        {
            return failure(before_source.error().code);
        }
        const bool is_unchanged_signature =
            entry.name() == name && std::get<FlowSourceSignature>(before_source->parameters) == signature;
        if (is_unchanged_signature)
        {
            return editGraph({}, {}, "Edit function signature");
        }

        std::vector<std::unique_ptr<Node>> replacements;
        if (function)
        {
            replacements.push_back(std::make_unique<FuncDefNode>(id.value, name, std::move(*args), std::move(*results))
            );
            const auto& definition = static_cast<const FuncDefNode&>(*replacements.front());
            for (const auto& storage : context_.source_.graph.nodes())
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
        GraphDelta before, after;
        before.restore = after.restore = true;
        std::size_t bytes{};
        for (const auto& replacement : replacements)
        {
            const auto& old = *context_.source_.graph.findNodeById(replacement->id());
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
            const auto from = context_.source_.graph.topology().findPin(link.from)->owner;
            const auto to = context_.source_.graph.topology().findPin(link.to)->owner;
            const bool has_replaced_endpoint = std::ranges::find(before.erase, from) != before.erase.end() ||
                                               std::ranges::find(before.erase, to) != before.erase.end();
            if (has_replaced_endpoint)
            {
                before.connect.push_back(link);
                after.connect.push_back(link);
            }
        }
        editing::EditOperationPtr edit = std::make_unique<GraphEdit>(
            context_,
            std::move(before),
            std::move(after),
            "Edit function signature",
            bytes,
            nullptr,
            std::move(replacements)
        );
        return prepared(std::move(edit));
    }
    editing::EditResult<PreparedFlowEdit> FlowEditFactory::setExports(
        std::vector<lux::flowforge::ExportMethodNode> exports
    )
    {
        const auto limits = lux::flowforge::FlowSourceLimits{};
        if (exports.size() > limits.max_exports)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        }
        std::unordered_set<std::uint64_t> identities, symbols;
        for (const auto& value : exports)
        {
            const auto* found = context_.source_.graph.findNodeById(value.entry_node_id);
            const bool invalid_entry = !found || found->operation() != lux::flowforge::ENodeOperation::ON_EVENT;
            const bool is_invalid_export =
                invalid_entry || !value.id.value || !value.symbol || !identities.insert(value.id.value).second ||
                !symbols.insert(value.symbol).second || value.binding_hints.size() > limits.max_exports;
            if (is_invalid_export)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
            }
            for (const auto& hint : value.binding_hints)
            {
                const bool is_invalid_hint = hint.kind > lux::script::EScriptBindingHintKind::EVENT ||
                                             hint.qualified_name.empty() ||
                                             hint.qualified_name.size() > limits.max_string_bytes ||
                                             hint.qualified_name.find('\0') != std::string::npos;
                if (is_invalid_hint)
                {
                    return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
                }
            }
        }
        return change(ExportsAccess{}, std::move(exports), "Set FlowForge exports");
    }
    editing::EditResult<PreparedFlowEdit> FlowEditFactory::removeNodes(
        std::span<const lux::flowforge::NodeId> nodes,
        std::span<const lux::graph::LinkRecord> links
    )
    {
        GraphDelta before, after;
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
            const auto* node = context_.source_.graph.findNodeById(id);
            if (!node)
            {
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
            }
            bytes += nodeStorageBytes(*node);
            if (const auto* layout = context_.source_.graph.layout().find(id))
            {
                before.place.push_back({id, *layout});
            }
        }
        for (const auto& link : context_.source_.graph.topology().links())
        {
            const auto& topology = context_.source_.graph.topology();
            const auto from = topology.findPin(link.from)->owner;
            const auto to = topology.findPin(link.to)->owner;
            const bool has_removed_endpoint =
                std::ranges::binary_search(after.erase, from) || std::ranges::binary_search(after.erase, to);
            if (has_removed_endpoint)
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
        return editGraph(std::move(before), std::move(after), "Remove FlowForge nodes", bytes);
    }
    editing::EditResult<PreparedFlowEdit> FlowEditFactory::connect(lux::flowforge::PinId from, lux::flowforge::PinId to)
    {
        GraphDelta before, after;
        const auto& topology = context_.source_.graph.topology();
        if (!topology.findLink(from, to))
        {
            for (const auto& link : topology.links())
            {
                const bool has_displaced_endpoint =
                    (link.to == to && topology.findPin(to) && topology.findPin(to)->fan_cap == 1) ||
                    (link.from == from && topology.findPin(from) && topology.findPin(from)->fan_cap == 1);
                if (has_displaced_endpoint)
                {
                    after.disconnect.push_back(link);
                    before.connect.push_back(link);
                }
            }
            after.connect.push_back({from, to});
            before.disconnect.push_back({from, to});
        }
        return editGraph(std::move(before), std::move(after), "Connect pins");
    }
    editing::EditResult<PreparedFlowEdit> FlowEditFactory::disconnect(
        lux::flowforge::PinId from,
        lux::flowforge::PinId to
    )
    {
        GraphDelta before, after;
        after.disconnect.push_back({from, to});
        before.connect.push_back({from, to});
        return editGraph(std::move(before), std::move(after), "Disconnect pins");
    }
    editing::EditResult<PreparedFlowEdit> FlowEditFactory::moveNodes(
        std::span<const lux::graph::GraphLayoutEntry> entries
    )
    {
        GraphDelta before, after;
        for (const auto& entry : entries)
        {
            const auto* current = context_.source_.graph.layout().find(entry.node);
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
        return editGraph(std::move(before), std::move(after), "Move nodes");
    }
    editing::EditResult<PreparedFlowEdit> FlowEditFactory::addVariable(
        std::string_view name,
        std::string_view type,
        const lux::flowforge::FlowSourceLiteral& initial
    )
    {
        const auto id = context_.source_.graph.nextVariableId();
        const bool is_exhausted =
            id == UINT64_MAX || variables().size() >= lux::flowforge::FlowSourceLimits{}.max_variables;
        if (is_exhausted)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::ID_EXHAUSTED));
        }
        if (std::ranges::find(variables(), name, &lux::flowforge::FlowGraph::GraphVariable::name) != variables().end())
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        auto variable = lux::flowforge::materializeFlowVariable(
            {id, std::string(name), std::string(type), initial},
            context_.environment_
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
        editing::EditOperationPtr edit = std::make_unique<VariableEdit>(
            context_,
            VariableEdit::Membership{std::move(*captured), variables().size(), true}
        );
        static_cast<FlowOperation&>(*edit).ids_.variables.push_back(id);
        return prepared(std::move(edit));
    }
    editing::EditResult<PreparedFlowEdit> FlowEditFactory::setVariable(const lux::flowforge::FlowSourceVariable& value)
    {
        const auto found = std::ranges::find(variables(), value.id, &lux::flowforge::FlowGraph::GraphVariable::id);
        const bool duplicate_name = std::ranges::any_of(variables(), [&](const auto& variable) {
            return variable.id != value.id && variable.name == value.name;
        });
        const bool is_invalid_variable = found == variables().end() || duplicate_name;
        if (is_invalid_variable)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        auto before = lux::flowforge::captureFlowVariable(*found);
        auto variable = lux::flowforge::materializeFlowVariable(value, context_.environment_);
        if (!before || !variable)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        auto after = lux::flowforge::captureFlowVariable(*variable);
        if (!after)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
        }
        editing::EditOperationPtr edit = std::make_unique<VariableEdit>(
            context_,
            VariableEdit::Replacement{
                std::move(*before),
                std::move(*after),
                static_cast<std::size_t>(found - variables().begin())
            }
        );
        return prepared(std::move(edit));
    }
    editing::EditResult<PreparedFlowEdit> FlowEditFactory::removeVariable(std::uint64_t id)
    {
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
        editing::EditOperationPtr edit = std::make_unique<VariableEdit>(
            context_,
            VariableEdit::Membership{std::move(*before), static_cast<std::size_t>(found - variables().begin()), false}
        );
        return prepared(std::move(edit));
    }
    editing::EditResult<PreparedFlowEdit> FlowEditFactory::make(VFlowEdit& edit)
    {
        return std::visit(
            [&](auto& value) -> editing::EditResult<PreparedFlowEdit> {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, FlowRename>)
                    return rename(value.value);
                else if constexpr (std::is_same_v<T, FlowSetLiteral>)
                    return setPinLiteral(value.pin, value.value);
                else if constexpr (std::is_same_v<T, FlowInsertNode>)
                    return insertNode(value.value, value.placement, true, value.preserve_ids);
                else if constexpr (std::is_same_v<T, FlowInsertFunctionUse>)
                    return insertFunctionUse(value.definition, value.return_node, value.placement);
                else if constexpr (std::is_same_v<T, FlowSetSignature>)
                    return setFunctionSignature(context_.base_, value.node, value.name, value.value);
                else if constexpr (std::is_same_v<T, FlowSetExports>)
                    return setExports(value.value);
                else if constexpr (std::is_same_v<T, FlowRemoveNodes>)
                    return removeNodes(value.nodes, value.links);
                else if constexpr (std::is_same_v<T, FlowConnect>)
                    return connect(value.from, value.to);
                else if constexpr (std::is_same_v<T, FlowDisconnect>)
                    return disconnect(value.from, value.to);
                else if constexpr (std::is_same_v<T, FlowMoveNodes>)
                    return moveNodes(value.value);
                else if constexpr (std::is_same_v<T, FlowAddVariable>)
                    return addVariable(value.name, value.type, value.initial);
                else if constexpr (std::is_same_v<T, FlowSetVariable>)
                    return setVariable(value.value);
                else
                    return removeVariable(value.id);
            },
            edit
        );
    }

    std::size_t sourceBytes(const FlowAuthoringSource& source) noexcept
    {
        std::size_t bytes = sizeof(source) + source.name.capacity() + 1;
        for (const auto& storage : source.graph.nodes())
            bytes += nodeStorageBytes(*storage.node);
        for (const auto& variable : source.graph.variables())
            bytes += sizeof(variable) + variable.name.capacity() + 1 + variable.type->size;
        bytes += ExportsAccess::bytes(source.graph.exports());
        bytes += source.graph.topology().nodes().size() * sizeof(lux::graph::NodeRecord);
        bytes += source.graph.topology().pins().size() * sizeof(lux::graph::PinRecord);
        bytes += source.graph.topology().links().size() * sizeof(lux::graph::LinkRecord);
        bytes += source.graph.layout().all().size() * sizeof(lux::graph::GraphLayoutEntry);
        return bytes;
    }

    // A heterogeneous batch uses the same concrete operations against one isolated graph.
    // It does not record a second history, publish intermediate observations, or keep field scratch.
    class FlowBatchEdit final : public FlowOperation
    {
        class Plan final : public FlowPlan
        {
        public:
            Plan(
                const FlowBatchEdit& edit,
                std::unique_ptr<FlowAuthoringSource> candidate,
                FlowEditIds ids,
                bool changed
            )
                : edit_(edit), candidate_(std::move(candidate)), ids_(std::move(ids)), changed_(changed)
            {}
            editing::EEditEffect effect() const noexcept override
            {
                return changed_ ? editing::EEditEffect::CHANGE : editing::EEditEffect::NO_CHANGE;
            }

        private:
            void applyContent() noexcept override
            {
                auto& live = edit_.owner_.source_;
                auto& next = candidate_ ? *candidate_ : *edit_.parked_;
                live.graph.preserveVariableIdsFrom(next.graph);
                next.graph.preserveVariableIdsFrom(live.graph);
                using std::swap;
                swap(live.name, next.name);
                swap(live.graph, next.graph);
                if (candidate_)
                    edit_.parked_ = std::move(candidate_);
                if (!ids_.nodes.empty() || !ids_.variables.empty())
                    swap(edit_.ids_, ids_);
                // Inputs and intermediate graphs are reclaimed while the host still holds admission.
                edit_.edits_.clear();
            }
            void publish(const editing::CommitInfo& info) noexcept override
            {
                edit_.owner_.publish(info, true);
            }
            const FlowBatchEdit& edit_;
            std::unique_ptr<FlowAuthoringSource> candidate_;
            FlowEditIds ids_;
            bool changed_;
        };

    public:
        FlowBatchEdit(FlowEditContext owner, std::vector<VFlowEdit> edits, std::string label)
            : owner_(std::move(owner)), edits_(std::move(edits)), label_(std::move(label)),
              retained_(2 * sourceBytes(owner_.source_) + sizeof(*this) + label_.capacity())
        {
            // Charge input payloads as well as both replay graphs. The prepare budget bounds
            // cumulative intermediate staging; no hidden unlimited candidate allocation.
            for (const auto& edit : edits_)
                retained_ += std::visit(
                    [](const auto& value) -> std::size_t {
                        using T = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<T, FlowInsertNode>)
                            return sizeof(T) + (value.value ? nodeStorageBytes(*value.value) : 0);
                        else if constexpr (std::is_same_v<T, FlowSetSignature>)
                        {
                            std::size_t size = sizeof(T) + value.name.capacity();
                            for (const auto& arg : value.value.arguments)
                                size += sizeof(arg) + arg.name.capacity() + arg.type.capacity();
                            for (const auto& arg : value.value.results)
                                size += sizeof(arg) + arg.name.capacity() + arg.type.capacity();
                            return size;
                        }
                        else if constexpr (std::is_same_v<T, FlowRename>)
                            return sizeof(T) + value.value.capacity();
                        else if constexpr (std::is_same_v<T, FlowSetLiteral>)
                            return sizeof(T) + value.value.value.capacity();
                        else if constexpr (std::is_same_v<T, FlowSetExports>)
                            return sizeof(T) + ExportsAccess::bytes(value.value);
                        else if constexpr (std::is_same_v<T, FlowAddVariable>)
                            return sizeof(T) + value.name.capacity() + value.type.capacity() +
                                   value.initial.value.capacity();
                        else if constexpr (std::is_same_v<T, FlowSetVariable>)
                            return sizeof(T) + value.value.name.capacity() + value.value.type.capacity() +
                                   value.value.value.value.capacity();
                        else if constexpr (std::is_same_v<T, FlowMoveNodes>)
                            return sizeof(T) + value.value.capacity() * sizeof(lux::graph::GraphLayoutEntry);
                        else if constexpr (std::is_same_v<T, FlowRemoveNodes>)
                            return sizeof(T) + value.nodes.capacity() * sizeof(lux::flowforge::NodeId) +
                                   value.links.capacity() * sizeof(lux::graph::LinkRecord);
                        else
                            return sizeof(T);
                    },
                    edit
                );
            // Function-use intents expand signatures into new pins. Reserve an additional source
            // image per intent instead of counting their small descriptor alone. The actual candidate
            // is checked below before commit; oversized expansions fail without touching the source.
            retained_ += edits_.size() * sourceBytes(owner_.source_);
        }
        editing::HistoryId historyId() const noexcept override
        {
            return owner_.base_.history;
        }
        editing::StateId baseState() const noexcept override
        {
            return owner_.base_;
        }
        std::string_view label() const noexcept override
        {
            return label_;
        }
        std::size_t retainedBytesUpperBound() const noexcept override
        {
            return retained_;
        }
        editing::EditResult<editing::PreparedEditPtr> prepare(
            const editing::ApplyContext& context,
            editing::EditPreparationBudget& budget
        ) const noexcept override
        {
            if (parked_)
                return editing::PreparedEditPtr(new Plan(*this, {}, {}, true));
            if (auto charge = budget.reserve(retained_); !charge)
                return lux::cxx::unexpected(charge.error());
            auto frozen =
                lux::flowforge::captureFlowSource(owner_.source_.id, owner_.source_.name, owner_.source_.graph);
            if (!frozen)
                return lux::cxx::unexpected(editing::makeEditFailure(
                    editing::EEditError::PRECONDITION_FAILED,
                    static_cast<std::uint64_t>(frozen.error().code)
                ));
            auto graph = lux::flowforge::materializeFlowSource(*frozen, owner_.environment_);
            if (!graph)
                return lux::cxx::unexpected(editing::makeEditFailure(
                    editing::EEditError::PRECONDITION_FAILED,
                    static_cast<std::uint64_t>(graph.error().code)
                ));
            auto candidate =
                std::make_unique<FlowAuthoringSource>(FlowAuthoringSource{frozen->id, frozen->name, std::move(*graph)});
            candidate->graph.preserveVariableIdsFrom(owner_.source_.graph);
            FlowEditFactory factory{{*candidate, owner_.environment_, owner_.code_, owner_.base_, {}}};
            FlowEditIds ids;
            for (auto& intent : edits_)
            {
                auto edit = factory.make(intent);
                if (!edit)
                    return lux::cxx::unexpected(edit.error());
                auto step = edit->operation->prepare(context, budget);
                if (!step)
                    return lux::cxx::unexpected(step.error());
                if ((*step)->effect() == editing::EEditEffect::CHANGE)
                {
                    static_cast<FlowPlan&>(**step).commitCandidate();
                    ids.nodes.insert(ids.nodes.end(), edit->inserted->nodes.begin(), edit->inserted->nodes.end());
                    ids.variables.insert(
                        ids.variables.end(),
                        edit->inserted->variables.begin(),
                        edit->inserted->variables.end()
                    );
                }
            }
            auto after = lux::flowforge::captureFlowSource(candidate->id, candidate->name, candidate->graph);
            if (!after)
                return lux::cxx::unexpected(editing::makeEditFailure(
                    editing::EEditError::PRECONDITION_FAILED,
                    static_cast<std::uint64_t>(after.error().code)
                ));
            const bool changed = *frozen != *after;
            // Signature expansion can create more nodes than the input payload. Check the actual
            // final graph against the retained upper bound before live content is touched.
            if (sourceBytes(owner_.source_) + sourceBytes(*candidate) > retained_)
                return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
            return editing::PreparedEditPtr(new Plan(*this, std::move(candidate), std::move(ids), changed));
        }

    private:
        FlowEditContext owner_; // Metadata and code outlive all input, live and parked nodes.
        mutable std::vector<VFlowEdit> edits_;
        std::string label_;
        std::size_t retained_;
        mutable std::unique_ptr<FlowAuthoringSource> parked_;
    };
}
namespace lux::editor::flowforge
{
    editing::EditResult<PreparedFlowEdit> prepareFlowEdit(
        FlowAuthoringSource& source,
        lux::flowforge::FlowSourceEnvironment environment,
        editing::StateId base,
        std::vector<VFlowEdit> edits,
        std::string label,
        contracts::CodeLease code,
        FlowEditObserver observer
    )
    {
        // Parameter order is not a lifetime policy. Keep code outside consumed inputs.
        struct Input
        {
            lux::flowforge::FlowSourceEnvironment environment;
            contracts::CodeLease code;
            std::vector<VFlowEdit> edits;
        } input{std::move(environment), std::move(code), std::move(edits)};
        detail::FlowEditContext context{source, input.environment, input.code, base, observer};
        if (input.edits.size() == 1)
            return detail::FlowEditFactory{context}.make(input.edits.front());
        return detail::prepared(
            std::make_unique<detail::FlowBatchEdit>(context, std::move(input.edits), std::move(label))
        );
    }
    editing::EditResult<PreparedFlowEdit> prepareBorrowedFlowInsert(
        FlowAuthoringSource& source,
        lux::flowforge::FlowSourceEnvironment environment,
        editing::StateId base,
        std::unique_ptr<lux::flowforge::Node>& input,
        lux::graph::GraphNodeLayout placement,
        FlowEditObserver observer
    )
    {
        auto code = environment.code_lifetime ? contracts::CodeLease::plugin(environment.code_lifetime)
                                              : contracts::CodeLease::builtin();
        return detail::FlowEditFactory{{source, std::move(environment), std::move(code), base, observer}
        }.insertNode(input, placement, false, false);
    }
}
