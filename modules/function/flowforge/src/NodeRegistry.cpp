#include <lux/engine/flowforge/NativeCallDefinition.hpp>
#include <lux/engine/flowforge/ScalarNodes.hpp>
#include <lux/engine/flowforge/detail/FlowNodeIdentity.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/graph/NodeRegistry.hpp>

namespace lux::flowforge
{
    template <typename T>
        requires std::is_base_of_v<lux::flowforge::Node, T>
    std::unique_ptr<NodeCreatInfo> createControlFlowCreator(std::string_view name)
    {
        auto ptr = std::make_unique<NodeCreatInfo>();
        ptr->name = name;
        ptr->category = "Control Flow";
        ptr->creator = []() -> std::unique_ptr<Node> { return std::make_unique<T>(); };

        return ptr;
    }

    // Palette entry for a typed binary/unary pure-data node. The palette is
    // flat name -> creator, so per-type variants get a " (Int)" / " (Float)"
    // suffix; bool-typed logic ops need no suffix (they only exist for bool).
    static std::unique_ptr<NodeCreatInfo> createArithmeticCreator(
        std::string name,
        std::string category,
        std::shared_ptr<const FlowNodeType> definition,
        const lux::meta::RefType* operand_type
    )
    {
        auto ptr = std::make_unique<NodeCreatInfo>();
        ptr->name = std::move(name);
        ptr->category = std::move(category);
        ptr->creator = [definition = std::move(definition), operand_type]() -> std::unique_ptr<Node>
        {
            auto payload = definition->create();
            if (!payload)
            {
                return {};
            }
            payload->get<ScalarNodePayload>()->operand_type = operand_type;
            auto node = createFlowValueNode(definition, std::move(*payload));
            if (!node)
            {
                return {};
            }
            node.value()->setName(toString(detail::builtinNodeOperation(definition->identity().canonical_name)));
            return std::move(*node);
        };
        return ptr;
    }

    NodeRegistry::NodeRegistry()
    {
        registerBuiltinNodes();
    }

    NodeRegistry& NodeRegistry::global()
    {
        static NodeRegistry instance;
        return instance;
    }

    NodeRegistry::~NodeRegistry() {}

    void NodeRegistry::registerBuiltinNodes()
    {
        registerNode(createControlFlowCreator<StartNode>("Start"));
        registerNode(createControlFlowCreator<BranchNode>("Branch"));
        registerNode(createControlFlowCreator<SequenceNode>("Sequence"));
        registerNode(createControlFlowCreator<ForLoopNode>("For Loop"));
        registerNode(createControlFlowCreator<WhileLoopNode>("While Loop"));
        registerNode(createControlFlowCreator<BreakNode>("Break"));
        registerNode(createControlFlowCreator<ReturnNode>("Return"));

        FlowNodeCatalog scalars;
        if (!scalars.add(scalarNodeRegistrations()))
        {
            std::terminate(); // Inconsistent module-owned declarations.
        }
        const auto definition = [&](ENodeOperation operation) noexcept
        { return scalars.find(graph::nodeTypeId(detail::builtinNodeName(operation))); };

        // -------- pure data nodes: arithmetic / comparison / logic --------
        const auto* i32 = &lux::meta::ref_type_of_v<int32_t>;
        const auto* f32 = &lux::meta::ref_type_of_v<float>;
        const auto* bl = &lux::meta::ref_type_of_v<bool>;

        struct Entry
        {
            const char* base;
            ENodeOperation op;
        };

        static constexpr Entry binary_math[] = {
            {"Add", ENodeOperation::ADD},
            {"Subtract", ENodeOperation::SUBTRACT},
            {"Multiply", ENodeOperation::MULTIPLY},
            {"Divide", ENodeOperation::DIVIDE},
        };
        static constexpr Entry comparisons[] = {
            {"Equal", ENodeOperation::CMP_EQ},
            {"Not Equal", ENodeOperation::CMP_NE},
            {"Less", ENodeOperation::CMP_LT},
            {"Less Equal", ENodeOperation::CMP_LE},
            {"Greater", ENodeOperation::CMP_GT},
            {"Greater Equal", ENodeOperation::CMP_GE},
        };

        for (const auto& e : binary_math)
        {
            registerNode(createArithmeticCreator(std::string(e.base) + " (Int)", "Math", definition(e.op), i32));
            registerNode(createArithmeticCreator(std::string(e.base) + " (Float)", "Math", definition(e.op), f32));
        }
        // Modulo is integer-only in the palette (arith.remf exists but the
        // gameplay-facing default keeps float modulo out until asked for).
        registerNode(createArithmeticCreator("Modulo (Int)", "Math", definition(ENodeOperation::MODULO), i32));

        for (const auto& e : comparisons)
        {
            registerNode(createArithmeticCreator(std::string(e.base) + " (Int)", "Compare", definition(e.op), i32));
            registerNode(createArithmeticCreator(std::string(e.base) + " (Float)", "Compare", definition(e.op), f32));
        }

        registerNode(createArithmeticCreator("And", "Logic", definition(ENodeOperation::LOGICAL_AND), bl));
        registerNode(createArithmeticCreator("Or", "Logic", definition(ENodeOperation::LOGICAL_OR), bl));
        registerNode(createArithmeticCreator("Not", "Logic", definition(ENodeOperation::LOGICAL_NOT), bl));
        registerNode(createArithmeticCreator("Negate (Int)", "Math", definition(ENodeOperation::NEGATE), i32));
        registerNode(createArithmeticCreator("Negate (Float)", "Math", definition(ENodeOperation::NEGATE), f32));
    }

