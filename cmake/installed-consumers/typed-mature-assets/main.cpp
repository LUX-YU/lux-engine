#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>
#include <lux/engine/resource/asset/animation/AnimationClipAsset.hpp>
#include <lux/engine/resource/asset/animation/SkeletonAsset.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/resource/asset/shader/ShaderAsset.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/resource/asset/texture/TextureAtlasAssets.hpp>

#include <type_traits>

int main()
{
    static_assert(std::is_same_v<lux::asset::MeshAsset::data_type, lux::rdesc::Mesh>);
    static_assert(std::is_same_v<lux::asset::SkeletonAsset::data_type, lux::rdesc::Skeleton>);
    static_assert(std::is_same_v<lux::asset::AnimationClipAsset::data_type, lux::rdesc::AnimationClip>);
    static_assert(std::is_same_v<lux::asset::MaterialAsset::data_type, lux::asset::MaterialAssetData>);
    static_assert(std::is_same_v<lux::asset::ModelAsset::data_type, lux::asset::ModelAssetData>);
    static_assert(std::is_same_v<lux::asset::TextureAsset::data_type, lux::rdesc::Texture>);
    static_assert(std::is_same_v<
        lux::script::ScriptArtifactAsset::data_type,
        lux::script::ScriptArtifact
    >);
    return 0;
}
