#pragma once
#include <lux/engine/resource/asset/visibility.h>
#include <lux/engine/resource/asset/shader/ShaderAsset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <string_view>

namespace lux::asset
{
	struct ShaderLoadConfig {
		// If provided, use it directly instead of guessing.
		std::optional<lux::rdesc::EShaderType> explicit_stage;

		// Filename-to-stage mapping (can override the default rules).
		std::vector<std::pair<std::string_view, lux::rdesc::EShaderType>> name_hints = {
			{".vert",    lux::rdesc::EShaderType::VERTEX},
			{".frag",    lux::rdesc::EShaderType::FRAGMENT},
			{".geom",    lux::rdesc::EShaderType::GEOMETRY},
			{".comp",    lux::rdesc::EShaderType::COMPUTE}
		};
	};

	class LUX_ASSET_PUBLIC ShaderSerDeser : public TAssetSerDeser<ShaderLoadConfig>
	{
	public:
		explicit ShaderSerDeser(std::shared_ptr<AssetManager>);

		// Pure-data decode entry point for asynchronous loading.
		//
		// Parses a COMPLETE .luxasset memory image (header + info + data +
		// optional payloads) and returns ONLY the asset payload object
		// (asset_data_t == lux::rdesc::Shader, the owned SPIR-V bytes). It is a
		// free-standing computation: it touches the byte image and nothing else
		// — no AssetManager, no registration, no AssetInfo / ShaderInfo. This is
		// the thread-safe half a worker thread runs off the main thread; the
		// main thread later injects the result into an already-existing asset
		// shell at a sync point.
		//
		// noexcept: every error is reported through the expected channel; any
		// internally throwing call (ShaderInfo deserialize is bypassed entirely,
		// vector allocation is the only candidate) is caught and mapped.
		[[nodiscard]] static lux::cxx::expected<
			std::unique_ptr<lux::rdesc::Shader>,
			EAssetError>
		decodeData(const void* bytes, std::size_t len) noexcept;

	protected:
		lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
		fromLuxAssetStream(std::istream& path) override;

		EAssetError
		exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& path) override;
	};
}
