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

    struct ScriptEventAwaitNode::TypeStorage final
    {
        std::string name;
        lux::meta::RefType type;
    };

    std::size_t ScriptEventAwaitNode::descriptionBytes() const noexcept
    {
        return sizeof(TypeStorage) + type_->name.capacity() + source_.system_name.capacity() +
               source_.event_name.capacity() + source_.payload.canonical_name.capacity() + 4;
    }

    ScriptEventAwaitNode::ScriptEventAwaitNode(const lux::script::ScriptEventSourceDescription& source)
        : ExecIntermediateNode(ENodeOperation::SCRIPT_EVENT_WAIT, "Execute", "Received"), source_(source),
          type_(std::make_unique<TypeStorage>())
    {
        type_->name = source_.payload.canonical_name;
        type_->type = {
            .qtype =
                {static_cast<std::uint8_t>(baseType(source_.payload.abi_kind)),
                 static_cast<std::uint8_t>(lux::meta::ETypeQual::VALUE)},
            .traits =
                {.is_standard_layout = true,
                 .is_trivially_constructible = true,
                 .is_trivially_copyable = true,
                 .is_trivially_default_constructible = true,
                 .is_trivially_destructible = true,
                 .is_trivially_move_assignable = true,
                 .is_trivially_move_constructible = true},
            .name = type_->name,
            .hash = source_.payload.type_id,
            .size = source_.payload.size,
            .alignment = source_.payload.alignment
        };
        setName(source_.system_name + "." + source_.event_name);
        payload_pin_ = std::make_unique<DataOutPin>(this, DataPinInfo{"Payload", std::addressof(type_->type)});
    }

    ScriptEventAwaitNode::~ScriptEventAwaitNode() = default;
} // namespace lux::flowforge
