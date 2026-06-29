#include <lux/engine/asset/ShaderSerDeser.hpp>
#include <AssetManagerImpl.hpp>

#include <spirv_cross/spirv_cross.hpp>
#include <cassert>
#include <array>
#include <cstring>
#include <map>
#include <algorithm>

namespace lux::asset
{
	ShaderSerDeser::ShaderSerDeser(std::shared_ptr<AssetManager> manager)
		: TAssetSerDeser<ShaderLoadConfig>(std::move(manager)) {
	}

	// 从路径名根据配置猜阶段
	static inline std::optional<EShaderType>
	infer_stage_from_name(const std::filesystem::path& name, const ShaderLoadConfig& cfg)
	{
		const auto s = name.string();
		for (const auto& elem : cfg.name_hints) {
			const auto& [key, st] = elem;
			if (s.find(key) != std::string::npos) return st;
		}
		return std::nullopt;
	}

	lux::cxx::expected<AssetIDPair, EAssetError> ShaderSerDeser::importFromFile(const std::filesystem::path& p)
	{
		using lux::cxx::unexpected;

		// 1) 打开文件
		if (!std::filesystem::exists(p))
			return unexpected(EAssetError::FILE_NOT_EXIST);

		std::ifstream ifs(p, std::ios::binary);
		if (!ifs)
		{
			return unexpected(EAssetError::FILE_OPEN_FAIL);
		}

		// 2) 阶段推断（显式配置优先，否则文件名推断）
		auto& config = TAssetSerDeser::config();

		if (!config.explicit_stage) {
			if (auto st = infer_stage_from_name(p, config)) {
				config.explicit_stage = st;
			}
			else {
				return unexpected(EAssetError::FILE_TYPE_ERROR);
			}
		}

		// 3) 调用 fromFile(ifs) 完成构建
		auto exp = this->fromFileStream(ifs);
		if (!exp) {
			return unexpected(exp.error());
		}

		// 4) 注册到 manager（与你的基类流程一致）
		const auto& asset_id = exp.value()->id();
		auto* asset_ptr = exp.value().get();

		if (!submitAsset(std::move(exp.value())))
			return unexpected(EAssetError::ASSET_ALREADY_EXIST);

		return AssetIDPair{ asset_ptr, asset_id };
	}

	EAssetError ShaderSerDeser::loadFromSpriv(const std::filesystem::path& path, Shader& shader)
	{
		// RAII throughout (std::ifstream + std::vector), and the read is
		// length-checked — the old FILE*+malloc path ignored fread's result
		// and mishandled ftell's signed result through a size_t.
		std::error_code ec;
		const auto bytes = std::filesystem::file_size(path, ec);
		if (ec)         return EAssetError::FILE_NOT_EXIST;
		if (bytes == 0) return EAssetError::ABNORMAL_FILE_SIZE;

		std::ifstream ifs(path, std::ios::binary);
		if (!ifs) return EAssetError::FILE_NOT_EXIST;

		std::vector<std::byte> buf(static_cast<size_t>(bytes));
		if (!ifs.read(reinterpret_cast<char*>(buf.data()),
		              static_cast<std::streamsize>(buf.size())))
			return EAssetError::READ_FILE_FAIL;

		shader = Shader(std::move(buf));
		return EAssetError::SUCCESS;
	}

	static inline bool is_spirv_magic(const std::byte* p, size_t n) {
		if (n < 4)
		{
			return false;
		}
		uint32_t m = 0;
		std::memcpy(&m, p, 4);
		return m == 0x07230203u;
	}

	lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
	ShaderSerDeser::fromFileStream(std::ifstream& ifs)
	{
		using lux::cxx::unexpected;

		// 读全文件
		ifs.seekg(0, std::ios::end);
		const auto n = ifs.tellg();
		if (n <= 0) 
			return unexpected(EAssetError::ABNORMAL_FILE_SIZE);
		ifs.seekg(0, std::ios::beg);

		std::vector<std::byte> buf(static_cast<size_t>(n));
		auto& ifs_ = ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
		if (!ifs_)
		{
			return unexpected(EAssetError::READ_FILE_FAIL);
		}

		// 判定 SPIR-V
		if (!is_spirv_magic(buf.data(), buf.size()))
			return unexpected(EAssetError::FILE_TYPE_ERROR);

		// 组装通用 info（此时还没有 id，这通常由注册时决定；也可以在此生成）
		auto ainfo = manager().createAssetInfo(EAssetType::SHADER);
		ShaderInfo sinfo;
		bool ok = reflect(buf.data(), buf.size(), sinfo);
		if (!ok) {
			return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
		}

		// 构造 ShaderAsset，并把 SPIR-V 字节直接移交（buf 即 payload，无需再拷贝）
		auto uptr = std::make_unique<ShaderAsset>(std::move(ainfo), std::move(sinfo));

		uptr->assignData(std::move(buf));

		return uptr;
	}

	lux::cxx::expected<std::unique_ptr<Shader>, EAssetError>
	ShaderSerDeser::decodeData(const void* bytes, std::size_t len) noexcept
	{
		using lux::cxx::unexpected;

		if (bytes == nullptr || len == 0) {
			return unexpected(EAssetError::ABNORMAL_FILE_SIZE);
		}

		// noexcept contract: the only throwing operation below is the payload
		// vector allocation (loadHeaderRaw is memcpy-only; ShaderInfo decode is
		// NOT touched — pure data needs no reflection metadata). Wrap the lot in
		// a try/catch so a bad_alloc surfaces as an error code, never a throw.
		try
		{
			// loadHeaderRaw consumes a std::vector<std::byte>; a header-sized
			// view is all it reads (it never indexes past sizeof(header)). Copy
			// just the prefix instead of the whole image to keep this cheap.
			const auto* base = static_cast<const std::byte*>(bytes);

			std::vector<std::byte> header_view(
				base,
				base + std::min(len, sizeof(AssetFileHeader))
			);

			AssetFileHeader header{};
			if (auto ec = loadHeaderRaw<EAssetType::SHADER>(header_view, header);
				ec != EAssetError::SUCCESS) {
				return unexpected(ec);
			}

			// Locate the data segment and bounds-check it against the real image
			// length (header_view was a truncated prefix, so validate vs `len`).
			const size_t off = static_cast<size_t>(header.data_offset);
			const size_t dlen = static_cast<size_t>(header.data_size);
			if (off > len || len - off < dlen) {
				return unexpected(EAssetError::ABNORMAL_FILE_SIZE); // 防 size_t 溢出
			}

			// Slice the SPIR-V payload into owned storage and hand it to Shader.
			std::vector<std::byte> payload(base + off, base + off + dlen);
			return std::make_unique<Shader>(std::move(payload));
		}
		catch (...)
		{
			return unexpected(EAssetError::OUT_OF_MEMORY);
		}
	}

	lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
	ShaderSerDeser::fromLuxAssetStream(std::istream& ifs)
	{
		using lux::cxx::unexpected;

		// 1) 读全文件到内存，减少系统调用
		ifs.seekg(0, std::ios::end);
		const std::streamoff endpos = ifs.tellg();
		if (endpos <= 0) {
			return unexpected(EAssetError::ABNORMAL_FILE_SIZE);
		}
		ifs.seekg(0, std::ios::beg);

		std::vector<std::byte> file(static_cast<size_t>(endpos));
		if (!ifs.read(reinterpret_cast<char*>(file.data()),
			static_cast<std::streamsize>(file.size()))) {
			return unexpected(EAssetError::READ_FILE_FAIL);
		}

		// 2) 解析并校验头 + ShaderAssetInfo
		AssetFileHeader  header{};
		ShaderAssetInfo  sinfo{};
		if (auto ec = loadHeaderRaw<EAssetType::SHADER>(file, header);
			ec != EAssetError::SUCCESS) {
			return unexpected(ec);
		}

		// 3) 解析 shader info， 切出
		size_t off = static_cast<size_t>(header.info_offset);
		size_t len = static_cast<size_t>(header.info_size);

		if (off > file.size() || file.size() - off < len) {
			return unexpected(EAssetError::ABNORMAL_FILE_SIZE); // 防止 size_t 溢出
		}

		std::vector<std::byte> shader_info_buf;
		std::string err;
		shader_info_buf.resize(len);
		std::memcpy(shader_info_buf.data(), file.data() + off, len);
		if (!ShaderInfo::deserialize(shader_info_buf, sinfo, &err))
		{
			return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
		}

		// 4) 切出 payload（SPIR-V 原始字节）→ Shader：复用 decodeData，避免两份解码逻辑
		auto data = decodeData(file.data(), file.size());
		if (!data) {
			return unexpected(data.error());
		}

		// 5) 组装 AssetInfo（头里已经有通用信息）
		auto ainfo = std::make_unique<AssetInfo>(header.info);

		// 6) 构造 ShaderAsset，并把 Shader 放进去
		auto uptr = std::make_unique<ShaderAsset>(std::move(ainfo), std::move(sinfo));

		uptr->setData(std::move(data.value()));

		return uptr;
	}

