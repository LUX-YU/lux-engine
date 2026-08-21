#include <lux/cxx/algorithm/sha256.hpp>

#include <lux/engine/resource/asset/AssetCodecCatalog.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/animation/AnimationClipAsset.hpp>
#include <lux/engine/resource/asset/animation/SkeletonAsset.hpp>
#include <lux/engine/resource/asset/material/MaterialAsset.hpp>
#include <lux/engine/resource/asset/material/MaterialInstanceAsset.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/resource/asset/script/ScriptAsset.hpp>
#include <lux/engine/resource/asset/shader/ShaderAsset.hpp>
#include <lux/engine/resource/asset/storage/VirtualPath.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/resource/asset/texture/TextureAtlasAssets.hpp>

#include <Eigen/Geometry>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using namespace lux::asset;

    struct Golden final
    {
        std::string_view name;
        std::size_t size;
        std::string_view sha256;
    };

    // Filled from the deterministic fixtures below. These are format contracts,
    // not implementation fingerprints: any intentional wire change must update
    // the schema/version before these values are changed.
    constexpr std::array<Golden, 11> kGolden{{
        {"texture", 820u,
            "e01de6ccfb600f997b0ad08035acbda1c404647faa86284e4dcd28a03efed3cc"},
        {"texture_atlas", 477u,
            "2c0a7f6353760c6994065c143b169707c16191899076b53c0814604e5a86d2e1"},
        {"flipbook_clip", 469u,
            "38e7fa62a043f95947ba06b0a756118ec86ea33250195791038e541747a15533"},
        {"material", 495u,
            "34ddba8c3a78463d048553fa3d44481a737646895b6797e8fb62e19e9bd1fd8f"},
        {"material_instance", 472u,
            "a35ae4037601ccabd656b458ce979beec7947ccb3081345ddb3955fabbb6d495"},
        {"mesh", 708u,
            "55d3667e298f4b5a358cdd9979b348323d5c911ff5f5971f55beeaf181b5f765"},
        {"model", 485u,
            "715aa44f5c17fb9fcf23fb2f91bc35b2c4b9db084ba6325ce5dcfc92822558be"},
        {"script", 531u,
            "0c9673e7e98a1aa11c027658ab12d4e7f42d75c8a5885ef89453d9836cc886ed"},
        {"shader", 423u,
            "ba6d84448d95af9a83ba28d16cf8899f5c788595c2c94078b86ae6ea6ed57a9d"},
        {"skeleton", 576u,
            "0f9757141b0f49ac269a74901050c96d378d71858227e31b58e6aeca0ece0248"},
        {"animation_clip", 496u,
            "c88929b5122c40953854b8828d48b87022a7711b8705e249d2df7145ef0baf50"},
    }};

    [[nodiscard]] asset_id_t id(std::uint32_t ordinal)
    {
        std::array<std::uint8_t, 16> bytes{
            0x31u, 0x90u, 0x14u, 0x00u,
            0x00u, 0x00u, 0x40u, 0x00u,
            0x80u, 0x00u, 0x00u, 0x00u,
            0x00u, 0x00u, 0x00u, 0x00u,
        };
        bytes[12] = static_cast<std::uint8_t>((ordinal >> 24u) & 0xffu);
        bytes[13] = static_cast<std::uint8_t>((ordinal >> 16u) & 0xffu);
        bytes[14] = static_cast<std::uint8_t>((ordinal >> 8u) & 0xffu);
        bytes[15] = static_cast<std::uint8_t>(ordinal & 0xffu);
        return asset_id_t{bytes};
    }

    [[nodiscard]] std::unique_ptr<AssetInfo> info(
        EAssetType type,
        std::uint32_t ordinal
    )
    {
        auto result = std::make_unique<AssetInfo>();
        result->id = id(ordinal);
        result->type = type;
        result->date = 0x0102030405060708ull;
        constexpr std::string_view display{"wire-contract"};
        std::memcpy(
            result->display_name,
            display.data(),
            display.size()
        );
        return result;
    }

    [[nodiscard]] std::vector<std::byte> readAll(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) return {};
        const auto end = input.tellg();
        if (end <= 0) return {};
        std::vector<std::byte> bytes(static_cast<std::size_t>(end));
        input.seekg(0, std::ios::beg);
        input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())
        );
        return input ? bytes : std::vector<std::byte>{};
    }

    [[nodiscard]] std::string sha256(std::span<const std::byte> bytes)
    {
        const auto digest = lux::cxx::algorithm::Sha256::hash(bytes);
        std::array<char, lux::cxx::algorithm::Sha256Digest::hex_size> text{};
        digest.formatHex(text);
        return {text.data(), text.size()};
    }

    [[nodiscard]] std::vector<std::byte> asLegacyV1(
        std::span<const std::byte> current
    )
    {
        if (current.size() < sizeof(AssetFileHeader)) return {};
        AssetFileHeader header{};
        std::memcpy(&header, current.data(), sizeof(header));

        compat::AssetFileHeaderV1 legacy{};
        legacy.magic_number = header.magic_number;
        legacy.version = asset_version_v1;
        legacy.info_offset = sizeof(compat::AssetFileHeaderV1);
        legacy.info_size = header.info_size;
        legacy.data_offset = legacy.info_offset + legacy.info_size;
        legacy.data_size = header.data_size;
        legacy.info.id = header.info.id;
        legacy.info.type = header.info.type;
        legacy.info.date = header.info.date;

        std::vector<std::byte> image(
            sizeof(legacy) + current.size() - sizeof(header)
        );
        std::memcpy(image.data(), &legacy, sizeof(legacy));
        std::memcpy(
            image.data() + sizeof(legacy),
            current.data() + sizeof(header),
            current.size() - sizeof(header)
        );
        return image;
    }

    bool fail(std::string_view fixture, std::string_view message)
    {
        std::cerr << fixture << ": " << message << '\n';
        return false;
    }

    bool verifyFixture(
        const Golden& golden,
        std::unique_ptr<LuxAsset> asset,
        const fs::path& root
    )
    {
        const auto fixture_id = asset->id();
        const auto type = asset->type();
        auto codecs = runtimeAssetCodecCatalog();
        auto manager = std::make_shared<AssetManager>(codecs);
        auto writer = manager->createSerDeser(type, manager);
        if (!writer || !manager->registerAsset(std::move(asset)))
            return fail(golden.name, "cannot register deterministic fixture");

        const auto first_path = root / (std::string(golden.name) + ".luxasset");
        if (writer->exportAsLuxAsset(fixture_id, first_path) !=
            EAssetError::SUCCESS)
        {
            return fail(golden.name, "encode failed");
        }
        const auto first = readAll(first_path);
        if (first.empty()) return fail(golden.name, "encoded image is empty");

        const auto actual_sha = sha256(first);
        if (golden.size == 0u || golden.sha256.empty())
        {
            std::cout << "GOLDEN " << golden.name << " size=" << first.size()
                      << " sha256=" << actual_sha << '\n';
            return fail(golden.name, "golden constants are not frozen");
        }
        if (first.size() != golden.size || actual_sha != golden.sha256)
        {
            std::cerr << "ACTUAL " << golden.name << " size=" << first.size()
                      << " sha256=" << actual_sha << '\n';
            return fail(golden.name, "length/SHA-256 contract changed");
        }

        auto decoded = codecs->decodeAsset(
            lux::cxx::SharedBytes<>::copyOf(first)
        );
        if (!decoded || (*decoded)->id() != fixture_id ||
            (*decoded)->type() != type || !(*decoded)->hasData())
        {
            return fail(
                golden.name,
                "manager-less Catalog decode did not produce a complete asset"
            );
        }

        if (type == EAssetType::MODEL)
        {
            const auto* model = (*decoded)->as<ModelAsset>();
            if (model == nullptr || model->meshAssetIds() != std::vector{id(6u)} ||
                model->materialAssetIds() != std::vector{id(4u)} ||
                model->skeletonAssetId() != std::optional{id(10u)} ||
                model->animationClipAssetIds() != std::vector{id(11u)})
            {
                return fail(golden.name, "runtime Model manifest is incomplete");
            }
        }
        else if (type == EAssetType::SCRIPT)
        {
            const auto* script = (*decoded)->as<ScriptAsset>();
            if (script == nullptr || script->data() == nullptr ||
                script->data()->module_name != "wire.fixture" ||
                script->payload() != std::vector<std::byte>{
                    std::byte{'r'}, std::byte{'e'}, std::byte{'t'},
                    std::byte{'u'}, std::byte{'r'}, std::byte{'n'}})
            {
                return fail(
                    golden.name,
                    "Script description or primary payload is incomplete"
                );
            }
        }
        else if (type == EAssetType::SHADER)
        {
            const auto* shader = (*decoded)->as<ShaderAsset>();
            if (shader == nullptr || shader->data() == nullptr ||
                shader->shaderInfo().entry_points.size() != 1u ||
                shader->data()->size() != 4u)
            {
                return fail(
                    golden.name,
                    "ShaderInfo or SPIR-V payload is incomplete"
                );
            }
        }

        auto decode_manager = std::make_shared<AssetManager>(codecs);
        auto decoder = decode_manager->createSerDeser(type, decode_manager);
        if (!decode_manager->registerAsset(std::move(*decoded)))
            return fail(golden.name, "decoded fixture registration failed");
        const auto second_path = root / (std::string(golden.name) + ".roundtrip");
        if (decoder->exportAsLuxAsset(fixture_id, second_path) !=
            EAssetError::SUCCESS)
        {
            return fail(golden.name, "re-encode failed");
        }
        if (readAll(second_path) != first)
            return fail(golden.name, "decode/re-encode is not byte-identical");

        const auto legacy = asLegacyV1(first);
        auto legacy_manager = std::make_shared<AssetManager>(codecs);
        auto legacy_decoded = codecs->decodeAsset(
            lux::cxx::SharedBytes<>::copyOf(legacy)
        );
        if (!legacy_decoded || (*legacy_decoded)->id() != fixture_id ||
            (*legacy_decoded)->type() != type || !(*legacy_decoded)->hasData())
        {
            return fail(
                golden.name,
                "legacy v1 image is not readable through the Catalog"
            );
        }

        if (type == EAssetType::SCRIPT)
        {
            constexpr payload_tag_t kAuxiliaryTag = 0x545345545855414Cull;
            constexpr std::array kAuxiliaryBytes{
                std::byte{0x21}, std::byte{0x43}, std::byte{0x65}};
            auto with_auxiliary = first;
            const PayloadBlockHeader block{
                kAuxiliaryTag,
                static_cast<std::uint64_t>(kAuxiliaryBytes.size())};
            const auto* block_bytes = reinterpret_cast<const std::byte*>(
                std::addressof(block)
            );
            with_auxiliary.insert(
                with_auxiliary.end(),
                block_bytes,
                block_bytes + sizeof(block)
            );
            with_auxiliary.insert(
                with_auxiliary.end(),
                kAuxiliaryBytes.begin(),
                kAuxiliaryBytes.end()
            );
            auto decoded_with_auxiliary = codecs->decodeAsset(
                lux::cxx::SharedBytes<>::copyOf(with_auxiliary)
            );
            const auto* auxiliary = decoded_with_auxiliary
                ? (*decoded_with_auxiliary)->payload(kAuxiliaryTag)
                : nullptr;
            if (auxiliary == nullptr ||
                *auxiliary != std::vector<std::byte>(
                    kAuxiliaryBytes.begin(), kAuxiliaryBytes.end()))
            {
                return fail(
                    golden.name,
                    "Script auxiliary payload is incomplete"
                );
            }
        }

        auto bad_magic = first;
        bad_magic[0] ^= std::byte{0xff};
        if (decoder->parseLuxAssetMemory(bad_magic.data(), bad_magic.size()))
            return fail(golden.name, "bad magic accepted");

        auto bad_version = first;
        const asset_version_t unsupported = 0xffffffffu;
        std::memcpy(
            bad_version.data() + sizeof(std::uint32_t),
            &unsupported,
            sizeof(unsupported)
        );
        if (decoder->parseLuxAssetMemory(
                bad_version.data(),
                bad_version.size()
            ))
        {
            return fail(golden.name, "bad version accepted");
        }

        auto truncated = first;
        truncated.pop_back();
        if (decoder->parseLuxAssetMemory(truncated.data(), truncated.size()))
            return fail(golden.name, "truncated image accepted");

        // Auxiliary payload tails predate this contract and intentionally ignore
        // an incomplete final block. Freeze that behavior without interpreting
        // the bytes as part of the domain codec.
        auto malformed_tail = first;
        malformed_tail.push_back(std::byte{0x7f});
        if (!decoder->parseLuxAssetMemory(
                malformed_tail.data(),
                malformed_tail.size()
            ))
        {
            return fail(golden.name, "opaque malformed tail changed core decode");
        }
        return true;
    }

    [[nodiscard]] std::unique_ptr<LuxAsset> textureFixture()
    {
        lux::rdesc::TextureInfo texture_info{};
        texture_info.width = 1;
        texture_info.height = 1;
        texture_info.channel = 4;
        texture_info.mip_ranges[0] = {0u, 4u, 1u, 1u};
        const std::array pixels{
            std::byte{0x11}, std::byte{0x22},
            std::byte{0x33}, std::byte{0xff},
        };
        auto texture = lux::rdesc::Texture::copyOf(texture_info, pixels);
        if (!texture) return {};
        auto asset = std::make_unique<TextureAsset>(
            info(EAssetType::TEXTURE, 1u)
        );
        asset->setData(std::make_unique<lux::rdesc::Texture>(
            std::move(*texture)
        ));
        return asset;
    }

    [[nodiscard]] std::unique_ptr<LuxAsset> textureAtlasFixture()
    {
        auto atlas = std::make_unique<lux::rdesc::TextureAtlas>();
        atlas->name = "atlas";
        atlas->texture_uuid = opaqueFromAssetId(id(1u));
        atlas->frames.push_back({
            "idle",
            Eigen::Vector4f{0.0f, 0.0f, 0.5f, 1.0f},
            Eigen::Vector2f{0.25f, 0.75f},
        });
        return std::make_unique<TextureAtlasAsset>(
            info(EAssetType::TEXTURE_ATLAS, 2u),
            std::move(atlas)
        );
    }

    [[nodiscard]] std::unique_ptr<LuxAsset> flipbookFixture()
    {
        auto clip = std::make_unique<lux::rdesc::FlipbookClip>();
        clip->name = "blink";
        clip->atlas_uuid = opaqueFromAssetId(id(2u));
        clip->frames.push_back({0u, 0.125f});
        clip->events.push_back({0u, 7u});
        clip->loop = false;
        return std::make_unique<FlipbookClipAsset>(
            info(EAssetType::FLIPBOOK_CLIP, 3u),
            std::move(clip)
        );
    }

    [[nodiscard]] std::unique_ptr<LuxAsset> materialFixture()
    {
        auto data = std::make_unique<MaterialData>();
        data->parameter_count = 1u;
        data->parameter_defaults[0] = {0.25f, 0.5f, 0.75f, 1.0f};
        data->alpha_mode = 2u;
        data->double_sided = true;
        data->gbuffer_spirv = {0x07230203u, 0x00010000u};
        data->forward_spirv = {0x07230203u, 0x00010001u};
        data->texture_slot_ids[0] = id(1u);
        return std::make_unique<MaterialAsset>(
            info(EAssetType::MATERIAL, 4u),
            std::move(data)
        );
    }

    [[nodiscard]] std::unique_ptr<LuxAsset> materialInstanceFixture()
    {
        auto data = std::make_unique<MaterialInstanceData>();
        data->parent_material_id = id(4u);
        data->param_override_mask = 1u;
        data->params[0][0] = 0.5f;
        data->tex_override_mask = 1u;
        data->texture_slot_ids[0] = id(1u);
        data->render_state_override = 1u;
        data->alpha_mode = 2u;
        data->double_sided = true;
        return std::make_unique<MaterialInstanceAsset>(
            info(EAssetType::MATERIAL_INSTANCE, 5u),
            std::move(data)
        );
    }

    [[nodiscard]] std::unique_ptr<LuxAsset> meshFixture()
    {
        auto mesh = std::make_unique<lux::rdesc::Mesh>();
        lux::rdesc::Vertex vertex{};
        vertex.normal = Eigen::Vector3f::UnitZ();
        vertex.tangent = Eigen::Vector3f::UnitX();
        vertex.bitangent = Eigen::Vector3f::UnitY();
        vertex.bone.bone_ids[0] = -1;
        mesh->vertices = {vertex, vertex, vertex};
        mesh->vertices[1].position.x() = 1.0f;
        mesh->vertices[2].position.y() = 1.0f;
        mesh->indices = {0u, 1u, 2u};
        auto asset = std::make_unique<MeshAsset>(info(EAssetType::MESH, 6u));
        asset->setData(std::move(mesh));
        return asset;
    }

    [[nodiscard]] std::unique_ptr<LuxAsset> modelFixture()
    {
        auto asset = std::make_unique<ModelAsset>(
            info(EAssetType::MODEL, 7u)
        );
        asset->addMeshAssetId(id(6u));
        asset->addMaterialAssetId(id(4u));
        asset->setSkeletonAssetId(id(10u));
        asset->addAnimationClipAssetId(id(11u));
        return asset;
    }

    [[nodiscard]] std::unique_ptr<LuxAsset> scriptFixture()
    {
        auto script = std::make_unique<lux::rdesc::Script>();
        script->module_name = "wire.fixture";
        script->dependencies.push_back({"asset", uuids::to_string(id(1u))});
        script->provenance.compiler_id = "contract";
        script->body = lux::rdesc::LuaSourceScript{"main"};
        auto asset = std::make_unique<ScriptAsset>(
            info(EAssetType::SCRIPT, 8u),
            std::move(script),
            std::vector<std::byte>{
                std::byte{'r'}, std::byte{'e'}, std::byte{'t'},
                std::byte{'u'}, std::byte{'r'}, std::byte{'n'},
            }
        );
        return asset;
    }

    [[nodiscard]] std::unique_ptr<LuxAsset> shaderFixture()
    {
        lux::rdesc::ShaderInfo shader_info{};
        shader_info.entry_points.push_back({
            "main",
            lux::rdesc::EShaderType::FRAGMENT,
        });
        auto asset = std::make_unique<ShaderAsset>(
            info(EAssetType::SHADER, 9u),
            std::move(shader_info)
        );
        asset->setData(std::make_unique<lux::rdesc::Shader>(
            std::vector<std::byte>{
                std::byte{0x03}, std::byte{0x02},
                std::byte{0x23}, std::byte{0x07},
            }
        ));
        return asset;
    }

    [[nodiscard]] std::unique_ptr<LuxAsset> skeletonFixture()
    {
        auto skeleton = std::make_unique<lux::rdesc::Skeleton>();
        lux::rdesc::Bone_t bone{};
        bone.name = "root";
        bone.parent_index = -1;
        bone.bind_local = Eigen::Affine3f::Identity();
        bone.inv_bind_world = Eigen::Affine3f::Identity();
        skeleton->bones.push_back(std::move(bone));
        return std::make_unique<SkeletonAsset>(
            info(EAssetType::SKELETON, 10u),
            std::move(skeleton)
        );
    }

    [[nodiscard]] std::unique_ptr<LuxAsset> animationFixture()
    {
        auto clip = std::make_unique<lux::rdesc::AnimationClip>();
        clip->name = "idle";
        clip->duration = 1.0f;
        clip->loop = true;
        lux::rdesc::BoneTrack track{};
        track.bone_index = 0;
        track.times_t = {0.0f};
        track.translations = {Eigen::Vector3f::Zero()};
        track.times_r = {0.0f};
        track.rotations = {Eigen::Quaternionf::Identity()};
        track.times_s = {0.0f};
        track.scales = {Eigen::Vector3f::Ones()};
        clip->tracks.push_back(std::move(track));
        return std::make_unique<AnimationClipAsset>(
            info(EAssetType::ANIMATION_CLIP, 11u),
            std::move(clip)
        );
    }
}

int main()
{
    const auto nonce = std::chrono::steady_clock::now()
        .time_since_epoch()
        .count();
    const auto root = fs::temp_directory_path() /
        ("lux_asset_wire_contract_" + std::to_string(nonce));
    std::error_code error;
    fs::create_directories(root, error);
    if (error) return 1;

    bool success = true;
    success &= verifyFixture(kGolden[0], textureFixture(), root);
    success &= verifyFixture(kGolden[1], textureAtlasFixture(), root);
    success &= verifyFixture(kGolden[2], flipbookFixture(), root);
    success &= verifyFixture(kGolden[3], materialFixture(), root);
    success &= verifyFixture(kGolden[4], materialInstanceFixture(), root);
    success &= verifyFixture(kGolden[5], meshFixture(), root);
    success &= verifyFixture(kGolden[6], modelFixture(), root);
    success &= verifyFixture(kGolden[7], scriptFixture(), root);
    success &= verifyFixture(kGolden[8], shaderFixture(), root);
    success &= verifyFixture(kGolden[9], skeletonFixture(), root);
    success &= verifyFixture(kGolden[10], animationFixture(), root);

    fs::remove_all(root, error);
    return success ? 0 : 1;
}
