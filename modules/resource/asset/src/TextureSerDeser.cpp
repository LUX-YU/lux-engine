#include <lux/engine/asset/TextureSerDeser.hpp>
#include <AssetManagerImpl.hpp>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <rgbcx.h>
#include <bc7enc.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace
{
	using lux::rdesc::ETexturePixelFormat;
	using lux::rdesc::ETextureAssetFlags;
	using lux::rdesc::TextureInfo;
	using lux::rdesc::TextureMipRange;
	using lux::rdesc::kTextureMaxMipCount;

	[[nodiscard]] bool supportsRawOutputFormat(ETexturePixelFormat fmt) noexcept
	{
		switch (fmt)
		{
		case ETexturePixelFormat::RGBA8_UNORM:
		case ETexturePixelFormat::RGBA8_SRGB:
		case ETexturePixelFormat::RG8_UNORM:
		case ETexturePixelFormat::R8_UNORM:
			return true;
		default:
			return false;
		}
	}

	[[nodiscard]] bool supportsCompressedEncoding(ETexturePixelFormat fmt) noexcept
	{
		switch (fmt)
		{
		case ETexturePixelFormat::BC1_SRGB:
		case ETexturePixelFormat::BC3_SRGB:
		case ETexturePixelFormat::BC5_UNORM:
		case ETexturePixelFormat::BC7_SRGB:
			return true;
		default:
			return false;
		}
	}

	[[nodiscard]] size_t blockBytesForCompressed(ETexturePixelFormat fmt) noexcept
	{
		switch (fmt)
		{
		case ETexturePixelFormat::BC1_SRGB:
			return 8;
		case ETexturePixelFormat::BC3_SRGB:
		case ETexturePixelFormat::BC5_UNORM:
		case ETexturePixelFormat::BC7_SRGB:
			return 16;
		default:
			return 0;
		}
	}

	[[nodiscard]] uint32_t compressedChannelCount(ETexturePixelFormat fmt) noexcept
	{
		switch (fmt)
		{
		case ETexturePixelFormat::BC5_UNORM:
			return 2;
		default:
			return 4;
		}
	}

	void clearMipRanges(std::array<TextureMipRange, kTextureMaxMipCount>& ranges) noexcept
	{
		for (auto& r : ranges)
			r = {};
	}

	void assignSingleMip(TextureInfo& info, size_t payload_size) noexcept
	{
		info.mip_count = 1;
		clearMipRanges(info.mip_ranges);
		info.mip_ranges[0] = TextureMipRange{
			.offset = 0,
			.size = static_cast<uint64_t>(payload_size),
			.width = static_cast<uint32_t>(std::max(info.width, 0)),
			.height = static_cast<uint32_t>(std::max(info.height, 0)),
		};
	}

	[[nodiscard]] void* allocPayloadCopy(const std::vector<std::byte>& payload) noexcept
	{
		if (payload.empty())
			return nullptr;
		void* out = std::malloc(payload.size());
		if (!out)
			return nullptr;
		std::memcpy(out, payload.data(), payload.size());
		return out;
	}

	void downsampleRgbaBox(const std::vector<uint8_t>& src, int src_w, int src_h,
		std::vector<uint8_t>& dst, int& dst_w, int& dst_h)
	{
		dst_w = std::max(1, src_w / 2);
		dst_h = std::max(1, src_h / 2);
		dst.resize(static_cast<size_t>(dst_w) * dst_h * 4u);

		for (int y = 0; y < dst_h; ++y)
		{
			for (int x = 0; x < dst_w; ++x)
			{
				uint32_t sum[4]{0, 0, 0, 0};
				for (int ky = 0; ky < 2; ++ky)
				{
					for (int kx = 0; kx < 2; ++kx)
					{
						const int sx = std::min(src_w - 1, x * 2 + kx);
						const int sy = std::min(src_h - 1, y * 2 + ky);
						const size_t idx = (static_cast<size_t>(sy) * src_w + sx) * 4u;
						sum[0] += src[idx + 0];
						sum[1] += src[idx + 1];
						sum[2] += src[idx + 2];
						sum[3] += src[idx + 3];
					}
				}

				const size_t didx = (static_cast<size_t>(y) * dst_w + x) * 4u;
				dst[didx + 0] = static_cast<uint8_t>(sum[0] / 4u);
				dst[didx + 1] = static_cast<uint8_t>(sum[1] / 4u);
				dst[didx + 2] = static_cast<uint8_t>(sum[2] / 4u);
				dst[didx + 3] = static_cast<uint8_t>(sum[3] / 4u);
			}
		}
	}

	// One-time initialization for bc7enc_rdo encoders
	struct Bc7encInit {
		bc7enc_compress_block_params bc7_params;
		Bc7encInit() {
			rgbcx::init();
			bc7enc_compress_block_init();
			bc7enc_compress_block_params_init(&bc7_params);
		}
	};
	static const Bc7encInit s_bc7enc_init;

	[[nodiscard]] bool encodeCompressedMipLevel(ETexturePixelFormat fmt,
		const std::vector<uint8_t>& rgba, int width, int height,
		std::vector<std::byte>& out)
	{
		const size_t block_bytes = blockBytesForCompressed(fmt);
		const uint32_t block_w = (static_cast<uint32_t>(width) + 3u) / 4u;
		const uint32_t block_h = (static_cast<uint32_t>(height) + 3u) / 4u;
		out.resize(static_cast<size_t>(block_w) * block_h * block_bytes);

		uint8_t block_rgba[64]{};

		size_t out_offset = 0;
		for (uint32_t by = 0; by < block_h; ++by)
		{
			for (uint32_t bx = 0; bx < block_w; ++bx)
			{
				for (uint32_t py = 0; py < 4; ++py)
				{
					for (uint32_t px = 0; px < 4; ++px)
					{
						const int sx = std::min(width - 1, static_cast<int>(bx * 4u + px));
						const int sy = std::min(height - 1, static_cast<int>(by * 4u + py));
						const size_t src_i = (static_cast<size_t>(sy) * width + sx) * 4u;
						const size_t dst_i = (static_cast<size_t>(py) * 4u + px) * 4u;
						block_rgba[dst_i + 0] = rgba[src_i + 0];
						block_rgba[dst_i + 1] = rgba[src_i + 1];
						block_rgba[dst_i + 2] = rgba[src_i + 2];
						block_rgba[dst_i + 3] = rgba[src_i + 3];
					}
				}

				auto* dst_block = reinterpret_cast<unsigned char*>(out.data() + out_offset);

				switch (fmt)
				{
				case ETexturePixelFormat::BC1_SRGB:
					rgbcx::encode_bc1(10, dst_block, block_rgba, false, false);
					break;
				case ETexturePixelFormat::BC3_SRGB:
					rgbcx::encode_bc3(10, dst_block, block_rgba);
					break;
				case ETexturePixelFormat::BC5_UNORM:
					rgbcx::encode_bc5(dst_block, block_rgba, 0, 1, 4);
					break;
				case ETexturePixelFormat::BC7_SRGB:
					bc7enc_compress_block(dst_block, block_rgba, &s_bc7enc_init.bc7_params);
					break;
				default:
					return false;
				}

				out_offset += block_bytes;
			}
		}

		return true;
	}

	[[nodiscard]] bool buildTexturePayloadFromRgba(
		const uint8_t* rgba_pixels,
		int width,
		int height,
		const lux::asset::TextureLoadConfig& cfg,
		TextureInfo& out_info,
		std::vector<std::byte>& out_payload) noexcept
	{
		if (!rgba_pixels || width <= 0 || height <= 0)
			return false;

		out_info = {};
		out_info.width = width;
		out_info.height = height;
		out_info.layers = 1;
		out_info.pixel_format = cfg.output_format;
		out_info.color_space = cfg.color_space;
		out_info.flags = cfg.flags;
		out_info.copy = false;
		out_info.owns_data = true;

		if (lux::rdesc::isCompressedFormat(cfg.output_format))
		{
			if (!supportsCompressedEncoding(cfg.output_format))
				return false;

			std::vector<uint8_t> current_rgba(
				rgba_pixels,
				rgba_pixels + static_cast<size_t>(width) * height * 4u);
			clearMipRanges(out_info.mip_ranges);

			int mip_w = width;
			int mip_h = height;
			uint32_t mip_count = 0;

			while (mip_count < kTextureMaxMipCount)
			{
				std::vector<std::byte> level_payload;
				if (!encodeCompressedMipLevel(cfg.output_format, current_rgba, mip_w, mip_h, level_payload))
					return false;

				const uint64_t offset = static_cast<uint64_t>(out_payload.size());
				out_payload.insert(out_payload.end(), level_payload.begin(), level_payload.end());

				out_info.mip_ranges[mip_count] = TextureMipRange{
					.offset = offset,
					.size = static_cast<uint64_t>(level_payload.size()),
					.width = static_cast<uint32_t>(mip_w),
					.height = static_cast<uint32_t>(mip_h),
				};

				++mip_count;
				if (mip_w == 1 && mip_h == 1)
					break;

				std::vector<uint8_t> next_rgba;
				int next_w = 1;
				int next_h = 1;
				downsampleRgbaBox(current_rgba, mip_w, mip_h, next_rgba, next_w, next_h);
				current_rgba.swap(next_rgba);
				mip_w = next_w;
				mip_h = next_h;
			}

			out_info.channel = static_cast<int>(compressedChannelCount(cfg.output_format));
			out_info.mip_count = mip_count;
			out_info.flags |= lux::rdesc::toUnderlying(ETextureAssetFlags::COMPRESSED);
			return true;
		}

		if (!supportsRawOutputFormat(cfg.output_format))
			return false;

		const size_t pixel_count = static_cast<size_t>(width) * height;
		switch (cfg.output_format)
		{
		case ETexturePixelFormat::RGBA8_UNORM:
		case ETexturePixelFormat::RGBA8_SRGB:
		{
			out_info.channel = 4;
			out_payload.resize(pixel_count * 4u);
			std::memcpy(out_payload.data(), rgba_pixels, out_payload.size());
			break;
		}
		case ETexturePixelFormat::RG8_UNORM:
		{
			out_info.channel = 2;
			out_payload.resize(pixel_count * 2u);
			for (size_t i = 0; i < pixel_count; ++i)
			{
				out_payload[i * 2u + 0] = static_cast<std::byte>(rgba_pixels[i * 4u + 0]);
				out_payload[i * 2u + 1] = static_cast<std::byte>(rgba_pixels[i * 4u + 1]);
			}
			break;
		}
		case ETexturePixelFormat::R8_UNORM:
		{
			out_info.channel = 1;
			out_payload.resize(pixel_count);
			for (size_t i = 0; i < pixel_count; ++i)
				out_payload[i] = static_cast<std::byte>(rgba_pixels[i * 4u + 0]);
			break;
		}
		default:
			return false;
		}

		assignSingleMip(out_info, out_payload.size());
		return true;
	}

	[[nodiscard]] bool validateMipRanges(const TextureInfo& info, size_t payload_size) noexcept
	{
		if (info.mip_count == 0 || info.mip_count > kTextureMaxMipCount)
			return false;

		const uint64_t payload = static_cast<uint64_t>(payload_size);
		for (uint32_t i = 0; i < info.mip_count; ++i)
		{
			const auto& m = info.mip_ranges[i];
			if (m.width == 0 || m.height == 0)
				return false;
			if (m.size == 0)
				return false;
			if (m.offset > payload)
				return false;
			if (m.size > payload - m.offset)
				return false;
		}
		return true;
	}

	// Reconstruct the pure rdesc::Texture from an already-parsed header + POD
	// TextureAssetInfo over a full .luxasset byte image. This is the single
	// "decode the data section into a Texture" step shared by the manager-free
	// decodeData() and the asset-assembling fromLuxAssetStream() — neither
	// touches the AssetManager. Returns an owning Texture (it malloc-copies the
	// pixel bytes and takes ownership) or an EAssetError.
	[[nodiscard]] lux::cxx::expected<std::unique_ptr<lux::rdesc::Texture>, lux::asset::EAssetError>
	decodeTextureFromImage(
		const std::vector<std::byte>& file,
		const lux::asset::AssetFileHeader& header,
		const lux::rdesc::TextureAssetInfo& tex_info) noexcept
	{
		using lux::asset::EAssetError;

		// Bounds-check the pixel data section.
		if (file.size() < header.data_offset + header.data_size)
			return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);

		const size_t data_len = static_cast<size_t>(header.data_size);

		// Rebuild the runtime TextureInfo from the POD on-disk info.
		TextureInfo tinfo{};
		tinfo.width        = tex_info.width;
		tinfo.height       = tex_info.height;
		tinfo.channel      = tex_info.channel;
		tinfo.layers       = tex_info.layers;
		tinfo.mip_count    = tex_info.mip_count;
		tinfo.pixel_format = static_cast<lux::rdesc::ETexturePixelFormat>(tex_info.pixel_format);
		tinfo.color_space  = static_cast<lux::rdesc::ETextureColorSpace>(tex_info.color_space);
		tinfo.flags        = tex_info.flags;
		tinfo.mip_ranges   = tex_info.mip_ranges;
		tinfo.copy         = false;
		tinfo.owns_data    = true;

		if (!validateMipRanges(tinfo, data_len))
			return lux::cxx::unexpected(EAssetError::WRONG_FILE_HEADER);

		// Copy the pixel payload into a malloc'd block the Texture will own.
		void* pixels = std::malloc(data_len);
		if (!pixels)
			return lux::cxx::unexpected(EAssetError::OUT_OF_MEMORY);
		std::memcpy(pixels, file.data() + header.data_offset, data_len);

		// make_unique<Texture> could throw bad_alloc; keep decodeData noexcept
		// by funneling any throw into an error rather than letting it escape.
		try
		{
			return std::make_unique<lux::rdesc::Texture>(tinfo, pixels, data_len);
		}
		catch (...)
		{
			std::free(pixels);
			return lux::cxx::unexpected(EAssetError::OUT_OF_MEMORY);
		}
	}
}

