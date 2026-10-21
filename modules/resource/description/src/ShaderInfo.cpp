#include <algorithm>
#include <cstring>
#include <lux/engine/description/ShaderInfo.hpp>
#include <map>
#include <span>

namespace lux::rdesc
{
    enum class EShaderInfoOp : uint8_t
    {
        ENTRY_POINT = 0x01,
        BINDING = 0x02,
        PUSH_CONST = 0x03,
        SPEC_CONST = 0x04,
        VERTEX_INPUT = 0x05,
        BINDING_LAYOUT = 0x06,
        PUSH_LAYOUT = 0x07,
        ENTRY_INPUT = 0x08,
        ENTRY_OUTPUT = 0x09,
        IMAGE_SHAPE = 0x0A,
        END = 0xFF
    };

    // ================= Basic buffer I/O (little-endian) =================
    template <typename T> static inline void appendLE(std::vector<std::byte>& out, const T& v)
    {
        static_assert(std::is_trivially_copyable<T>::value, "POD only");
        const auto* p = reinterpret_cast<const std::byte*>(&v);
        out.insert(out.end(), p, p + sizeof(T));
    }

    static inline void appendBytes(std::vector<std::byte>& out, const void* p, size_t n)
    {
        out.insert(out.end(), reinterpret_cast<const std::byte*>(p), reinterpret_cast<const std::byte*>(p) + n);
    }

    static inline void appendU8(std::vector<std::byte>& out, uint8_t v)
    {
        appendLE(out, v);
    }

    static inline void appendU32(std::vector<std::byte>& out, uint32_t v)
    {
        appendLE(out, v);
    }

    [[maybe_unused]] static inline void appendU64(std::vector<std::byte>& out, uint64_t v)
    {
        appendLE(out, v);
    }

    [[maybe_unused]] static inline void appendI64(std::vector<std::byte>& out, int64_t v)
    {
        appendLE(out, v);
    }

    [[maybe_unused]] static inline void appendF64(std::vector<std::byte>& out, double v)
    {
        appendLE(out, v);
    }

    static inline void appendStr(std::vector<std::byte>& out, const std::string& s)
    {
        appendU32(out, static_cast<uint32_t>(s.size()));
        if (!s.empty())
        {
            appendBytes(out, s.data(), s.size());
        }
    }

    template <typename T> static inline bool readLE(std::span<const std::byte> buf, size_t& off, T& v)
    {
        static_assert(std::is_trivially_copyable<T>::value, "POD only");
        if (off + sizeof(T) > buf.size())
        {
            return false;
        }
        std::memcpy(&v, buf.data() + off, sizeof(T));
        off += sizeof(T);
        return true;
    }

    static inline bool readBytes(std::span<const std::byte> buf, size_t& off, void* dst, size_t n)
    {
        if (off + n > buf.size())
        {
            return false;
        }
        std::memcpy(dst, buf.data() + off, n);
        off += n;
        return true;
    }

    static inline bool readU8(std::span<const std::byte> b, size_t& o, uint8_t& v)
    {
        return readLE(b, o, v);
    }

    static inline bool readU32(std::span<const std::byte> b, size_t& o, uint32_t& v)
    {
        return readLE(b, o, v);
    }

    static inline bool readU64(std::span<const std::byte> b, size_t& o, uint64_t& v)
    {
        return readLE(b, o, v);
    }

    static inline bool readI64(std::span<const std::byte> b, size_t& o, int64_t& v)
    {
        return readLE(b, o, v);
    }

    static inline bool readF64(std::span<const std::byte> b, size_t& o, double& v)
    {
        return readLE(b, o, v);
    }

    static inline bool readStr(std::span<const std::byte> b, size_t& o, std::string& s)
    {
        uint32_t n = 0;
        if (!readU32(b, o, n))
        {
            return false;
        }
        if (o + n > b.size())
        {
            return false;
        }
        s.assign(reinterpret_cast<const char*>(b.data() + o), n);
        o += n;
        return true;
    }

    // Small helper: build a record's payload, then pack it as (opcode, size, payload)
    struct PayloadBuilder
    {
        std::vector<std::byte> payload;

        template <typename T> void put(const T& v)
        {
            appendLE(payload, v);
        }

        void putStr(const std::string& s)
        {
            appendStr(payload, s);
        }

        void putPad(size_t n)
        {
            payload.resize(payload.size() + n);
        }

        void clear()
        {
            payload.clear();
        }

