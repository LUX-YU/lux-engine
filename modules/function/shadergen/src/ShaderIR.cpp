// =============================================================================
//  ShaderIR.cpp  —  ShaderIR 内容的单点指纹（所有客户复用，规则不漂移）
// =============================================================================

#include <lux/engine/shadergen/ShaderIR.hpp>

#include <cstring>
#include <string>

namespace lux::shadergen
{
    uint64_t computeFingerprint(const ShaderIR& ir) noexcept
    {
        uint64_t f = 1469598103934665603ull;  // FNV-1a 64
        auto mix  = [&](uint64_t x) noexcept { f ^= x; f *= 1099511628211ull; };
        auto mixf = [&](float v) noexcept { uint32_t u; std::memcpy(&u, &v, 4); mix(u); };
        auto mixs = [&](const std::string& s) noexcept {
            mix(s.size());
            for (char ch : s) mix(static_cast<unsigned char>(ch));
        };

        mix(ir.values.size());
        for (const auto& v : ir.values)
        {
            mix(static_cast<uint16_t>(v.op));
            mix(static_cast<uint8_t>(v.type));
            for (int k = 0; k < 4; ++k) mix(v.operands[k]);
            mix(v.slot);
            for (int k = 0; k < 4; ++k) mix(v.swizzle[k]);
            for (int k = 0; k < 4; ++k) mixf(v.constant[k]);
        }
        mix(ir.outputs.size());
        for (const auto& o : ir.outputs)
        {
            mixs(o.name);
            mix(o.value_id);
            mix(static_cast<uint8_t>(o.type));
            // 默认值只在 value_id==kNoValue 时直入 SPIR-V 字面量 -> 仅此时进指纹；
            // 否则 dflt 不影响发射，仅 dflt 不同的两图必须同指纹。
            if (o.value_id == kNoValue)
                for (int k = 0; k < 4; ++k) mixf(o.dflt[k]);
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
            mixs(t.name);
        mix(ir.params.size());
        for (const auto& p : ir.params)
        {
            mixs(p.name);
            mix(static_cast<uint8_t>(p.type));
            // 参数默认值不进指纹：set4 SSBO uniform（发射 `_mat.params[slot]`，非字面量），
            // 仅值不同的两图产出 byte-identical SPIR-V，必须同指纹 -> 共享 PSO。
        }
        mix(ir.raw_blocks.size());
        for (const auto& r : ir.raw_blocks)
        {
            mixs(r.language);
            mixs(r.code);  // raw 文本直入 shader -> 必须进指纹
        }
        return f;
    }

} // namespace lux::shadergen
