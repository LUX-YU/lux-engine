#include <lux/engine/asset/ScriptDescriptionCodec.hpp>

#include <lux/engine/asset/detail/ByteIO.hpp>

#include <utility>

namespace lux::asset::detail
{
    namespace
    {
        inline void writeType(ByteWriter& w, const lux::rdesc::Script::Type& t)
        {
            w.str(t.name);
            w.u64(t.type_id);
            w.u32(t.size);
            w.u32(t.align);
            w.u8 (t.kind);
            w.u8 (0);
            w.u8 (0);
            w.u8 (0);
        }

        inline bool readType(ByteReader& c, lux::rdesc::Script::Type& t) noexcept
        {
            if (!c.str(t.name, kMaxStringLen)) return false;
            if (!c.u64(t.type_id))             return false;
            if (!c.u32(t.size))                return false;
            if (!c.u32(t.align))               return false;
            std::uint8_t pad[4];
            if (!c.bytes(pad, 4))              return false;
            t.kind = pad[0];
            return true;
        }
    } // namespace

    std::vector<std::byte> encodeScriptDescription(const lux::rdesc::Script& s)
    {
        ByteWriter w;
        w.reserve(256);

        w.u32(kScriptDescMagic);
        w.u32(kEndianTag);
        w.u32(s.schema_version);
        w.u32(s.abi_version);
        w.u8 (static_cast<std::uint8_t>(s.kind));
        w.u8 (0);
        w.u8 (0);
        w.u8 (0);
        w.str(s.module_name);

        w.u32(static_cast<std::uint32_t>(s.functions.size()));
        for (const auto& f : s.functions)
        {
            w.str(f.name);
            w.u64(f.symbol_id);
            w.str(f.native_symbol);
            w.u32(static_cast<std::uint32_t>(f.args.size()));
            for (const auto& t : f.args)    writeType(w, t);
            w.u32(static_cast<std::uint32_t>(f.returns.size()));
            for (const auto& t : f.returns) writeType(w, t);
        }

        w.u32(static_cast<std::uint32_t>(s.dependencies.size()));
        for (const auto& d : s.dependencies)
        {
            w.str(d.kind);
            w.str(d.id);
        }

        w.str(s.provenance.compiler_id);
        w.str(s.provenance.compiler_version);
        w.str(s.provenance.source_id);
        w.str(s.provenance.source_hash);
        w.str(s.provenance.built_at);

        w.u32(kScriptDescTrailer);
        return std::move(w).take();
    }

    bool decodeScriptDescription(std::span<const std::byte> blob,
                                 lux::rdesc::Script&        out,
                                 std::string*               error_out) noexcept
    {
        ByteReader c{ blob, error_out };
        lux::rdesc::Script tmp{};

        std::uint32_t magic = 0;
        if (!c.u32(magic)) return false;
        if (magic != kScriptDescMagic) { c.fail("bad magic"); return false; }

        std::uint32_t endian = 0;
        if (!c.u32(endian)) return false;
        if (endian != kEndianTag) { c.fail("bad endian tag"); return false; }

        if (!c.u32(tmp.schema_version)) return false;
        if (!c.u32(tmp.abi_version))    return false;

        std::uint8_t kind_byte = 0;
        std::uint8_t pad[3]    = {};
        if (!c.u8(kind_byte))     return false;
        if (!c.bytes(pad, 3))     return false;
        tmp.kind = static_cast<lux::rdesc::Script::Kind>(kind_byte);

        if (!c.str(tmp.module_name, kMaxStringLen)) return false;

        std::uint32_t fn_count = 0;
        if (!c.u32(fn_count)) return false;
        if (fn_count > kMaxFunctionCount) { c.fail("function_count exceeds limit"); return false; }
        tmp.functions.resize(fn_count);
        for (auto& f : tmp.functions)
        {
            if (!c.str(f.name, kMaxStringLen))          return false;
            if (!c.u64(f.symbol_id))                    return false;
            if (!c.str(f.native_symbol, kMaxStringLen)) return false;

            std::uint32_t a_count = 0;
            if (!c.u32(a_count)) return false;
            if (a_count > kMaxArgumentCount) { c.fail("args_count exceeds limit"); return false; }
            f.args.resize(a_count);
            for (auto& t : f.args) if (!readType(c, t)) return false;

            std::uint32_t r_count = 0;
            if (!c.u32(r_count)) return false;
            if (r_count > kMaxArgumentCount) { c.fail("returns_count exceeds limit"); return false; }
            f.returns.resize(r_count);
            for (auto& t : f.returns) if (!readType(c, t)) return false;
        }

        std::uint32_t dep_count = 0;
        if (!c.u32(dep_count)) return false;
        if (dep_count > kMaxDependencyCnt) { c.fail("dependency_count exceeds limit"); return false; }
        tmp.dependencies.resize(dep_count);
        for (auto& d : tmp.dependencies)
        {
            if (!c.str(d.kind, kMaxStringLen)) return false;
            if (!c.str(d.id, kMaxStringLen))   return false;
        }

        if (!c.str(tmp.provenance.compiler_id, kMaxStringLen))      return false;
        if (!c.str(tmp.provenance.compiler_version, kMaxStringLen)) return false;
        if (!c.str(tmp.provenance.source_id, kMaxStringLen))        return false;
        if (!c.str(tmp.provenance.source_hash, kMaxStringLen))      return false;
        if (!c.str(tmp.provenance.built_at, kMaxStringLen))         return false;

        std::uint32_t trailer = 0;
        if (!c.u32(trailer)) return false;
        if (trailer != kScriptDescTrailer) { c.fail("bad trailer magic"); return false; }

        out = std::move(tmp);
        return true;
    }
}
