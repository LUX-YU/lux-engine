#pragma once
#include <lux/engine/asset/ShaderAsset.hpp>
#include <lux/engine/asset/AssetSerDeser.hpp>
#include <spirv_cross/spirv_cross.hpp>
#include <spirv_cross/spirv_glsl.hpp>
#include <string_view>

namespace lux::asset
{
	struct ShaderLoadConfig {
		// 若提供，则直接使用，不再猜
		std::optional<EShaderType> explicit_stage;

		// 文件名到阶段的映射（可覆盖默认规则）
		std::vector<std::pair<std::string_view, EShaderType>> name_hints = {
			{".vert",    EShaderType::VERTEX},
			{".frag",    EShaderType::FRAGMENT},
			{".geom",    EShaderType::GEOMETRY},
			{".comp",    EShaderType::COMPUTE}
		};
	};

	class LUX_RESOURCE_PUBLIC ShaderSerDeser : public TAssetSerDeser<ShaderLoadConfig>
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
		[[nodiscard]] static lux::cxx::expected<std::unique_ptr<Shader>, EAssetError>
		decodeData(const void* bytes, std::size_t len) noexcept;

		// load from a raw spri-v code
		EAssetError loadFromSpriv(const std::filesystem::path& path, Shader& shader);

		[[nodiscard]] lux::cxx::expected<AssetIDPair, EAssetError>
		importFromFile(const std::filesystem::path& p) override;

	protected:
		lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
		fromFileStream(std::ifstream& external_path) override;

		lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
		fromLuxAssetStream(std::istream& path) override;

		EAssetError
		exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& path) override;

		void shaderReflection(const Shader& shader);

		bool reflect(const void* bytes, size_t byte_count, ShaderInfo& info);
	};
}
