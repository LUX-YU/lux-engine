#include <lux/engine/flowforge/ControlNodes.hpp>
#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/NativeCallDefinition.hpp>
#include <lux/engine/flowforge/ScalarNodes.hpp>
#include <lux/engine/flowforge/graph/NodeRegistry.hpp>

#include <algorithm>

namespace lux::flowforge
{
    namespace
    {
        std::unique_ptr<NodeCreateInfo> createRecipe(
            std::string name,
            std::string category,
            std::string node_name,
            std::shared_ptr<const FlowNodeType> definition,
            const meta::RefType* operand_type = nullptr
        ) noexcept
        {
            auto result = std::make_unique<NodeCreateInfo>();
            result->name = std::move(name);
            result->category = std::move(category);
            result->creator = [
                definition = std::move(definition),
                operand_type,
                name = std::move(node_name)
            ]() noexcept -> FlowForgeResult<FlowNode>
            {
                auto payload = definition->create();
                if (!payload)
                {
                    return cxx::unexpected(std::move(payload.error()));
                }
                if (operand_type)
                {
                    payload->get<ScalarNodePayload>()->operand_type = operand_type;
                }
                auto node = createFlowNode(definition, std::move(*payload));
                if (!node)
                {
                    return cxx::unexpected(std::move(node.error()));
                }
                node->name = name;
                return node;
            };
            return result;
        }
    } // namespace

    FlowForgeResult<std::unique_ptr<NodeRegistry>> NodeRegistry::create(object::CodeLease code) noexcept
    {
        if (!code.valid())
        {
            return cxx::unexpected(
                FlowForgeFailure{EFlowForgeError::INVALID_DESCRIPTION, "node recipes require a valid code lease"}
            );
        }
        return std::unique_ptr<NodeRegistry>(new NodeRegistry(std::move(code)));
    }

    NodeRegistry::NodeRegistry(object::CodeLease code) noexcept
    {
        registerBuiltinNodes(std::move(code));
    }

    NodeRegistry::~NodeRegistry() = default;

    void NodeRegistry::registerBuiltinNodes(object::CodeLease code) noexcept
    {
        FlowNodeCatalog definitions;
        const bool has_controls = definitions.add(controlNodeRegistrations(code)).has_value();
        const bool has_scalars = definitions.add(scalarNodeRegistrations(code)).has_value();
        const auto native = nativeCallRegistration(std::move(code));
        const bool has_native = definitions.add({&native, 1}).has_value();
        const bool has_all_definitions = has_controls && has_scalars && has_native;
        if (!has_all_definitions)
        {
            std::terminate(); // Inconsistent module-owned declarations.
        }
        native_type_ = definitions.find(native.identity.id);
        const auto install = [&](
            std::string name,
            std::string category,
            std::string node_name,
            std::string_view type,
            const meta::RefType* operand = nullptr
        ) noexcept
        {
            auto definition = definitions.find(graph::nodeTypeId(type));
            if (!definition)
            {
                std::terminate();
            }
            auto recipe = createRecipe(
                std::move(name),
                std::move(category),
                std::move(node_name),
                std::move(definition),
                operand
            );
            if (!registerNode(std::move(recipe)))
            {
                std::terminate();
            }
        };

        struct Entry final
        {
            std::string_view name;
            std::string_view type;
        };

        constexpr Entry controls[]{
            {"Start", "lux.flow.start"},
            {"Branch", "lux.flow.branch"},
            {"Sequence", "lux.flow.sequence"},
            {"For Loop", "lux.flow.for_loop"},
            {"While Loop", "lux.flow.while_loop"},
            {"Break", "lux.flow.break"},
            {"Return", "lux.flow.return"}
        };
        for (const auto& entry : controls)
        {
            install(std::string(entry.name), "Control Flow", std::string(entry.name), entry.type);
        }
        constexpr Entry math[]{
            {"Add", "lux.flow.add"},
            {"Subtract", "lux.flow.subtract"},
            {"Multiply", "lux.flow.multiply"},
            {"Divide", "lux.flow.divide"}
        };
        constexpr Entry comparisons[]{
            {"Equal", "lux.flow.equal"},
            {"Not Equal", "lux.flow.not_equal"},
            {"Less", "lux.flow.less"},
            {"Less Equal", "lux.flow.less_equal"},
            {"Greater", "lux.flow.greater"},
            {"Greater Equal", "lux.flow.greater_equal"}
        };
        const auto* integer = &meta::ref_type_of_v<std::int32_t>;
        const auto* real = &meta::ref_type_of_v<float>;
        const auto* boolean = &meta::ref_type_of_v<bool>;
        for (const auto& entry : math)
        {
            install(std::string(entry.name) + " (Int)", "Math", std::string(entry.name), entry.type, integer);
            install(std::string(entry.name) + " (Float)", "Math", std::string(entry.name), entry.type, real);
        }
        install("Modulo (Int)", "Math", "Modulo", "lux.flow.modulo", integer);
        for (const auto& entry : comparisons)
        {
            install(std::string(entry.name) + " (Int)", "Compare", std::string(entry.name), entry.type, integer);
            install(std::string(entry.name) + " (Float)", "Compare", std::string(entry.name), entry.type, real);
        }
        install("And", "Logic", "Logical And", "lux.flow.and", boolean);
        install("Or", "Logic", "Logical Or", "lux.flow.or", boolean);
        install("Not", "Logic", "Logical Not", "lux.flow.not", boolean);
        install("Negate (Int)", "Math", "Negate", "lux.flow.negate", integer);
        install("Negate (Float)", "Math", "Negate", "lux.flow.negate", real);
    }

