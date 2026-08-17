#include <lux/engine/description/ShaderInfo.hpp>
#include <cstring>
#include <map>
#include <span>
#include <algorithm>

namespace lux::rdesc
{
	enum : uint8_t {
		OP_ENTRY_POINT = 0x01,
		OP_BINDING = 0x02,
		OP_PUSH_CONST = 0x03,
		OP_SPEC_CONST = 0x04,
		OP_VERTEX_INPUT = 0x05,
		OP_END = 0xFF
	};

	// ================= Basic buffer I/O (little-endian) =================
	template<typename T>
	static inline void appendLE(std::vector<std::byte>& out, const T& v) {
		static_assert(std::is_trivially_copyable<T>::value, "POD only");
		const auto* p = reinterpret_cast<const std::byte*>(&v);
		out.insert(out.end(), p, p + sizeof(T));
	}

	static inline void appendBytes(std::vector<std::byte>& out, const void* p, size_t n) {
		out.insert(out.end(), reinterpret_cast<const std::byte*>(p),
			reinterpret_cast<const std::byte*>(p) + n);
	}

	static inline void appendU8(std::vector<std::byte>& out, uint8_t  v)  { appendLE(out, v); }
	static inline void appendU32(std::vector<std::byte>& out, uint32_t v) { appendLE(out, v); }
	[[maybe_unused]] static inline void appendU64(std::vector<std::byte>& out, uint64_t v) { appendLE(out, v); }
	[[maybe_unused]] static inline void appendI64(std::vector<std::byte>& out, int64_t  v) { appendLE(out, v); }
	[[maybe_unused]] static inline void appendF64(std::vector<std::byte>& out, double   v) { appendLE(out, v); }

	static inline void appendStr(std::vector<std::byte>& out, const std::string& s) {
		appendU32(out, static_cast<uint32_t>(s.size()));
		if (!s.empty())
		{
			appendBytes(out, s.data(), s.size());
		}
	}

	template<typename T>
	static inline bool readLE(std::span<const std::byte> buf, size_t& off, T& v) {
		static_assert(std::is_trivially_copyable<T>::value, "POD only");
		if (off + sizeof(T) > buf.size())
		{
			return false;
		}
		std::memcpy(&v, buf.data() + off, sizeof(T));
		off += sizeof(T);
		return true;
	}

	static inline bool readBytes(std::span<const std::byte> buf, size_t& off, void* dst, size_t n) {
		if (off + n > buf.size())
		{
			return false;
		}
		std::memcpy(dst, buf.data() + off, n);
		off += n;
		return true;
	}

	static inline bool readU8(std::span<const std::byte> b, size_t& o, uint8_t& v) { return readLE(b, o, v); }
	static inline bool readU32(std::span<const std::byte> b, size_t& o, uint32_t& v) { return readLE(b, o, v); }
	static inline bool readU64(std::span<const std::byte> b, size_t& o, uint64_t& v) { return readLE(b, o, v); }
	static inline bool readI64(std::span<const std::byte> b, size_t& o, int64_t& v) { return readLE(b, o, v); }
	static inline bool readF64(std::span<const std::byte> b, size_t& o, double& v) { return readLE(b, o, v); }

	static inline bool readStr(std::span<const std::byte> b, size_t& o, std::string& s) {
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
	struct PayloadBuilder {
		std::vector<std::byte> payload;
		template<typename T> void put(const T& v) { appendLE(payload, v); }
		void putStr(const std::string& s) { appendStr(payload, s); }
		void putPad(size_t n) { payload.resize(payload.size() + n); }
		void clear() { payload.clear(); }
		void flush(std::vector<std::byte>& out, uint8_t opcode) {
			appendU8(out, opcode);
			appendU32(out, static_cast<uint32_t>(payload.size()));
			if (!payload.empty())
			{
				out.insert(out.end(), payload.begin(), payload.end());
			}
			payload.clear();
		}
	};

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
			pb.flush(out, OP_ENTRY_POINT);
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
				pb.flush(out, OP_BINDING);
			}
		}

		// push constants
		for (const auto& pc : info.push_constants)
		{
			pb.put<uint32_t>(pc.offset);
			pb.put<uint32_t>(pc.size);
			pb.flush(out, OP_PUSH_CONST);
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
			switch (sc.default_value.kind) {
			case SpecDefaultValue::Kind::Bool: {
				pb.put<uint8_t>(sc.default_value.v.b8);
				pb.putPad(7);
			} break;
			case SpecDefaultValue::Kind::Int:
				pb.put<int64_t>(sc.default_value.v.i64); break;
			case SpecDefaultValue::Kind::UInt:
				pb.put<uint64_t>(sc.default_value.v.u64); break;
			case SpecDefaultValue::Kind::Float:
			case SpecDefaultValue::Kind::Double:
				pb.put<double>(sc.default_value.v.f64); break;
			default: pb.put<uint64_t>(0); break;
			}
			pb.put<uint32_t>(sc.vec_size);
			pb.put<uint32_t>(sc.columns);
			pb.flush(out, OP_SPEC_CONST);
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
			pb.flush(out, OP_VERTEX_INPUT);
		}

		// END
		appendU8(out, OP_END);
		appendU32(out, 0u);

		out.shrink_to_fit();
		return out;
	}

	bool ShaderInfo::deserialize(std::span<const std::byte> buffer, ShaderInfo& out, std::string* err)
	{
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

			if (op == OP_END)
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
			auto rdU8 = [&](uint8_t& v) { return readU8(buffer, p, v); };
			auto rdU32 = [&](uint32_t& v) { return readU32(buffer, p, v); };
			auto rdU64 = [&](uint64_t& v) { return readU64(buffer, p, v); };
			auto rdI64 = [&](int64_t& v) { return readI64(buffer, p, v); };
			auto rdF64 = [&](double& v) { return readF64(buffer, p, v); };
			auto rdStr = [&](std::string& s) { return readStr(buffer, p, s); };

			switch (op)
			{
			case OP_ENTRY_POINT: {
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
			} break;

			case OP_BINDING: {
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
			} break;

			case OP_PUSH_CONST: {
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
			} break;

			case OP_SPEC_CONST: {
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
				sc.default_value.kind = static_cast<SpecDefaultValue::Kind>(kind);

				if (!rdU32(sc.default_value.bit_width))
				{
					return fail("sc.bit_width");
				}

				switch (sc.default_value.kind) {
				case SpecDefaultValue::Kind::Bool: {
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
				} break;
				case SpecDefaultValue::Kind::Int: {
					int64_t v = 0;
					if (!rdI64(v))
					{
						return fail("sc.i64");
					}
					sc.default_value.v.i64 = v;
				} break;
				case SpecDefaultValue::Kind::UInt: {
					uint64_t v = 0;
					if (!rdU64(v))
					{
						return fail("sc.u64");
					}
					sc.default_value.v.u64 = v;
				} break;
				case SpecDefaultValue::Kind::Float:
				case SpecDefaultValue::Kind::Double: {
					double v = 0;
					if (!rdF64(v))
					{
						return fail("sc.f64");
					}
					sc.default_value.v.f64 = v;
				} break;
				default: {
					if (left() < 8)
					{
						return fail("sc.unknown payload");
					}
					p += 8; // skip
				} break;
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
			} break;

			case OP_VERTEX_INPUT: {
				VertexInputAttribute vi{};
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
				vi.base = static_cast<VertexScalarBase>(base);
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
			} break;

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
		std::sort(out.sets.begin(), out.sets.end(),
			[](const auto& a, const auto& b) { return a.set < b.set; }
		);

		return true;
	}

} // namespace lux::rdesc