namespace lux::asset
{
	TextureSerDeser::TextureSerDeser(std::shared_ptr<AssetManager> manager)
	: TAssetSerDeser(std::move(manager)) {
	}

	TextureSerDeser::~TextureSerDeser(){}

	lux::cxx::expected<AssetIDPair, EAssetError>
	TextureSerDeser::importFromFile(const std::filesystem::path& path)
	{
		stbi_set_flip_vertically_on_load(config().flip_vertically);
		int width = 0;
		int height = 0;
		int source_channels = 0;

		uint8_t* img_data = stbi_load(
			path.string().c_str(),
			&width,
			&height,
			&source_channels,
			STBI_rgb_alpha);

		if (img_data == nullptr) {
			return lux::cxx::unexpected(EAssetError::UNKNOWN_ERROR);
		}
		(void)source_channels;

		TextureInfo texture_info{};
		std::vector<std::byte> payload;
		const bool ok = buildTexturePayloadFromRgba(
			img_data,
			width,
			height,
			config(),
			texture_info,
			payload);

		stbi_image_free(img_data);
		if (!ok)
			return lux::cxx::unexpected(EAssetError::UNSUPPORTED);

		void* payload_ptr = allocPayloadCopy(payload);
		if (!payload_ptr)
			return lux::cxx::unexpected(EAssetError::OUT_OF_MEMORY);

		auto texture = std::make_unique<Texture>(texture_info, payload_ptr, payload.size());

		auto asset = manager().createAssetSeeded<TextureAsset>(
			config().deterministic_seed, std::move(texture));
		const auto id = asset->id();
		const auto asset_ptr = asset.get();

		if (!submitAsset(std::move(asset))) {
			return lux::cxx::unexpected(EAssetError::ASSET_ALREADY_EXIST);
		}

		return AssetIDPair{ asset_ptr, id };
	}

