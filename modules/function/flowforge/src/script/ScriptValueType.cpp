#include <lux/engine/flowforge/script/ScriptValueType.hpp>

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

    detail::ScriptValueType::ScriptValueType(const script::ScriptAbilityValueDescription& description) noexcept
        : name(description.canonical_name)
    {
        type = {
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
            .name = name,
            .hash = description.type_id,
            .size = description.size,
            .alignment = description.alignment
        };
    }
} // namespace lux::flowforge