    namespace
    {
        // A type the graph can carry across a native call boundary: scalars
        // map to first-class MLIR values, pointer-qualified types pass as
        // llvm.ptr. Everything else (by-value records, strings) stays out of
        // the auto-populated palette until the object model covers it.
        bool isGraphMappable(const lux::meta::RefType& rt, bool as_return)
        {
            using lux::meta::EBaseType;
            using lux::meta::ETypeQual;
            const auto qual = static_cast<ETypeQual>(rt.qtype.qual);
            switch (qual)
            {
            case ETypeQual::PTR:
            case ETypeQual::PTR_TO_CONST:
            case ETypeQual::CONST_PTR:
            case ETypeQual::CONST_PTR_TO_CONST:
                return true; // all pointer flavors pass as llvm.ptr
            case ETypeQual::VALUE:
                break;
            default:
                return false; // references need by-ref semantics — later
            }
            switch (static_cast<EBaseType>(rt.qtype.base))
            {
            case EBaseType::BOOL:
            case EBaseType::INT8:
            case EBaseType::UINT8:
            case EBaseType::INT16:
            case EBaseType::UINT16:
            case EBaseType::INT32:
            case EBaseType::UINT32:
            case EBaseType::INT64:
            case EBaseType::UINT64:
            case EBaseType::FLOAT:
            case EBaseType::DOUBLE:
                return true;
            case EBaseType::VOID:
                return as_return;
            default:
                return false;
            }
        }
    } // namespace

    std::size_t NodeRegistry::populateFromReflection(
        const lux::meta::ReflectionRegistry& reflection,
        object::CodeLease code
    )
    {
        std::size_t added = 0;
        for (const auto& fn_ptr : reflection.functions())
        {
            const lux::meta::RefFunction* fn = fn_ptr.get();
            if (!fn || !fn->invokable.invoker)
            {
                continue; // no callable trampoline
            }
            if (!isGraphMappable(fn->invokable.return_type, /*as_return=*/true))
            {
                continue;
            }
            bool ok = true;
            for (const auto& p : fn->invokable.parameters)
            {
                ok = ok && isGraphMappable(p.type, /*as_return=*/false);
            }
            if (!ok)
            {
                continue;
            }

            auto definition = NativeCallDefinition::create(fn->invokable, code);
            if (!definition)
            {
                continue;
            }
            auto info = std::make_unique<NodeCreatInfo>();
            info->name = std::string(fn->invokable.name);
            info->category = "Native";
            info->creator = [definition = std::move(*definition)]() -> std::unique_ptr<Node>
            { return std::make_unique<NativeFuncCall>(definition); };
            if (registerNode(std::move(info)))
            {
                ++added;
            }
        }
        return added;
    }

    bool NodeRegistry::registerNode(std::unique_ptr<NodeCreatInfo> info)
    {
        if (!info)
        {
            return false;
        }

        // Stamp the creator name on every instantiated node — the graph
        // serializer re-instantiates registry-backed nodes (native calls in
        // particular) by this name on load.
        if (info->creator)
        {
            auto raw_creator = std::move(info->creator);
            info->creator = [raw = std::move(raw_creator), name = info->name]() -> std::unique_ptr<Node>
            {
                auto node = raw();
                if (node)
                {
                    node->setCreatorName(name);
                }
                return node;
            };
        }

        auto [it, inserted] = node_name_map_.try_emplace(info->name, info.get());
        if (!inserted)
        {
            return false;
        }
        node_category_map_[info->category].push_back(info.get());
        node_creators_.push_back(std::move(info));

        return true;
    }

    NodeCreatInfo* NodeRegistry::findNodeByName(const std::string& name) const
    {
        auto it = node_name_map_.find(name);
        return it != node_name_map_.end() ? it->second : nullptr;
    }

    NodeCreatInfo* NodeRegistry::findNodeByCategory(const std::string& name) const
    {
        auto it = node_category_map_.find(name);
        return it != node_category_map_.end() ? it->second.front() : nullptr;
    }
} // namespace lux::flowforge