        void flush(std::vector<std::byte>& out, EShaderInfoOp opcode)
        {
            appendU8(out, static_cast<uint8_t>(opcode));
            appendU32(out, static_cast<uint32_t>(payload.size()));
            if (!payload.empty())
            {
                out.insert(out.end(), payload.begin(), payload.end());
            }
            payload.clear();
        }
    };

    static void appendBlock(PayloadBuilder& output, const ShaderBlock& block)
    {
        output.put(block.size);
        output.put(block.alignment);
        output.put<std::uint8_t>(static_cast<std::uint8_t>(block.storage));
        output.put<std::uint8_t>(block.runtime_records ? 1u : 0u);
        output.put<std::uint32_t>(static_cast<std::uint32_t>(block.fields.size()));
        for (const auto& field : block.fields)
        {
            output.putStr(field.path);
            output.put<std::uint8_t>(static_cast<std::uint8_t>(field.scalar));
            output.put(field.offset);
            output.put(field.rows);
            output.put(field.columns);
            output.put(field.array_stride);
            output.put(field.matrix_stride);
        }
    }

    static bool readBlock(std::span<const std::byte> bytes, std::size_t& offset, ShaderBlock& output)
    {
        std::uint8_t storage = 0, records = 0;
        std::uint32_t count = 0;
        const bool has_header = readU32(bytes, offset, output.size) && readU32(bytes, offset, output.alignment) &&
                                readU8(bytes, offset, storage) && readU8(bytes, offset, records) &&
                                readU32(bytes, offset, count);
        if (!has_header || count > (bytes.size() - offset) / 25 || records > 1)
        {
            return false;
        }
        output.storage = static_cast<EDataLayout>(storage);
        output.runtime_records = records != 0;
        output.fields.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            ShaderMember field{};
            std::uint8_t scalar = 0;
            const bool has_field = readStr(bytes, offset, field.path) && readU8(bytes, offset, scalar) &&
                                   readU32(bytes, offset, field.offset) && readU32(bytes, offset, field.rows) &&
                                   readU32(bytes, offset, field.columns) &&
                                   readU32(bytes, offset, field.array_stride) &&
                                   readU32(bytes, offset, field.matrix_stride);
            if (!has_field)
            {
                return false;
            }
            field.scalar = static_cast<EDataScalar>(scalar);
            output.fields.push_back(std::move(field));
        }
        return true;
    }

    std::vector<std::byte> ShaderInfo::serialize(const ShaderInfo& info)
    {
        std::vector<std::byte> out;
        out.reserve(1024);
        PayloadBuilder pb;

        // entry points
        for (const auto& ep : info.entry_points)
        {
            pb.putStr(ep.name);
            pb.put<uint8_t>(static_cast<uint8_t>(ep.stage));
            pb.flush(out, EShaderInfoOp::ENTRY_POINT);
            const auto append = [&](const auto& variables, EShaderInfoOp operation)
            {
                for (const auto& variable : variables)
                {
                    pb.put<std::uint32_t>(static_cast<std::uint32_t>(&ep - info.entry_points.data()));
                    pb.put(variable.location);
                    pb.putStr(variable.name);
                    pb.put<std::uint8_t>(static_cast<std::uint8_t>(variable.base));
                    pb.put(variable.vec_size);
                    pb.put(variable.columns);
                    pb.put(variable.array_size);
                    pb.flush(out, operation);
                }
            };
            append(ep.inputs, EShaderInfoOp::ENTRY_INPUT);
            append(ep.outputs, EShaderInfoOp::ENTRY_OUTPUT);
        }

        // bindings
        for (const auto& set : info.sets)
        {
            for (const auto& b : set.bindings)
            {
                pb.put<uint32_t>(b.set);
                pb.put<uint32_t>(b.binding);
                pb.put<uint8_t>(static_cast<uint8_t>(b.type));
                pb.put<uint32_t>(b.count);
                pb.putStr(b.name);
                pb.put<uint32_t>(b.blockSize);
                pb.put<uint8_t>(b.writable ? 1u : 0u);
                pb.flush(out, EShaderInfoOp::BINDING);
                pb.put(b.set);
                pb.put(b.binding);
                pb.put(b.stages);
                pb.put<std::uint8_t>(b.readable ? 1u : 0u);
                appendBlock(pb, b.block);
                pb.flush(out, EShaderInfoOp::BINDING_LAYOUT);
                if (b.image_arrayed || b.image_depth)
                {
                    pb.put(b.set);
                    pb.put(b.binding);
                    pb.put<std::uint8_t>(b.image_arrayed);
                    pb.put<std::uint8_t>(b.image_depth);
                    pb.flush(out, EShaderInfoOp::IMAGE_SHAPE);
                }
            }
        }

        // push constants
        for (const auto& pc : info.push_constants)
        {
            pb.put<uint32_t>(pc.offset);
            pb.put<uint32_t>(pc.size);
            pb.flush(out, EShaderInfoOp::PUSH_CONST);
            pb.put(pc.offset);
            pb.put(pc.stages);
            appendBlock(pb, pc.block);
            pb.flush(out, EShaderInfoOp::PUSH_LAYOUT);
        }

        // spec constants
        for (const auto& sc : info.spec_constants)
        {
            pb.put<uint32_t>(sc.id);
            pb.put<uint32_t>(sc.constant_id);
            pb.putStr(sc.name);
            pb.put<uint8_t>(static_cast<uint8_t>(sc.default_value.kind));
            pb.put<uint32_t>(sc.default_value.bit_width);

            // Fixed 8-byte slot
            switch (sc.default_value.kind)
            {
            case SpecDefaultValue::EKind::BOOL:
            {
                pb.put<uint8_t>(sc.default_value.v.b8);
                pb.putPad(7);
            }
            break;
            case SpecDefaultValue::EKind::INT:
                pb.put<int64_t>(sc.default_value.v.i64);
                break;
            case SpecDefaultValue::EKind::UINT:
                pb.put<uint64_t>(sc.default_value.v.u64);
                break;
            case SpecDefaultValue::EKind::FLOAT:
            case SpecDefaultValue::EKind::DOUBLE:
                pb.put<double>(sc.default_value.v.f64);
                break;
            default:
                pb.put<uint64_t>(0);
                break;
            }
            pb.put<uint32_t>(sc.vec_size);
            pb.put<uint32_t>(sc.columns);
            pb.flush(out, EShaderInfoOp::SPEC_CONST);
        }

        // vertex inputs
        for (const auto& vi : info.vertex_inputs)
        {
            pb.put<uint32_t>(vi.location);
            pb.putStr(vi.name);
            pb.put<uint8_t>(static_cast<uint8_t>(vi.base));
            pb.put<uint32_t>(vi.vec_size);
            pb.put<uint32_t>(vi.columns);
            pb.put<uint32_t>(vi.array_size);
            pb.flush(out, EShaderInfoOp::VERTEX_INPUT);
        }

        // END
        appendU8(out, static_cast<uint8_t>(EShaderInfoOp::END));
        appendU32(out, 0u);

        out.shrink_to_fit();
        return out;
    }

    bool ShaderInfo::deserialize(std::span<const std::byte> buffer, ShaderInfo& result, std::string* err)
    {
        ShaderInfo out;
        auto fail = [&](const char* m)
        {
            if (err)
            {
                *err = m;
            }
            return false;
        };

        size_t off = 0;
        std::map<uint32_t, std::vector<EDescriptorBindingInfo>> setBuckets;

        while (true)
        {
            if (off + 1 + 4 > buffer.size())
            {
                return fail("truncated opcode/size");
            }
            uint8_t op = 0;
            uint32_t sz = 0;
            if (!readU8(buffer, off, op))
            {
                return fail("read opcode");
            }
            if (!readU32(buffer, off, sz))
            {
                return fail("read size");
            }

            if (op == static_cast<uint8_t>(EShaderInfoOp::END))
            {
                if (sz != 0)
                {
                    return fail("OP_END size must be 0");
                }
                break;
            }

            if (off + sz > buffer.size())
            {
                return fail("payload out of range");
            }

            size_t p = off, end = off + sz;
            auto left = [&] { return end - p; };
            const auto record = buffer.first(end);
            auto rdU8 = [&](uint8_t& v) { return readU8(record, p, v); };
            auto rdU32 = [&](uint32_t& v) { return readU32(record, p, v); };
            auto rdU64 = [&](uint64_t& v) { return readU64(record, p, v); };
            auto rdI64 = [&](int64_t& v) { return readI64(record, p, v); };
            auto rdF64 = [&](double& v) { return readF64(record, p, v); };
            auto rdStr = [&](std::string& s) { return readStr(record, p, s); };

            switch (op)
            {
            case static_cast<uint8_t>(EShaderInfoOp::ENTRY_POINT):
            {
                EntryPointInfo ep{};
                if (!rdStr(ep.name))
                {
                    return fail("ep.name");
                }
                uint8_t st = 0;
                if (!rdU8(st))
                {
                    return fail("ep.stage");
                }
                ep.stage = static_cast<EShaderType>(st);
                out.entry_points.push_back(std::move(ep));
            }
            break;

            case static_cast<uint8_t>(EShaderInfoOp::ENTRY_INPUT):
            case static_cast<uint8_t>(EShaderInfoOp::ENTRY_OUTPUT):
            {
                std::uint32_t entry = 0;
                std::uint8_t base = 0;
                ShaderIoVariable variable;
                if (!rdU32(entry) || entry >= out.entry_points.size() || !rdU32(variable.location) ||
                    !rdStr(variable.name) || !rdU8(base) ||
                    base > static_cast<std::uint8_t>(EVertexScalarBase::UNKNOWN) || !rdU32(variable.vec_size) ||
                    !rdU32(variable.columns) || !rdU32(variable.array_size))
                {
                    return fail("entry interface");
                }
                variable.base = static_cast<EVertexScalarBase>(base);
                auto& target = op == static_cast<std::uint8_t>(EShaderInfoOp::ENTRY_INPUT)
                                   ? out.entry_points[entry].inputs
                                   : out.entry_points[entry].outputs;
                target.push_back(std::move(variable));
            }
            break;

            case static_cast<uint8_t>(EShaderInfoOp::BINDING):
            {
                EDescriptorBindingInfo b{};
                if (!rdU32(b.set))
                {
                    return fail("binding.set");
                }
                if (!rdU32(b.binding))
                {
                    return fail("binding.binding");
                }
                uint8_t ty = 0;
                if (!rdU8(ty))
                {
                    return fail("binding.type");
                }
                b.type = static_cast<EDescriptorType>(ty);
                if (!rdU32(b.count))
                {
                    return fail("binding.count");
                }
                if (!rdStr(b.name))
                {
                    return fail("binding.name");
                }
                if (!rdU32(b.blockSize))
                {
                    return fail("binding.blockSize");
                }
                uint8_t wr = 0;
                if (!rdU8(wr))
                {
                    return fail("binding.writable");
                }
                b.writable = (wr != 0);
                setBuckets[b.set].push_back(std::move(b));
            }
            break;

            case static_cast<uint8_t>(EShaderInfoOp::PUSH_CONST):
            {
                PushConstantRangeInfo pc{};
                if (!rdU32(pc.offset))
                {
                    return fail("pc.offset");
                }
                if (!rdU32(pc.size))
                {
                    return fail("pc.size");
                }
                out.push_constants.push_back(pc);
            }
            break;

            case static_cast<std::uint8_t>(EShaderInfoOp::BINDING_LAYOUT):
            {
                std::uint32_t set = 0, binding = 0, stages = 0;
                std::uint8_t readable = 0;
                const bool has_header = rdU32(set) && rdU32(binding) && rdU32(stages) && rdU8(readable);
                if (!has_header || readable > 1)
                {
                    return fail("binding layout header");
                }
                const auto group = setBuckets.find(set);
                if (group == setBuckets.end())
                {
                    return fail("layout without binding");
                }
                const auto entry = std::find_if(
                    group->second.begin(),
                    group->second.end(),
                    [binding](const auto& value) { return value.binding == binding; }
                );
                if (entry == group->second.end() || !readBlock(buffer.first(end), p, entry->block) || p != end)
                {
                    return fail("binding layout");
                }
                entry->stages = stages;
                entry->readable = readable != 0;
            }
            break;

            case static_cast<std::uint8_t>(EShaderInfoOp::IMAGE_SHAPE):
            {
                std::uint32_t set = 0, binding = 0;
                std::uint8_t arrayed = 0, depth = 0;
                if (!rdU32(set) || !rdU32(binding) || !rdU8(arrayed) || !rdU8(depth) || arrayed > 1 || depth > 1)
                {
                    return fail("image shape");
                }
                auto group = setBuckets.find(set);
                if (group == setBuckets.end())
                {
                    return fail("image without binding");
                }
                auto entry = std::ranges::find(group->second, binding, &EDescriptorBindingInfo::binding);
                if (entry == group->second.end())
                {
                    return fail("image binding");
                }
                entry->image_arrayed = arrayed != 0;
                entry->image_depth = depth != 0;
            }
            break;

            case static_cast<std::uint8_t>(EShaderInfoOp::PUSH_LAYOUT):
            {
                std::uint32_t offset = 0, stages = 0;
                if (!rdU32(offset) || !rdU32(stages))
                {
                    return fail("push layout header");
                }
                const auto range = std::find_if(
                    out.push_constants.begin(),
                    out.push_constants.end(),
                    [offset](const auto& value) { return value.offset == offset; }
                );
                if (range == out.push_constants.end() || !readBlock(buffer.first(end), p, range->block) || p != end)
                {
                    return fail("push layout");
                }
                range->stages = stages;
            }
            break;

            case static_cast<uint8_t>(EShaderInfoOp::SPEC_CONST):
            {
                SpecConstantInfo sc{};
                if (!rdU32(sc.id))
                {
                    return fail("sc.id");
                }
                if (!rdU32(sc.constant_id))
                {
                    return fail("sc.constant_id");
                }
                if (!rdStr(sc.name))
                {
                    return fail("sc.name");
                }

                uint8_t kind = 0;
                if (!rdU8(kind))
                {
                    return fail("sc.kind");
                }
                sc.default_value.kind = static_cast<SpecDefaultValue::EKind>(kind);

                if (!rdU32(sc.default_value.bit_width))
                {
                    return fail("sc.bit_width");
                }

                switch (sc.default_value.kind)
                {
                case SpecDefaultValue::EKind::BOOL:
                {
                    uint8_t b = 0;
                    if (!rdU8(b))
                    {
                        return fail("sc.bool");
                    }
                    sc.default_value.v.b8 = b;
                    if (left() < 7)
                    {
                        return fail("sc.bool pad");
                    }
                    p += 7; // skip pad
                }
                break;
                case SpecDefaultValue::EKind::INT:
                {
                    int64_t v = 0;
                    if (!rdI64(v))
                    {
                        return fail("sc.i64");
                    }
                    sc.default_value.v.i64 = v;
                }
                break;
                case SpecDefaultValue::EKind::UINT:
                {
                    uint64_t v = 0;
                    if (!rdU64(v))
                    {
                        return fail("sc.u64");
                    }
                    sc.default_value.v.u64 = v;
                }
                break;
                case SpecDefaultValue::EKind::FLOAT:
                case SpecDefaultValue::EKind::DOUBLE:
                {
                    double v = 0;
                    if (!rdF64(v))
                    {
                        return fail("sc.f64");
                    }
                    sc.default_value.v.f64 = v;
                }
                break;
                default:
                {
                    if (left() < 8)
                    {
                        return fail("sc.unknown payload");
                    }
                    p += 8; // skip
                }
                break;
                }

                if (!rdU32(sc.vec_size))
                {
                    return fail("sc.vec_size");
                }
                if (!rdU32(sc.columns))
                {
                    return fail("sc.columns");
                }

                out.spec_constants.push_back(std::move(sc));
            }
            break;

            case static_cast<uint8_t>(EShaderInfoOp::VERTEX_INPUT):
            {
                ShaderIoVariable vi{};
                if (!rdU32(vi.location))
                {
                    return fail("vi.location");
                }
                if (!rdStr(vi.name))
                {
                    return fail("vi.name");
                }
                uint8_t base = 0;
                if (!rdU8(base))
                {
                    return fail("vi.base");
                }
                vi.base = static_cast<EVertexScalarBase>(base);
                if (!rdU32(vi.vec_size))
                {
                    return fail("vi.vec_size");
                }
                if (!rdU32(vi.columns))
                {
                    return fail("vi.columns");
                }
                if (!rdU32(vi.array_size))
                {
                    return fail("vi.array_size");
                }
                out.vertex_inputs.push_back(std::move(vi));
            }
            break;

            default:
                // Forward compatibility: skip unknown records
                break;
            }

            // Advance past this record's payload
            off = end;
        }

        // Merge setBuckets => ShaderInfo.sets
        out.sets.clear();
        out.sets.reserve(setBuckets.size());
        for (auto& kv : setBuckets)
        {
            DescriptorSetLayoutInfo s{};
            s.set = kv.first;
            s.bindings = std::move(kv.second);
            out.sets.push_back(std::move(s));
        }
        std::sort(out.sets.begin(), out.sets.end(), [](const auto& a, const auto& b) { return a.set < b.set; });

        result = std::move(out);
        return true;
    }

} // namespace lux::rdesc