	EAssetError TextureSerDeser::loadInfoFromFile(const std::filesystem::path& path, TextureInfo& texture)
	{
		bool load_rst = stbi_info(
			path.string().c_str(),
			&texture.width,
			&texture.height,
			&texture.channel
		);

		return load_rst ? EAssetError::SUCCESS : EAssetError::UNKNOWN_ERROR;
	}

	lux::cxx::expected<std::unique_ptr<lux::rdesc::Texture>, EAssetError>
	TextureSerDeser::decodeData(const void* bytes, std::size_t len) noexcept
	{
		if (!bytes || len == 0)
			return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);

		// loadHeader / decodeTextureFromImage operate on a contiguous byte
		// buffer; mirror the std::vector<std::byte> the stream path uses. The
		// copy could throw bad_alloc — keep this noexcept by catching it.
		try
		{
			const auto* first = static_cast<const std::byte*>(bytes);
			std::vector<std::byte> file(first, first + len);

			AssetFileHeader  header{};
			TextureAssetInfo tex_info{};
			const auto err = loadHeader<TextureAssetInfo, EAssetType::TEXTURE>(file, header, tex_info);
			if (err != EAssetError::SUCCESS)
				return lux::cxx::unexpected(err);

			return decodeTextureFromImage(file, header, tex_info);
		}
		catch (...)
		{
			return lux::cxx::unexpected(EAssetError::OUT_OF_MEMORY);
		}
	}

	lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
	TextureSerDeser::fromLuxAssetStream(std::istream& ifs)
	{
		// Read entire file into memory
		ifs.seekg(0, std::ios::end);
		auto file_size = static_cast<size_t>(ifs.tellg());
		ifs.seekg(0, std::ios::beg);

		std::vector<std::byte> file(file_size);
		ifs.read(reinterpret_cast<char*>(file.data()), static_cast<std::streamsize>(file_size));

		// Parse POD header + TextureAssetInfo
		AssetFileHeader header{};
		TextureAssetInfo tex_info{};
		auto err = loadHeader<TextureAssetInfo, EAssetType::TEXTURE>(file, header, tex_info);
		if (err != EAssetError::SUCCESS)
			return lux::cxx::unexpected(err);

		// Decode the pixel data section into the pure Texture (shared with
		// decodeData — neither path touches the AssetManager).
		auto texture = decodeTextureFromImage(file, header, tex_info);
		if (!texture.has_value())
			return lux::cxx::unexpected(texture.error());

		// Assemble the full LuxAsset around the decoded data + parsed AssetInfo.
		auto ainfo = std::make_unique<AssetInfo>(header.info);
		auto asset = std::make_unique<TextureAsset>(std::move(ainfo));
		asset->setData(std::move(texture.value()));

		return asset;
	}

	lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
	TextureSerDeser::fromMemory(const void* mem, size_t len)
	{
		stbi_set_flip_vertically_on_load(config().flip_vertically);
		auto asset_info = manager().createAssetInfo(EAssetType::TEXTURE, config().deterministic_seed);

		int width = 0;
		int height = 0;
		int source_channels = 0;
		uint8_t* img_data = stbi_load_from_memory(
			static_cast<const uint8_t*>(mem),
			len,
			&width,
			&height,
			&source_channels,
			STBI_rgb_alpha
		);

		if (img_data == nullptr) {
			return lux::cxx::unexpected(EAssetError::UNKNOWN_ERROR);
		}
		(void)source_channels;

		TextureInfo texture_info{};
		std::vector<std::byte> payload;
		const bool ok = buildTexturePayloadFromRgba(
			img_data,
			width,
			height,
			config(),
			texture_info,
			payload);

		stbi_image_free(img_data);
		if (!ok)
			return lux::cxx::unexpected(EAssetError::UNSUPPORTED);

		void* payload_ptr = allocPayloadCopy(payload);
		if (!payload_ptr)
			return lux::cxx::unexpected(EAssetError::OUT_OF_MEMORY);

		std::unique_ptr<TextureAsset> texture_asset = std::make_unique<TextureAsset>(std::move(asset_info));
		std::unique_ptr<Texture>	  texture		= std::make_unique<Texture>(texture_info, payload_ptr, payload.size());

		texture_asset->setData(std::move(texture));

		return texture_asset;
	}

	EAssetError
	TextureSerDeser::exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& of)
	{
		auto texture_asset = asset.as<TextureAsset>();
		if (!texture_asset || !texture_asset->data())
			return EAssetError::UNKNOWN_ERROR;

		const auto* tex = texture_asset->data();

		const auto& tinfo = tex->info();

		// Populate POD info
		TextureAssetInfo tex_info{};
		tex_info.width   = tinfo.width;
		tex_info.height  = tinfo.height;
		tex_info.channel = tinfo.channel;
		tex_info.layers  = tinfo.layers;
		tex_info.mip_count = tinfo.mip_count;
		tex_info.pixel_format = static_cast<uint32_t>(tinfo.pixel_format);
		tex_info.color_space = static_cast<uint32_t>(tinfo.color_space);
		tex_info.flags = tinfo.flags;
		tex_info.mip_ranges = tinfo.mip_ranges;

		if (!validateMipRanges(tinfo, tex->size()))
			return EAssetError::WRONG_FILE_HEADER;

		// Build header (POD path: header + TextureAssetInfo in one block)
		auto header_bytes = makeHeader<TextureAssetInfo, EAssetType::TEXTURE>(
			*texture_asset->info(), tex_info, tex->size());

		// Write: header+info | raw pixel data
		of.write(reinterpret_cast<const char*>(header_bytes.data()),
				 static_cast<std::streamsize>(header_bytes.size()));
		of.write(reinterpret_cast<const char*>(tex->data()),
				 static_cast<std::streamsize>(tex->size()));
		of.flush();

		return EAssetError::SUCCESS;
	}

	// Texture class implementations moved to modules/resource/description/src/Texture.cpp
}