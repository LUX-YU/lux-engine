// =============================================================================
//  ShaderIR.cpp — single source of truth for fingerprinting ShaderIR content
//  (shared by every client, so the rules can't drift)
// =============================================================================

#include <lux/engine/material/ShaderIR.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace lux::shadergen
{
    namespace
    {
        bool validType(EValueType type) noexcept
        {
            return type <= EValueType::VEC4;
        }

        bool finite(const float (&values)[4]) noexcept
        {
            return std::ranges::all_of(values, [](float value) noexcept { return std::isfinite(value); });
        }

        template <class Slots> bool validNames(const Slots& slots) noexcept
        {
            for (std::size_t index = 0; index < slots.size(); ++index)
            {
                const auto& name = slots[index].name;
                if (name.empty() || name.size() > UINT32_MAX || name.find('\0') != std::string::npos)
                {
                    return false;
                }
                for (std::size_t previous = 0; previous < index; ++previous)
                {
                    if (slots[previous].name == name)
                    {
                        return false;
                    }
                }
            }
            return true;
        }
    } // namespace

    bool validateShaderIR(const ShaderIR& ir) noexcept
    {
        const bool is_invalid_count = ir.values.size() >= kNoValue || ir.outputs.empty() ||
                                      ir.outputs.size() > UINT32_MAX || ir.inputs.size() > UINT32_MAX ||
                                      ir.params.size() > UINT32_MAX || ir.textures.size() > UINT32_MAX ||
                                      ir.raw_blocks.size() > UINT32_MAX;
        const bool is_invalid_name =
            !validNames(ir.outputs) || !validNames(ir.inputs) || !validNames(ir.params) || !validNames(ir.textures);
        if (is_invalid_count || is_invalid_name)
        {
            return false;
        }

        for (const auto& input : ir.inputs)
        {
            const bool is_invalid_input =
                !validType(input.type) || input.location < -1 || input.interpolation > EInterpolation::FLAT;
            if (is_invalid_input)
            {
                return false;
            }
        }
        for (const auto& parameter : ir.params)
        {
            if (!validType(parameter.type) || !finite(parameter.dflt))
            {
                return false;
            }
        }
        for (const auto& block : ir.raw_blocks)
        {
            const bool is_invalid_block = block.language.empty() || block.code.empty() ||
                                          block.language.size() > UINT32_MAX || block.code.size() > UINT32_MAX ||
                                          block.language.find('\0') != std::string::npos ||
                                          block.code.find('\0') != std::string::npos;
            if (is_invalid_block)
            {
                return false;
            }
        }
        for (std::size_t index = 0; index < ir.values.size(); ++index)
        {
            const auto& value = ir.values[index];
            const bool is_invalid_value = !validType(value.type) || value.op > EOp::RAW_EXPR || !finite(value.constant);
            if (is_invalid_value)
            {
                return false;
            }
            for (auto operand : value.operands)
            {
                if (operand != kNoValue && operand >= index)
                {
                    return false;
                }
            }
            unsigned count = 0;
            switch (value.op)
            {
            case EOp::CONSTANT:
                break;
            case EOp::INPUT:
                if (value.slot >= ir.inputs.size() || ir.inputs[value.slot].type != value.type)
                {
                    return false;
                }
                break;
            case EOp::PARAM:
                if (value.slot >= ir.params.size() || ir.params[value.slot].type != value.type)
                {
                    return false;
                }
                break;
            case EOp::SAMPLE_TEXTURE:
                if (value.slot >= ir.textures.size() || value.type != EValueType::VEC4)
                {
                    return false;
                }
                count = 1;
                break;
            case EOp::LERP:
                count = 3;
                break;
            case EOp::CONSTRUCT:
                count = static_cast<unsigned>(value.type) + 1;
                break;
            case EOp::MUL:
            case EOp::ADD:
            case EOp::SUB:
            case EOp::DIV:
            case EOp::DOT:
            case EOp::MIN:
            case EOp::MAX:
            case EOp::POW:
            case EOp::STEP:
            case EOp::MOD:
            case EOp::CROSS:
            case EOp::REFLECT:
                count = 2;
                break;
            case EOp::RAW_EXPR:
                if (value.slot >= ir.raw_blocks.size())
                {
                    return false;
                }
                // Escape hatches may have holes; every referenced placeholder must be bound.
                for (std::size_t offset = 0; offset < ir.raw_blocks[value.slot].code.size(); ++offset)
                {
                    const auto& code = ir.raw_blocks[value.slot].code;
                    if (code[offset] != '$')
                    {
                        continue;
                    }
                    const bool has_digit =
                        offset + 1 < code.size() && code[offset + 1] >= '0' && code[offset + 1] <= '3';
                    if (!has_digit || value.operands[code[offset + 1] - '0'] == kNoValue)
                    {
                        return false;
                    }
                    ++offset;
                }
                continue;
            default:
                count = 1;
                break;
            }
            for (unsigned operand = 0; operand < 4; ++operand)
            {
                if ((operand < count) != (value.operands[operand] != kNoValue))
                {
                    return false;
                }
            }
            if (count == 0)
            {
                continue;
            }
            const auto first_type = ir.values[value.operands[0]].type;
            switch (value.op)
            {
            case EOp::SAMPLE_TEXTURE:
                if (first_type != EValueType::VEC2)
                {
                    return false;
                }
                break;
            case EOp::SWIZZLE:
                for (unsigned component = 0; component <= static_cast<unsigned>(value.type); ++component)
                {
                    if (value.swizzle[component] > static_cast<unsigned>(first_type))
                    {
                        return false;
                    }
                }
                break;
            case EOp::CONSTRUCT:
                for (unsigned operand = 0; operand < count; ++operand)
                {
                    if (ir.values[value.operands[operand]].type != EValueType::FLOAT)
                    {
                        return false;
                    }
                }
                break;
            default:
                for (unsigned operand = 1; operand < count; ++operand)
                {
                    const bool is_scalar_weight = value.op == EOp::LERP && operand == 2 &&
                                                  ir.values[value.operands[operand]].type == EValueType::FLOAT;
                    if (is_scalar_weight)
                    {
                        continue;
                    }
                    if (ir.values[value.operands[operand]].type != first_type)
                    {
                        return false;
                    }
                }
                const bool is_scalar_result = value.op == EOp::DOT || value.op == EOp::LENGTH;
                const bool is_vector3 =
                    value.op == EOp::CROSS || value.op == EOp::DECODE_NORMAL || value.op == EOp::TBN_NORMAL;
                const bool is_type_mismatch = value.type != (is_scalar_result ? EValueType::FLOAT : first_type) ||
                                              (is_vector3 && first_type != EValueType::VEC3) ||
                                              (value.op == EOp::DOT && first_type == EValueType::FLOAT);
                if (is_type_mismatch)
                {
                    return false;
                }
                break;
            }
        }
        for (const auto& output : ir.outputs)
        {
            const bool has_value = output.value_id != kNoValue;
            const bool is_invalid_reference =
                has_value && (output.value_id >= ir.values.size() || ir.values[output.value_id].type != output.type);
            const bool is_invalid_output = !validType(output.type) || !finite(output.dflt) || is_invalid_reference;
            if (is_invalid_output)
            {
                return false;
            }
        }
        return true;
    }

    uint64_t computeFingerprint(const ShaderIR& ir) noexcept
    {
        // Existing word-wise cache fingerprint; preserve its seed and mixing contract.
        uint64_t f = 1469598103934665603ull;
        auto mix = [&](uint64_t x) noexcept
        {
            f ^= x;
            f *= 1099511628211ull;
        };
        auto mixf = [&](float v) noexcept
        {
            uint32_t u;
            std::memcpy(&u, &v, 4);
            mix(u);
        };
        auto mixs = [&](const std::string& s) noexcept
        {
            mix(s.size());
            for (char ch : s)
            {
                mix(static_cast<unsigned char>(ch));
            }
        };

        mix(ir.values.size());
        for (const auto& v : ir.values)
        {
            mix(static_cast<uint16_t>(v.op));
            mix(static_cast<uint8_t>(v.type));
            for (int k = 0; k < 4; ++k)
            {
                mix(v.operands[k]);
            }
            mix(v.slot);
            for (int k = 0; k < 4; ++k)
            {
                mix(v.swizzle[k]);
            }
            for (int k = 0; k < 4; ++k)
            {
                mixf(v.constant[k]);
            }
        }
        mix(ir.outputs.size());
        for (const auto& o : ir.outputs)
        {
            mixs(o.name);
            mix(o.value_id);
            mix(static_cast<uint8_t>(o.type));
            // The default value only becomes a SPIR-V literal when
            // value_id==kNoValue, so it only enters the fingerprint in that
            // case; otherwise dflt has no effect on emission, and two graphs
            // that differ only in dflt must still hash identically.
            if (o.value_id == kNoValue)
            {
                for (int k = 0; k < 4; ++k)
                {
                    mixf(o.dflt[k]);
                }
            }
        }
        mix(ir.inputs.size());
        for (const auto& s : ir.inputs)
        {
            mixs(s.name);
            mix(static_cast<uint8_t>(s.type));
            mix(static_cast<uint32_t>(s.location));
            mix(static_cast<uint8_t>(s.interpolation));
        }
        mix(ir.textures.size());
        for (const auto& t : ir.textures)
        {
            mixs(t.name);
        }
        mix(ir.params.size());
        for (const auto& p : ir.params)
        {
            mixs(p.name);
            mix(static_cast<uint8_t>(p.type));
            // Parameter default values do not enter the fingerprint: they live
            // in the set-4 SSBO uniform (emitted as `_mat.params[slot]`, not a
            // literal), so two graphs differing only in the value produce
            // byte-identical SPIR-V and must hash identically -> shared PSO.
        }
        mix(ir.raw_blocks.size());
        for (const auto& r : ir.raw_blocks)
        {
            mixs(r.language);
            mixs(r.code); // raw text goes straight into the shader -> must be fingerprinted
        }
        return f;
    }

} // namespace lux::shadergen
