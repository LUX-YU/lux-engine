#include "ModelFixtureBuilder.hpp"

#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/toolchain/asset/model/ModelCooker.hpp>

#include <array>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>

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
        result.id = id(80U);
        result.type = lux::asset::ModelAsset::asset_type;
        constexpr char name[] = "static-pbr-fixture";
        std::memcpy(result.display_name.data(), name, sizeof(name) - 1U);
        return result;
    }

    template <class Asset>
    std::vector<std::byte> encode(const Asset& asset)
    {
        const auto encoded = lux::asset::TAssetSerDeser<Asset>::encode(
            asset,
            lux::asset::AssetEncodeLimits{64U * 1024U * 1024U}
        );
        assert(encoded);
        return *encoded;
    }
}

int main(int argc, char** argv)
{
    const auto source = lux::toolchain::test::writeStaticPbrFixture(
        std::filesystem::path{LUX_MODEL_TEST_ROOT} / "static"
    );
    if (argc == 2 && std::string_view{argv[1]} == "--fixture-only")
        return 0;
    const auto cooked = lux::toolchain::cookModel(info(), source);
    if (!cooked)
        std::cerr << "static model cook failed: " << cooked.error().detail << '\n';
    assert(cooked);
    assert(cooked->model);
    assert(cooked->meshes.size() == 3U);
    assert(cooked->materials.size() >= 2U);
    assert(cooked->textures.size() >= 3U);
    assert(!cooked->skeleton);
    assert(cooked->animations.empty());

    const auto& model = cooked->model->data();
    assert(model.primitives.size() == cooked->meshes.size());
    assert(model.nodes.size() >= 5U);
    assert(model.primitives[0].material == model.primitives[1].material);
    std::size_t shared_primitive_references{};
    bool saw_local_translation{};
    for (const auto& node : model.nodes)
    {
        shared_primitive_references += static_cast<std::size_t>(
            std::count(node.primitives.begin(), node.primitives.end(), 0U)
        );
        saw_local_translation = saw_local_translation ||
            !node.local_transform.translation().isApprox(Eigen::Vector3f::Zero());
    }
    assert(shared_primitive_references == 2U);
    assert(saw_local_translation);
    assert(std::any_of(
        cooked->materials.begin(),
        cooked->materials.end(),
        [](const auto& material) noexcept {
            return material->data().alpha_mode == lux::rdesc::EAlphaMode::Mask;
        }
    ));

    const auto first_model_wire = encode(*cooked->model);
    const auto repeated = lux::toolchain::cookModel(info(), source);
    assert(repeated);
    assert(repeated->model->id() == cooked->model->id());
    assert(encode(*repeated->model) == first_model_wire);
    assert(repeated->meshes.size() == cooked->meshes.size());
    assert(repeated->materials.size() == cooked->materials.size());
    assert(repeated->textures.size() == cooked->textures.size());
    for (std::size_t index = 0U; index < cooked->meshes.size(); ++index)
        assert(repeated->meshes[index]->id() == cooked->meshes[index]->id());
    for (std::size_t index = 0U; index < cooked->materials.size(); ++index)
        assert(repeated->materials[index]->id() == cooked->materials[index]->id());
    return 0;
}
