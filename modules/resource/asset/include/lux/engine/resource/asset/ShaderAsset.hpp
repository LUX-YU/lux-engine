#pragma once
#include <lux/engine/description/Shader.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include "Asset.hpp"
#include "AssetSerDeser.hpp"

namespace lux::asset
{
	// for serialization/deserialization
	using ShaderAssetInfo = lux::rdesc::ShaderInfo;

	class LUX_RESOURCE_PUBLIC ShaderAsset : public TAsset<lux::rdesc::Shader>
	{
	public:
		static constexpr EAssetType asset_type{ EAssetType::SHADER };

		ShaderAsset(std::unique_ptr<AssetInfo> info, ShaderAssetInfo type);

		[[nodiscard]] const ShaderAssetInfo& shaderInfo() const;

	private:
		ShaderAssetInfo _shader_info;
	};
}

