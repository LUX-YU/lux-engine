#include <lux/engine/asset/ShaderAsset.hpp>

namespace lux::asset
{
    // Shader class implementations moved to modules/resource/description/src/Shader.cpp
    // ShaderInfo::serialize/deserialize moved to modules/resource/description/src/ShaderInfo.cpp
    ShaderAsset::ShaderAsset(std::unique_ptr<AssetInfo> info, ShaderAssetInfo shader_info)
        : TAsset<Shader>(std::move(info))
    {
        _shader_info = shader_info;
    }

    const ShaderAssetInfo &ShaderAsset::shaderInfo() const
    {
        return _shader_info;
    }
}