	EAssetError ShaderSerDeser::exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& of)
	{
		auto shader_asset = asset.as<ShaderAsset>();
		auto shader_data  = shader_asset->data();
		if (!shader_data)
		{
			return EAssetError::ASSET_NO_DATA;
		}
		
		std::vector<std::byte> des_data = ShaderInfo::serialize(shader_asset->shaderInfo());
		if(des_data.empty())
		{
			return EAssetError::ASSET_NO_INFO;
		}

		auto header_data = makeHeaderRaw<EAssetType::SHADER>(
			*shader_asset->info(), des_data.size(), shader_data->size()
		);

		of.write(reinterpret_cast<const char*>(header_data.data()), header_data.size());
		if (!of) return EAssetError::WRITE_FILE_FAIL;
		of.write(reinterpret_cast<const char*>(des_data.data()), des_data.size());
		if (!of) return EAssetError::WRITE_FILE_FAIL;
		of.write(reinterpret_cast<const char*>(shader_data->data()), shader_data->size());
		if (!of) return EAssetError::WRITE_FILE_FAIL;

		of.flush();
		if (!of) return EAssetError::WRITE_FILE_FAIL;

		return EAssetError::SUCCESS;
	}

	namespace sc = SPIRV_CROSS_NAMESPACE;
	// 辅助：vertex 基类型映射
	static VertexScalarBase toVertexBase(const sc::SPIRType& t) {
		using BT = sc::SPIRType::BaseType;
		switch (t.basetype) {
		case BT::Boolean: return VertexScalarBase::Bool;
		case BT::Int:     return VertexScalarBase::Int;
		case BT::UInt:    return VertexScalarBase::UInt;
		case BT::Half:
		case BT::Float:   return VertexScalarBase::Float;
		case BT::Double:  return VertexScalarBase::Double;
		default:          return VertexScalarBase::Unknown;
		}
	}
	static uint32_t calcArraySize(const sc::Compiler& comp, const sc::SPIRType& t) {
		if (t.array.empty()) return 1;
		uint64_t total = 1;
		for (size_t i = 0; i < t.array.size(); ++i) {
			if (t.array_size_literal[i]) total *= t.array[i];
			else return 1;
		}
		return static_cast<uint32_t>(total);
	}

	static bool isWritableStorage(const sc::Compiler& comp, uint32_t var_id) {
		auto bits = comp.get_decoration_bitset(var_id);
		return !bits.get(spv::DecorationNonWritable);
	}

	static void pushBinding(ShaderInfo& out, const EDescriptorBindingInfo& b) {
		size_t idx = out.findSet(b.set);
		if (idx == ShaderInfo::npos) {
			out.sets.push_back(DescriptorSetLayoutInfo{ b.set, {} });
			idx = out.sets.size() - 1;
		}
		out.sets[idx].bindings.push_back(b);
	}

	static std::vector<EntryPointInfo> detectEntry(sc::Compiler& comp)
	{
		std::vector<EntryPointInfo> out;
		auto entries = comp.get_entry_points_and_stages();
		if (entries.empty()) {
			return {};
		}

		for (auto& entry : entries)
		{
			EntryPointInfo epi{};
			epi.name = entry.name;
			switch (entry.execution_model) {
				case spv::ExecutionModelVertex:                 epi.stage = EShaderType::VERTEX; break; 
				case spv::ExecutionModelFragment:               epi.stage = EShaderType::FRAGMENT; break;
				case spv::ExecutionModelGLCompute:              epi.stage = EShaderType::COMPUTE; break;
				case spv::ExecutionModelGeometry:               epi.stage = EShaderType::GEOMETRY; break;
				case spv::ExecutionModelTessellationControl:    epi.stage = EShaderType::TESSELLATION_CTRL; break;
				case spv::ExecutionModelTessellationEvaluation: epi.stage = EShaderType::TESSELLATION_EVAL; break;
				// case spv::ExecutionModelRayGenerationKHR:       return EShaderType::RAYGEN;
				// case spv::ExecutionModelAnyHitKHR:              return EShaderType::ANY_HIT;
				// case spv::ExecutionModelClosestHitKHR:          return EShaderType::CLOSEST_HIT;
				// case spv::ExecutionModelMissKHR:                return EShaderType::MISS;
				// case spv::ExecutionModelIntersectionKHR:        return EShaderType::INTERSECTION;
				// case spv::ExecutionModelCallableKHR:            return EShaderType::CALLABLE;
				// case spv::ExecutionModelTaskEXT:                return EShaderType::TASK;
				// case spv::ExecutionModelMeshEXT:                return EShaderType::MESH;
				default: epi.stage = EShaderType::UNDEFINED;
			}
			out.push_back(epi);
		}
		return out;
	}

	bool ShaderSerDeser::reflect(const void* bytes, size_t byte_count, ShaderInfo& out)
	{
		const uint32_t* words = reinterpret_cast<const uint32_t*>(bytes);
		size_t word_count = byte_count / sizeof(uint32_t);

		sc::Compiler comp(words, word_count);
		const auto res = comp.get_shader_resources();

		// entry points
		out.entry_points = detectEntry(comp);

		// UBO
		for (auto& r : res.uniform_buffers) {
			auto set = comp.get_decoration(r.id, spv::DecorationDescriptorSet);
			auto binding = comp.get_decoration(r.id, spv::DecorationBinding);
			const auto& t = comp.get_type(r.base_type_id);

			EDescriptorBindingInfo b{};
			b.set = set;
			b.binding = binding;
			b.name = comp.get_name(r.id);
			b.type = EDescriptorType::UNIFORM_BUFFER;
			b.count = calcArraySize(comp, t);
			b.blockSize = static_cast<uint32_t>(comp.get_declared_struct_size(t));
			b.writable = false;
			pushBinding(out, b);
		}

		// SSBO
		for (auto& r : res.storage_buffers) {
			auto set = comp.get_decoration(r.id, spv::DecorationDescriptorSet);
			auto binding = comp.get_decoration(r.id, spv::DecorationBinding);
			const auto& t = comp.get_type(r.base_type_id);

			EDescriptorBindingInfo b{};
			b.set = set;
			b.binding = binding;
			b.name = comp.get_name(r.id);
			b.type = EDescriptorType::STORAGE_BUFFER;
			b.count = calcArraySize(comp, t);
			b.blockSize = static_cast<uint32_t>(comp.get_declared_struct_size(t));
			b.writable = isWritableStorage(comp, r.id);
			pushBinding(out, b);
		}

		// Combined image samplers
		for (auto& r : res.sampled_images) {
			auto set = comp.get_decoration(r.id, spv::DecorationDescriptorSet);
			auto binding = comp.get_decoration(r.id, spv::DecorationBinding);
			const auto& t = comp.get_type(r.base_type_id);

			EDescriptorBindingInfo b{};
			b.set = set;
			b.binding = binding;
			b.name = comp.get_name(r.id);
			b.type = EDescriptorType::COMBINED_IMAGE_SAMPLER;
			b.count = calcArraySize(comp, t);
			pushBinding(out, b);
		}

		// Separate images / samplers
		for (auto& r : res.separate_images) {
			auto set = comp.get_decoration(r.id, spv::DecorationDescriptorSet);
			auto binding = comp.get_decoration(r.id, spv::DecorationBinding);
			const auto& t = comp.get_type(r.base_type_id);
			EDescriptorBindingInfo b{};
			b.set = set; b.binding = binding; b.name = comp.get_name(r.id);
			b.type = EDescriptorType::SAMPLED_IMAGE; b.count = calcArraySize(comp, t);
			pushBinding(out, b);
		}
		for (auto& r : res.separate_samplers) {
			auto set = comp.get_decoration(r.id, spv::DecorationDescriptorSet);
			auto binding = comp.get_decoration(r.id, spv::DecorationBinding);
			EDescriptorBindingInfo b{};
			b.set = set; b.binding = binding; b.name = comp.get_name(r.id);
			b.type = EDescriptorType::SAMPLER; b.count = 1;
			pushBinding(out, b);
		}

		// Storage images
		for (auto& r : res.storage_images) {
			auto set = comp.get_decoration(r.id, spv::DecorationDescriptorSet);
			auto binding = comp.get_decoration(r.id, spv::DecorationBinding);
			const auto& t = comp.get_type(r.base_type_id);
			EDescriptorBindingInfo b{};
			b.set = set; b.binding = binding; b.name = comp.get_name(r.id);
			b.type = EDescriptorType::STORAGE_IMAGE; b.count = calcArraySize(comp, t);
			b.writable = isWritableStorage(comp, r.id);
			pushBinding(out, b);
		}

		// Input attachments
		for (auto& r : res.subpass_inputs) {
			auto set = comp.get_decoration(r.id, spv::DecorationDescriptorSet);
			auto binding = comp.get_decoration(r.id, spv::DecorationBinding);
			const auto& t = comp.get_type(r.base_type_id);
			EDescriptorBindingInfo b{};
			b.set = set; b.binding = binding; b.name = comp.get_name(r.id);
			b.type = EDescriptorType::INPUT_ATTACHMENT; b.count = calcArraySize(comp, t);
			pushBinding(out, b);
		}

		// Acceleration structures
		for (auto& r : res.acceleration_structures) {
			auto set = comp.get_decoration(r.id, spv::DecorationDescriptorSet);
			auto binding = comp.get_decoration(r.id, spv::DecorationBinding);
			EDescriptorBindingInfo b{};
			b.set = set; b.binding = binding; b.name = comp.get_name(r.id);
			b.type = EDescriptorType::ACCELERATION_STRUCTURE; b.count = 1;
			pushBinding(out, b);
		}

		// Push constants
		for (auto& pc : res.push_constant_buffers) {
			const auto& t = comp.get_type(pc.base_type_id);
			PushConstantRangeInfo rng{};
			rng.offset = 0;
			rng.size = static_cast<uint32_t>(comp.get_declared_struct_size(t));
			out.push_constants.push_back(rng);
		}

		// Specialization constants（修正：通过 SPIRConstant）
		for (auto& sc_info : comp.get_specialization_constants()) {
			SpecConstantInfo s{};
			s.id = sc_info.id;
			s.constant_id = sc_info.constant_id;
			s.name = comp.get_name(sc_info.id);

			const auto& c = comp.get_constant(sc_info.id);
			const auto& t = comp.get_type(c.constant_type);

			s.vec_size = c.vector_size();
			s.columns = c.columns();

			using BT = sc::SPIRType::BaseType;
			auto& dv = s.default_value;
			dv.bit_width = t.width;

			switch (t.basetype) {
			case BT::Boolean:
				dv.kind = SpecDefaultValue::Kind::Bool;
				dv.v.b8 = (c.scalar_u64() != 0) ? 1 : 0;
				break;
			case BT::Int:
				dv.kind = SpecDefaultValue::Kind::Int;
				if (t.width == 64)      dv.v.i64 = c.scalar_i64();
				else if (t.width == 16) dv.v.i64 = c.scalar_i16();
				else if (t.width == 8)  dv.v.i64 = c.scalar_i8();
				else                    dv.v.i64 = c.scalar_i32();
				break;
			case BT::UInt:
				dv.kind = SpecDefaultValue::Kind::UInt;
				if (t.width == 64)      dv.v.u64 = c.scalar_u64();
				else if (t.width == 16) dv.v.u64 = static_cast<uint16_t>(c.scalar_u16());
				else if (t.width == 8)  dv.v.u64 = static_cast<uint8_t>(c.scalar_u8());
				else                    dv.v.u64 = static_cast<uint32_t>(c.scalar());
				break;
			case BT::Half:
				dv.kind = SpecDefaultValue::Kind::Float;
				dv.v.f64 = static_cast<double>(c.scalar_f16());
				break;
			case BT::Float:
				dv.kind = SpecDefaultValue::Kind::Float;
				dv.v.f64 = static_cast<double>(c.scalar_f32());
				break;
			case BT::Double:
				dv.kind = SpecDefaultValue::Kind::Double;
				dv.v.f64 = c.scalar_f64();
				break;
			default:
				dv.kind = SpecDefaultValue::Kind::Unknown;
				dv.v.u64 = 0;
				break;
			}

			out.spec_constants.push_back(std::move(s));
		}

		// 先检查是否有 vertex 入口
		bool has_vertex = false;
		std::string vertex_entry;
		for (auto& ep : out.entry_points) {
			if (ep.stage == EShaderType::VERTEX) {
				has_vertex = true;
				// 优先找名为 "main" 的入口，找不到就用第一个
				if (vertex_entry.empty() || ep.name == "main")
					vertex_entry = ep.name;
			}
		}

		if (has_vertex) {
			// 为了避免多入口点下拿错接口，针对顶点入口单独取一次 resources
			sc::Compiler comp_vi(words, word_count);
			if (!vertex_entry.empty())
				comp_vi.set_entry_point(vertex_entry, spv::ExecutionModelVertex);

			const auto res_vi = comp_vi.get_shader_resources();

			for (auto& a : res_vi.stage_inputs) {
				// 跳过内建变量（比如 gl_VertexIndex / gl_InstanceIndex 等）
				if (comp_vi.has_decoration(a.id, spv::DecorationBuiltIn))
					continue;

				VertexInputAttribute vi{};
				// 位置（必需）
				vi.location = comp_vi.get_decoration(a.id, spv::DecorationLocation);
				// 名字（可读性）
				vi.name = comp_vi.get_name(a.id);

				// 类型信息
				const auto& t = comp_vi.get_type(a.base_type_id);
				vi.base = toVertexBase(t);   // Bool / Int / UInt / Float / Double / Unknown
				vi.vec_size = t.vecsize;         // 1,2,3,4
				vi.columns = t.columns;         // 矩阵列数，标量/向量为 1
				vi.array_size = t.array.empty() ? 1u
					: static_cast<uint32_t>(t.array[0]); // 只取最外层

				out.vertex_inputs.push_back(std::move(vi));
			}

			// 按 location 升序，便于上层生成 VkVertexInputAttributeDescription
			std::sort(out.vertex_inputs.begin(), out.vertex_inputs.end(),
				[](const auto& lhs, const auto& rhs) {
					return lhs.location < rhs.location;
				}
			);
		}

		return true;
	}
}