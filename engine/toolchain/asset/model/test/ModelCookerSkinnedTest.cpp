#include "ModelFixtureBuilder.hpp"

#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/toolchain/asset/model/ModelCooker.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace
{
    lux::asset::AssetId id(std::uint8_t value)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.back() = value;
        return lux::asset::AssetId{bytes};
    }

    lux::asset::AssetInfo info()
    {
        lux::asset::AssetInfo result{};
        result.id = id(90U);
        result.type = lux::asset::ModelAsset::asset_type;
        constexpr char name[] = "skinned-fixture";
        std::memcpy(result.display_name.data(), name, sizeof(name) - 1U);
        return result;
    }
}

int main()
{
    const auto source = lux::toolchain::test::writeSkinnedFixture(
        std::filesystem::path{LUX_MODEL_TEST_ROOT} / "skinned"
    );
    const auto cooked = lux::toolchain::cookModel(info(), source);
    if (!cooked)
        std::cerr << "skinned model cook failed: " << cooked.error().detail << '\n';
    assert(cooked);
    assert(cooked->model && cooked->skeleton);
    assert(!cooked->animations.empty());
    assert((*cooked->skeleton)->data().bones.size() == 2U);
    assert(cooked->model->data().skeleton == (*cooked->skeleton)->id());
    assert(cooked->model->data().animations.front() == cooked->animations.front()->id());
    assert(!cooked->animations.front()->data().tracks.empty());

    bool saw_weighted_vertex{};
    for (const auto& vertex : cooked->meshes.front()->data().vertices)
    {
        float sum{};
        for (std::uint8_t influence = 0U; influence < lux::rdesc::max_bone_influence; ++influence)
            sum += vertex.bone.weights[influence];
        if (sum > 0.0F)
        {
            saw_weighted_vertex = true;
            assert(std::abs(sum - 1.0F) < 1.0e-4F);
        }
    }
    assert(saw_weighted_vertex);

    const auto model_wire = lux::asset::TAssetSerDeser<lux::asset::ModelAsset>::encode(
        *cooked->model,
        lux::asset::AssetEncodeLimits{64U * 1024U * 1024U}
    );
    const auto skeleton_wire = lux::asset::TAssetSerDeser<lux::asset::SkeletonAsset>::encode(
        **cooked->skeleton,
        lux::asset::AssetEncodeLimits{64U * 1024U * 1024U}
    );
    const auto animation_wire = lux::asset::TAssetSerDeser<lux::asset::AnimationClipAsset>::encode(
        *cooked->animations.front(),
        lux::asset::AssetEncodeLimits{64U * 1024U * 1024U}
    );
    assert(model_wire && skeleton_wire && animation_wire);

    auto left_handed = lux::toolchain::ModelCookConfiguration{};
    left_handed.make_left_handed = true;
    const auto unsupported = lux::toolchain::cookModel(info(), source, left_handed);
    assert(!unsupported);
    assert(unsupported.error().code == lux::toolchain::EModelCookError::UNSUPPORTED_FEATURE);
    return 0;
}
