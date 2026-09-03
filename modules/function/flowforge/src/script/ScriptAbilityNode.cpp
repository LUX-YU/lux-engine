#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>

#include <lux/engine/meta/MetaDef.hpp>

#include <string>

namespace lux::flowforge
{
    namespace
    {
        [[nodiscard]] lux::meta::EBaseType baseType(std::uint8_t abi_kind) noexcept
        {
            using lux::semantic::EAbiKind;
            switch (static_cast<EAbiKind>(abi_kind))
            {
            case EAbiKind::BOOL: return lux::meta::EBaseType::Bool;
            case EAbiKind::I32: return lux::meta::EBaseType::Int32;
            case EAbiKind::U32: return lux::meta::EBaseType::Uint32;
            case EAbiKind::I64: return lux::meta::EBaseType::Int64;
            case EAbiKind::U64: return lux::meta::EBaseType::Uint64;
            case EAbiKind::F32: return lux::meta::EBaseType::Float;
            case EAbiKind::F64: return lux::meta::EBaseType::Double;
            case EAbiKind::STRUCT_REF: return lux::meta::EBaseType::Record;
            default: return lux::meta::EBaseType::Unknown;
            }
        }
    }

    struct ScriptAbilityNode::TypeStorage final
    {
        std::string name;
        lux::meta::RefType type;
    };

    ScriptAbilityNode::ScriptAbilityNode(std::uint64_t id, const ScriptAbilityNodeDescription& description)
        : ExecIntermediateNode(id, ENodeOperation::SCRIPT_ABILITY_CALL, "Execute", "Completed"),
          contract_(description.contract.name()),
          method_(description.method.name()),
          schema_version_(description.schema_version),
          schema_hash_(description.schema_hash),
          kind_(description.kind),
          receiver_(description.receiver),
          parameters_(description.parameters.begin(), description.parameters.end()),
          results_(description.results.begin(), description.results.end())
    {
        setName(description.method_display_name.empty() ? description.method.name() : description.method_display_name);
        types_.reserve(parameters_.size() + results_.size());
        parameter_pins_.reserve(parameters_.size());
        result_pins_.reserve(results_.size());

        for (const auto& parameter : parameters_)
        {
            parameter_pins_.push_back(std::make_unique<DataInPin>(
                this,
                DataPinInfo{std::string(parameter.name), storeType(parameter.value)},
                true
            ));
        }
        for (std::size_t index{}; index < results_.size(); ++index)
        {
            const auto name = results_.size() == 1U ? "Result" : "Result " + std::to_string(index);
            result_pins_.push_back(std::make_unique<DataOutPin>(
                this,
                DataPinInfo{name, storeType(results_[index])}
            ));
        }
    }

    ScriptAbilityNode::ScriptAbilityNode(const ScriptAbilityNodeDescription& description)
        : ScriptAbilityNode(reinterpret_cast<std::uintptr_t>(this), description)
    {
    }

    ScriptAbilityNode::~ScriptAbilityNode() = default;

    const lux::meta::RefType* ScriptAbilityNode::storeType(
        const lux::script::ScriptAbilityValueDescription& description
    )
    {
        auto storage = std::make_unique<TypeStorage>();
        storage->name = description.canonical_name;
        storage->type = {
            .qtype = {
                static_cast<std::uint8_t>(baseType(description.abi_kind)),
                static_cast<std::uint8_t>(lux::meta::ETypeQual::Value)
            },
            .traits = {
                .is_standard_layout = true,
                .is_trivially_constructible = true,
                .is_trivially_copyable = true,
                .is_trivially_default_constructible = true,
                .is_trivially_destructible = true,
                .is_trivially_move_assignable = true,
                .is_trivially_move_constructible = true
            },
            .name = storage->name,
            .hash = description.type_id,
            .size = description.size,
            .alignment = description.alignment
        };
        const auto* result = &storage->type;
        types_.push_back(std::move(storage));
        return result;
    }
} // namespace lux::flowforge
