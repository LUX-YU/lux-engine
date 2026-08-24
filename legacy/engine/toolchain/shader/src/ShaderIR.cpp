// =============================================================================
//  ShaderIR.cpp — single source of truth for fingerprinting ShaderIR content
//  (shared by every client, so the rules can't drift)
// =============================================================================

#include <lux/engine/toolchain/shader/ShaderIR.hpp>

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
            // The default value only becomes a SPIR-V literal when
            // value_id==kNoValue, so it only enters the fingerprint in that
            // case; otherwise dflt has no effect on emission, and two graphs
            // that differ only in dflt must still hash identically.
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
            // Parameter default values do not enter the fingerprint: they live
            // in the set-4 SSBO uniform (emitted as `_mat.params[slot]`, not a
            // literal), so two graphs differing only in the value produce
            // byte-identical SPIR-V and must hash identically -> shared PSO.
        }
        mix(ir.raw_blocks.size());
        for (const auto& r : ir.raw_blocks)
        {
            mixs(r.language);
            mixs(r.code);  // raw text goes straight into the shader -> must be fingerprinted
        }
        return f;
    }

} // namespace lux::shadergen
