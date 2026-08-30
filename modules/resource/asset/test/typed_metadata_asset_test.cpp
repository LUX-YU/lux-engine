#include <lux/cxx/algorithm/sha256.hpp>

#include <lux/engine/resource/asset/animation/AnimationClipAsset.hpp>
#include <lux/engine/resource/asset/animation/SkeletonAsset.hpp>
#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>

#include <Eigen/Geometry>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    [[nodiscard]] lux::asset::AssetId id(std::uint32_t ordinal)
    {
        std::array<std::uint8_t, 16U> bytes{
            0x31U, 0x90U, 0x14U, 0x00U,
            0x00U, 0x00U, 0x40U, 0x00U,
            0x80U, 0x00U, 0x00U, 0x00U,
            0x00U, 0x00U, 0x00U, 0x00U,
        };
        bytes[12] = static_cast<std::uint8_t>((ordinal >> 24U) & 0xFFU);
        bytes[13] = static_cast<std::uint8_t>((ordinal >> 16U) & 0xFFU);
        bytes[14] = static_cast<std::uint8_t>((ordinal >> 8U) & 0xFFU);
        bytes[15] = static_cast<std::uint8_t>(ordinal & 0xFFU);
        return lux::asset::AssetId{bytes};
    }

    template <class Asset>
    [[nodiscard]] lux::asset::AssetInfo info(std::uint32_t ordinal)
    {
        lux::asset::AssetInfo result{};
        result.id = id(ordinal);
        result.type = Asset::asset_type;
        result.date = 0x0102030405060708ULL;
        constexpr std::string_view display{"wire-contract"};
        std::memcpy(result.display_name.data(), display.data(), display.size());
        return result;
    }

    [[nodiscard]] std::string sha256(std::span<const std::byte> bytes)
    {
        const auto digest = lux::cxx::algorithm::Sha256::hash(bytes);
        std::array<char, lux::cxx::algorithm::Sha256Digest::hex_size> result{};
        digest.formatHex(result);
        return {result.data(), result.size()};
    }

    template <class Asset>
    std::vector<std::byte> verify(
        const std::shared_ptr<const Asset>& asset,
        std::size_t expected_size,
        std::string_view expected_hash
    )
    {
        constexpr lux::asset::AssetEncodeLimits encode_limits{1024U * 1024U};
        constexpr lux::asset::AssetDecodeLimits decode_limits{1024U * 1024U, 1024U * 1024U, 4U};
        const auto encoded = lux::asset::TAssetSerDeser<Asset>::encode(*asset, encode_limits);
        assert(encoded && encoded->size() == expected_size);
        assert(sha256(*encoded) == expected_hash);
        const auto decoded = lux::asset::TAssetSerDeser<Asset>::decode(
            asset->id(),
            lux::cxx::SharedBytes<>::copyOf(*encoded),
            decode_limits
        );
        assert(decoded);
        const auto reencoded = lux::asset::TAssetSerDeser<Asset>::encode(**decoded, encode_limits);
        assert(reencoded && *reencoded == *encoded);
        return *encoded;
    }

    template <class Asset, class Value>
    void rejectInformationMutation(
        std::vector<std::byte> encoded,
        lux::asset::AssetId requested,
        std::size_t relative_offset,
        const Value& value
    )
    {
        constexpr lux::asset::AssetDecodeLimits limits{1024U * 1024U, 1024U * 1024U, 4U};
        auto owned = lux::cxx::SharedBytes<>::copyOf(encoded);
        const auto image = lux::asset::inspectCookedAssetImage(owned, limits);
        assert(image);
        const auto information_offset = static_cast<std::size_t>(
            image->information().data() - image->image().data()
        );
        assert(information_offset + relative_offset + sizeof(Value) <= encoded.size());
        std::memcpy(encoded.data() + information_offset + relative_offset, &value, sizeof(Value));
        const auto decoded = lux::asset::TAssetSerDeser<Asset>::decode(
            requested,
            lux::cxx::SharedBytes<>::copyOf(encoded),
            limits
        );
        assert(!decoded);
    }
} // namespace

