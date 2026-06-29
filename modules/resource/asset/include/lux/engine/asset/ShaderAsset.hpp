#pragma once
#include <lux/engine/description/Shader.hpp>
#include "Asset.hpp"
#include "AssetSerDeser.hpp"
#include "ShaderInfo.hpp"

namespace lux::asset
{
	// ===== Backward compatibility: re-export description type =====
	using lux::rdesc::Shader;

	// for serialization/deserialization
	using ShaderAssetInfo = ShaderInfo;

	class LUX_RESOURCE_PUBLIC ShaderAsset : public TAsset<Shader>
	{
	public:
		static constexpr EAssetType asset_type{ EAssetType::SHADER };

		ShaderAsset(std::unique_ptr<AssetInfo> info, ShaderAssetInfo type);

		[[nodiscard]] const ShaderAssetInfo& shaderInfo() const;

	private:
		ShaderAssetInfo _shader_info;
	};
}

