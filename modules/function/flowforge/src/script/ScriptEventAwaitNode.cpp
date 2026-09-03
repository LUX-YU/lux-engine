#include <lux/engine/flowforge/script/ScriptEventAwaitNode.hpp>

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

    struct ScriptEventAwaitNode::TypeStorage final
    {
        std::string name;
        lux::meta::RefType type;
    };

    ScriptEventAwaitNode::ScriptEventAwaitNode(
        std::uint64_t id,
        const lux::script::ScriptEventSourceDescription& source
    )
        : ExecIntermediateNode(id, ENodeOperation::SCRIPT_EVENT_WAIT, "Execute", "Received"), source_(source),
          type_(std::make_unique<TypeStorage>())
    {
        type_->name = source_.payload.canonical_name;
        type_->type = {
            .qtype = {
                static_cast<std::uint8_t>(baseType(source_.payload.abi_kind)),
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
            .name = type_->name,
            .hash = source_.payload.type_id,
            .size = source_.payload.size,
            .alignment = source_.payload.alignment
        };
        setName(source_.system_name + "." + source_.event_name);
        payload_pin_ = std::make_unique<DataOutPin>(this, DataPinInfo{"Payload", std::addressof(type_->type)});
    }

    ScriptEventAwaitNode::ScriptEventAwaitNode(const lux::script::ScriptEventSourceDescription& source)
        : ScriptEventAwaitNode(reinterpret_cast<std::uintptr_t>(this), source)
    {
    }

    ScriptEventAwaitNode::~ScriptEventAwaitNode() = default;
}
