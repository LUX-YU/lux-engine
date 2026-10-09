#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNodeStorage.hpp>

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
            case EAbiKind::BOOL:
                return lux::meta::EBaseType::BOOL;
            case EAbiKind::I32:
                return lux::meta::EBaseType::INT32;
            case EAbiKind::U32:
                return lux::meta::EBaseType::UINT32;
            case EAbiKind::I64:
                return lux::meta::EBaseType::INT64;
            case EAbiKind::U64:
                return lux::meta::EBaseType::UINT64;
            case EAbiKind::F32:
                return lux::meta::EBaseType::FLOAT;
            case EAbiKind::F64:
                return lux::meta::EBaseType::DOUBLE;
            case EAbiKind::STRUCT_REF:
                return lux::meta::EBaseType::RECORD;
            default:
                return lux::meta::EBaseType::UNKNOWN;
            }
        }
    } // namespace

    struct ScriptAbilityNode::TypeStorage final
    {
        std::string name;
        lux::meta::RefType type;
    };

    std::size_t ScriptAbilityNode::descriptionBytes() const noexcept
    {
        std::size_t bytes = description_->bytes() +
                            (types_.capacity() + parameter_pins_.capacity() + result_pins_.capacity()) * sizeof(void*);
        for (const auto& type : types_)
        {
            bytes += sizeof(TypeStorage) + type->name.capacity() + 1;
        }
        return bytes;
    }

    ScriptAbilityNode::ScriptAbilityNode(const ScriptAbilityNodeDescription& description)
        : ExecIntermediateNode(ENodeOperation::SCRIPT_ABILITY_CALL, "Execute", "Completed"),
          description_(std::make_unique<detail::ScriptAbilityNodeStorage>(description))
    {
        const auto& owned = description_->description();
        setName(owned.method_display_name.empty() ? owned.method.name() : owned.method_display_name);
        types_.reserve(owned.parameters.size() + owned.results.size());
        parameter_pins_.reserve(owned.parameters.size());
        result_pins_.reserve(owned.results.size());

        for (const auto& parameter : owned.parameters)
        {
            parameter_pins_.push_back(std::make_unique<DataInPin>(
                this,
                DataPinInfo{std::string(parameter.name), storeType(parameter.value)},
                true
            ));
        }
        for (std::size_t index{}; index < owned.results.size(); ++index)
        {
            const auto name = owned.results.size() == 1U ? "Result" : "Result " + std::to_string(index);
            result_pins_.push_back(
                std::make_unique<DataOutPin>(this, DataPinInfo{name, storeType(owned.results[index])})
            );
        }
    }

    ScriptAbilityNode::~ScriptAbilityNode() = default;

    lux::script::ScriptApiContractIdView ScriptAbilityNode::contract() const noexcept
    {
        return description_->description().contract;
    }

    lux::script::ScriptApiMethodIdView ScriptAbilityNode::method() const noexcept
    {
        return description_->description().method;
    }

    std::uint32_t ScriptAbilityNode::expectedSchemaVersion() const noexcept
    {
        return description_->description().schema_version;
    }

    std::uint64_t ScriptAbilityNode::expectedSchemaHash() const noexcept
    {
        return description_->description().schema_hash;
    }

    lux::script::EScriptApiMethodKind ScriptAbilityNode::methodKind() const noexcept
    {
        return description_->description().kind;
    }

    lux::script::EScriptAbilityReceiverKind ScriptAbilityNode::receiverKind() const noexcept
    {
        return description_->description().receiver;
    }

    std::span<const lux::script::ScriptAbilityParameterDescription> ScriptAbilityNode::parameters() const noexcept
    {
        return description_->description().parameters;
    }

    std::span<const lux::script::ScriptAbilityValueDescription> ScriptAbilityNode::results() const noexcept
    {
        return description_->description().results;
    }

    const lux::meta::RefType* ScriptAbilityNode::storeType(const lux::script::ScriptAbilityValueDescription& description
    )
    {
        auto storage = std::make_unique<TypeStorage>();
        storage->name = description.canonical_name;
        storage->type = {
            .qtype =
                {static_cast<std::uint8_t>(baseType(description.abi_kind)),
                 static_cast<std::uint8_t>(lux::meta::ETypeQual::VALUE)},
            .traits =
                {.is_standard_layout = true,
                 .is_trivially_constructible = true,
                 .is_trivially_copyable = true,
                 .is_trivially_default_constructible = true,
                 .is_trivially_destructible = true,
                 .is_trivially_move_assignable = true,
                 .is_trivially_move_constructible = true},
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