int main()
{
    auto mesh = std::make_shared<lux::rdesc::Mesh>();
    lux::rdesc::Vertex vertex{};
    vertex.normal = Eigen::Vector3f::UnitZ();
    vertex.tangent = Eigen::Vector3f::UnitX();
    vertex.bitangent = Eigen::Vector3f::UnitY();
    vertex.bone.bone_ids[0] = -1;
    mesh->vertices = {vertex, vertex, vertex};
    mesh->vertices[1].position.x() = 1.0F;
    mesh->vertices[2].position.y() = 1.0F;
    mesh->indices = {0U, 1U, 2U};
    const auto mesh_asset = lux::asset::MeshAsset::create(
        info<lux::asset::MeshAsset>(6U),
        std::move(mesh)
    );
    assert(mesh_asset);
    const auto mesh_wire = verify(
        *mesh_asset,
        708U,
        "55d3667e298f4b5a358cdd9979b348323d5c911ff5f5971f55beeaf181b5f765"
    );
    rejectInformationMutation<lux::asset::MeshAsset>(mesh_wire, id(6U), 24U + 3U * sizeof(lux::rdesc::Vertex), 99U);

    auto invalid_mesh = std::make_shared<lux::rdesc::Mesh>();
    invalid_mesh->vertices = {vertex, vertex, vertex};
    invalid_mesh->indices = {0U, 1U, 3U};
    assert(!lux::asset::MeshAsset::create(info<lux::asset::MeshAsset>(60U), invalid_mesh));
    invalid_mesh->indices = {0U, 1U, 2U};
    invalid_mesh->vertices[0].position.x() = std::numeric_limits<float>::quiet_NaN();
    assert(!lux::asset::MeshAsset::create(info<lux::asset::MeshAsset>(61U), invalid_mesh));
    invalid_mesh->vertices[0].position.x() = 0.0F;
    invalid_mesh->vertices[0].bone.bone_ids[0] = 0;
    invalid_mesh->vertices[0].bone.weights[0] = 0.5F;
    assert(!lux::asset::MeshAsset::create(info<lux::asset::MeshAsset>(62U), invalid_mesh));

    auto skeleton = std::make_shared<lux::rdesc::Skeleton>();
    lux::rdesc::Bone_t bone{};
    bone.name = "root";
    bone.parent_index = -1;
    bone.bind_local = Eigen::Affine3f::Identity();
    bone.inv_bind_world = Eigen::Affine3f::Identity();
    skeleton->bones.push_back(std::move(bone));
    const auto skeleton_asset = lux::asset::SkeletonAsset::create(
        info<lux::asset::SkeletonAsset>(10U),
        std::move(skeleton)
    );
    assert(skeleton_asset);
    const auto skeleton_wire = verify(
        *skeleton_asset,
        576U,
        "0f9757141b0f49ac269a74901050c96d378d71858227e31b58e6aeca0ece0248"
    );
    rejectInformationMutation<lux::asset::SkeletonAsset>(skeleton_wire, id(10U), 24U, std::int32_t{-2});

    auto invalid_skeleton = std::make_shared<lux::rdesc::Skeleton>();
    invalid_skeleton->bones.push_back({
        "root",
        -2,
        Eigen::Affine3f::Identity(),
        Eigen::Affine3f::Identity()
    });
    assert(!lux::asset::SkeletonAsset::create(info<lux::asset::SkeletonAsset>(63U), invalid_skeleton));

    auto clip = std::make_shared<lux::rdesc::AnimationClip>();
    clip->name = "idle";
    clip->duration = 1.0F;
    clip->loop = true;
    lux::rdesc::BoneTrack track{};
    track.bone_index = 0;
    track.times_t = {0.0F};
    track.translations = {Eigen::Vector3f::Zero()};
    track.times_r = {0.0F};
    track.rotations = {Eigen::Quaternionf::Identity()};
    track.times_s = {0.0F};
    track.scales = {Eigen::Vector3f::Ones()};
    clip->tracks.push_back(std::move(track));
    const auto animation_asset = lux::asset::AnimationClipAsset::create(
        info<lux::asset::AnimationClipAsset>(11U),
        std::move(clip)
    );
    assert(animation_asset);
    const auto animation_wire = verify(
        *animation_asset,
        496U,
        "c88929b5122c40953854b8828d48b87022a7711b8705e249d2df7145ef0baf50"
    );
    rejectInformationMutation<lux::asset::AnimationClipAsset>(
        animation_wire,
        id(11U),
        20U,
        std::numeric_limits<float>::quiet_NaN()
    );

    auto invalid_clip = std::make_shared<lux::rdesc::AnimationClip>();
    invalid_clip->name = "invalid";
    invalid_clip->duration = 1.0F;
    lux::rdesc::BoneTrack invalid_track{};
    invalid_track.bone_index = 0;
    invalid_track.times_t = {0.5F, 0.25F};
    invalid_track.translations = {Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones()};
    invalid_clip->tracks.push_back(std::move(invalid_track));
    assert(!lux::asset::AnimationClipAsset::create(
        info<lux::asset::AnimationClipAsset>(64U),
        invalid_clip
    ));
    return 0;
}