    namespace
    {
        // A type the graph can carry across a native call boundary: scalars
        // map to first-class MLIR values, pointer-qualified types pass as
        // llvm.ptr. Everything else (by-value records, strings) stays out of
        // the auto-populated palette until the object model covers it.
        bool isGraphMappable(const lux::meta::RefType& rt, bool as_return) noexcept
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
        const meta::ReflectionRegistry& reflection,
        object::CodeLease code
    ) noexcept
    {
        const auto node_type = native_type_;
        std::size_t added{};
        for (const auto& function : reflection.functions())
        {
            const bool has_invoker = function && function->invokable.invoker;
            if (!has_invoker)
            {
                continue;
            }
            const auto& signature = function->invokable;
            const bool has_return = isGraphMappable(signature.return_type, true);
            const bool has_parameters = std::ranges::all_of(
                signature.parameters,
                [](const auto& parameter) noexcept { return isGraphMappable(parameter.type, false); }
            );
            const bool is_supported_signature = has_return && has_parameters;
            if (!is_supported_signature)
            {
                continue;
            }
            auto native = NativeCallDefinition::create(signature, code);
            if (!native)
            {
                continue;
            }
            auto info = std::make_unique<NodeCreateInfo>();
            info->name = std::string(signature.name);
            info->category = "Native";
            info->creator = [node_type, native = std::move(*native)]() noexcept -> FlowForgeResult<FlowNode>
            {
                auto payload = node_type->create();
                if (!payload)
                {
                    return cxx::unexpected(std::move(payload.error()));
                }
                payload->get<NativeCallPayload>()->definition = native;
                auto node = createFlowNode(node_type, std::move(*payload));
                if (!node)
                {
                    return cxx::unexpected(std::move(node.error()));
                }
                node->name = std::string(native->signature().name);
                return node;
            };
            if (registerNode(std::move(info)))
            {
                ++added;
            }
        }
        return added;
    }

    bool NodeRegistry::registerNode(std::unique_ptr<NodeCreateInfo> info) noexcept
    {
        if (!info)
        {
            return false;
        }
        const bool has_factory = static_cast<bool>(info->creator);
        const bool has_name = !info->name.empty() && !node_name_map_.contains(info->name);
        const bool is_invalid_recipe = !has_factory || !has_name;
        if (is_invalid_recipe)
        {
            return false;
        }
        // A palette label is captured once, independently of the recipe's lifetime.
        info->creator = [
            factory = std::move(info->creator),
            name = info->name
        ]() mutable noexcept -> FlowForgeResult<FlowNode>
        {
            auto node = factory();
            if (node)
            {
                node->creator = name;
            }
            return node;
        };
        node_name_map_.emplace(info->name, info.get());
        node_category_map_[info->category].push_back(info.get());
        node_creators_.push_back(std::move(info));
        return true;
    }

    NodeCreateInfo* NodeRegistry::findNodeByName(const std::string& name) const noexcept
    {
        const auto found = node_name_map_.find(name);
        return found == node_name_map_.end() ? nullptr : found->second;
    }

    NodeCreateInfo* NodeRegistry::findNodeByCategory(const std::string& name) const noexcept
    {
        const auto found = node_category_map_.find(name);
        return found == node_category_map_.end() ? nullptr : found->second.front();
    }
} // namespace lux::flowforge
