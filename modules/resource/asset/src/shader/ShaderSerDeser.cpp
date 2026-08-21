#include <lux/engine/resource/asset/shader/ShaderSerDeser.hpp>
#include <lux/engine/resource/asset/detail/AssetManagerImpl.hpp>

#include <cstring>
#include <span>

namespace lux::asset
{
	ShaderSerDeser::ShaderSerDeser(std::shared_ptr<AssetManager> manager)
		: TAssetSerDeser<ShaderLoadConfig>(std::move(manager)) {
	}

	lux::cxx::expected<std::unique_ptr<lux::rdesc::Shader>, EAssetError>
	ShaderSerDeser::decodeData(const void* bytes, std::size_t len) noexcept
	{
		using lux::cxx::unexpected;

		if (bytes == nullptr || len == 0) {
			return unexpected(EAssetError::ABNORMAL_FILE_SIZE);
		}

			const auto* base = static_cast<const std::byte*>(bytes);
			const auto file = std::span<const std::byte>{base, len};

			AssetFileHeader header{};
			if (auto ec = loadHeaderRaw<EAssetType::SHADER>(file, header);
				ec != EAssetError::SUCCESS) {
				return unexpected(ec);
			}

			// Locate the data segment and bounds-check it against the real image
			// length (header_view was a truncated prefix, so validate vs `len`).
			const size_t off = static_cast<size_t>(header.data_offset);
			const size_t dlen = static_cast<size_t>(header.data_size);
			if (off > len || len - off < dlen) {
				return unexpected(EAssetError::ABNORMAL_FILE_SIZE); // guard against size_t overflow
			}

			// Slice the SPIR-V payload into owned storage and hand it to Shader.
			std::vector<std::byte> payload(base + off, base + off + dlen);
		return std::make_unique<lux::rdesc::Shader>(std::move(payload));
	}

	lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
	ShaderSerDeser::fromLuxAssetStream(std::istream& ifs)
	{
		using lux::cxx::unexpected;

		// 1) Read the whole file into memory to reduce syscalls
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

		// 2) Parse and validate the header + ShaderAssetInfo
		AssetFileHeader  header{};
		ShaderAssetInfo  sinfo{};
		if (auto ec = loadHeaderRaw<EAssetType::SHADER>(file, header);
			ec != EAssetError::SUCCESS) {
			return unexpected(ec);
		}

		// 3) Parse the shader info and slice it out
		size_t off = static_cast<size_t>(header.info_offset);
		size_t len = static_cast<size_t>(header.info_size);

		if (off > file.size() || file.size() - off < len) {
			return unexpected(EAssetError::ABNORMAL_FILE_SIZE); // guard against size_t overflow
		}

		std::vector<std::byte> shader_info_buf;
		std::string err;
		shader_info_buf.resize(len);
		std::memcpy(shader_info_buf.data(), file.data() + off, len);
		if (!lux::rdesc::ShaderInfo::deserialize(shader_info_buf, sinfo, &err))
		{
			return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
		}

		// 4) Slice out the payload (raw SPIR-V bytes) -> Shader: reuse
		//    decodeData to avoid duplicating the decode logic
		auto data = decodeData(file.data(), file.size());
		if (!data) {
			return unexpected(data.error());
		}

		// 5) Assemble the AssetInfo (the header already carries the common info)
		auto ainfo = std::make_unique<AssetInfo>(header.info);

		// 6) Construct the ShaderAsset and put the Shader into it
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

		std::vector<std::byte> des_data = lux::rdesc::ShaderInfo::serialize(
			shader_asset->shaderInfo());
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

}
