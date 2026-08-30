#include <lux/engine/toolchain/asset/model/ModelCooker.hpp>

#include <lux/engine/toolchain/asset/material/MaterialCooker.hpp>
#include <lux/engine/toolchain/asset/texture/TextureCooker.hpp>

#include <lux/cxx/algorithm/sha256.hpp>

#include <assimp/GltfMaterial.h>
#include <assimp/Importer.hpp>
#include <assimp/anim.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <meshoptimizer.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace lux::toolchain
{
    namespace
    {
        enum class ETextureProfile : std::uint8_t
        {
            BASE_COLOR,
            NORMAL,
            METALLIC_ROUGHNESS,
            OCCLUSION,
            EMISSIVE,
        };

        [[nodiscard]] ModelCookFailure failure(
            EModelCookError code,
            std::uint32_t ordinal,
            std::string detail
        ) noexcept
        {
            return ModelCookFailure{code, ordinal, std::move(detail)};
        }

        [[nodiscard]] Eigen::Affine3f toEigen(const aiMatrix4x4& value) noexcept
        {
            Eigen::Affine3f result;
            result.matrix() <<
                value.a1, value.a2, value.a3, value.a4,
                value.b1, value.b2, value.b3, value.b4,
                value.c1, value.c2, value.c3, value.c4,
                value.d1, value.d2, value.d3, value.d4;
            return result;
        }

        [[nodiscard]] lux::asset::AssetId deriveId(
            lux::asset::AssetId model,
            std::string_view kind,
            std::string_view key
        ) noexcept
        {
            lux::cxx::algorithm::Sha256 hasher;
            hasher.update(model.bytes());
            hasher.update("|");
            hasher.update(kind);
            hasher.update("|");
            hasher.update(key);
            const auto digest = hasher.digest();
            std::array<std::uint8_t, 16U> bytes{};
            for (std::size_t index = 0U; index < bytes.size(); ++index)
                bytes[index] = std::to_integer<std::uint8_t>(digest[index]);
            bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
            bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
            return lux::asset::AssetId{bytes};
        }

        [[nodiscard]] lux::asset::AssetInfo subAssetInfo(
            const lux::asset::AssetInfo& model,
            lux::asset::AssetId id,
            lux::asset::AssetTypeId type,
            std::string_view display_name
        ) noexcept
        {
            auto result = model;
            result.id = id;
            result.type = type;
            result.display_name.fill('\0');
            std::memcpy(
                result.display_name.data(),
                display_name.data(),
                (std::min)(display_name.size(), result.display_name.size() - 1U)
            );
            return result;
        }

        [[nodiscard]] std::optional<lux::cxx::SharedBytes<>> readOwned(
            const std::filesystem::path& path
        )
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream) return std::nullopt;
            const auto end = stream.tellg();
            if (end <= 0 || static_cast<std::uintmax_t>(end) > (std::numeric_limits<std::size_t>::max)())
                return std::nullopt;
            auto storage = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(end));
            stream.seekg(0, std::ios::beg);
            stream.read(reinterpret_cast<char*>(storage->data()), static_cast<std::streamsize>(storage->size()));
            if (!stream) return std::nullopt;
            return lux::cxx::SharedBytes<>::fromOwner(storage, *storage);
        }

        [[nodiscard]] lux::cxx::SharedBytes<> rgbaToTga(
            std::span<const std::uint8_t> rgba,
            int width,
            int height,
            ETextureProfile profile
        )
        {
            auto storage = std::make_shared<std::vector<std::byte>>(
                18U + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U
            );
            auto& bytes = *storage;
            bytes[2] = std::byte{2U};
            bytes[12] = static_cast<std::byte>(width & 0xFF);
            bytes[13] = static_cast<std::byte>((width >> 8) & 0xFF);
            bytes[14] = static_cast<std::byte>(height & 0xFF);
            bytes[15] = static_cast<std::byte>((height >> 8) & 0xFF);
            bytes[16] = std::byte{32U};
            bytes[17] = std::byte{0x28U};
            for (std::size_t pixel = 0U; pixel < rgba.size() / 4U; ++pixel)
            {
                std::uint8_t red = rgba[pixel * 4U + 0U];
                std::uint8_t green = rgba[pixel * 4U + 1U];
                std::uint8_t blue = rgba[pixel * 4U + 2U];
                std::uint8_t alpha = rgba[pixel * 4U + 3U];
                if (profile == ETextureProfile::METALLIC_ROUGHNESS)
                {
                    red = blue;
                    blue = 0U;
                    alpha = 255U;
                }
                else if (profile == ETextureProfile::OCCLUSION)
                {
                    green = 0U;
                    blue = 0U;
                    alpha = 255U;
                }
                const std::size_t offset = 18U + pixel * 4U;
                bytes[offset + 0U] = static_cast<std::byte>(blue);
                bytes[offset + 1U] = static_cast<std::byte>(green);
                bytes[offset + 2U] = static_cast<std::byte>(red);
                bytes[offset + 3U] = static_cast<std::byte>(alpha);
            }
            return lux::cxx::SharedBytes<>::fromOwner(storage, *storage);
        }

        [[nodiscard]] std::optional<lux::cxx::SharedBytes<>> repackImage(
            lux::cxx::SharedBytes<> image,
            ETextureProfile profile
        )
        {
            if (profile != ETextureProfile::METALLIC_ROUGHNESS && profile != ETextureProfile::OCCLUSION)
                return image;
            if (image.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
                return std::nullopt;
            int width{};
            int height{};
            int channels{};
            std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded{
                stbi_load_from_memory(
                    reinterpret_cast<const stbi_uc*>(image.data()),
                    static_cast<int>(image.size()),
                    &width,
                    &height,
                    &channels,
                    STBI_rgb_alpha
                ),
                &stbi_image_free
            };
            if (!decoded || width <= 0 || height <= 0) return std::nullopt;
            const auto count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
            return rgbaToTga({decoded.get(), count}, width, height, profile);
        }

        [[nodiscard]] TextureCookConfiguration textureConfiguration(ETextureProfile profile) noexcept
        {
            using Format = lux::rdesc::ETexturePixelFormat;
            using Color = lux::rdesc::ETextureColorSpace;
            switch (profile)
            {
            case ETextureProfile::NORMAL:
                return {Format::BC5_UNORM, Color::DATA, false};
            case ETextureProfile::METALLIC_ROUGHNESS:
                return {Format::RG8_UNORM, Color::DATA, false};
            case ETextureProfile::OCCLUSION:
                return {Format::R8_UNORM, Color::DATA, false};
            case ETextureProfile::BASE_COLOR:
            case ETextureProfile::EMISSIVE:
                return {Format::BC7_SRGB, Color::SRGB, false};
            }
            return {Format::BC7_SRGB, Color::SRGB, false};
        }

        [[nodiscard]] const char* profileName(ETextureProfile profile) noexcept
        {
            switch (profile)
            {
            case ETextureProfile::BASE_COLOR: return "base-color";
            case ETextureProfile::NORMAL: return "normal";
            case ETextureProfile::METALLIC_ROUGHNESS: return "metallic-roughness";
            case ETextureProfile::OCCLUSION: return "occlusion";
            case ETextureProfile::EMISSIVE: return "emissive";
            }
            return "texture";
        }

        [[nodiscard]] bool looksLikePbr(const aiMaterial& material) noexcept
        {
            aiString path;
            if (material.GetTexture(aiTextureType_BASE_COLOR, 0U, &path) == AI_SUCCESS ||
                material.GetTexture(aiTextureType_METALNESS, 0U, &path) == AI_SUCCESS ||
                material.GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0U, &path) == AI_SUCCESS)
            {
                return true;
            }
            aiColor4D base{};
            return aiGetMaterialColor(&material, AI_MATKEY_BASE_COLOR, &base) == AI_SUCCESS;
        }

        struct CookContext final
        {
            const aiScene& scene;
            const std::filesystem::path& source;
            const ModelCookConfiguration& configuration;
            const lux::asset::AssetInfo& model_info;
            ModelCookProduct product;
            std::unordered_map<std::string, std::size_t> texture_by_key;
            std::unordered_map<std::string, std::int32_t> bone_by_name;
            Eigen::Affine3f import_transform{Eigen::Affine3f::Identity()};
            Eigen::Matrix3f import_rotation{Eigen::Matrix3f::Identity()};
            bool has_import_transform{};
        };

        [[nodiscard]] lux::cxx::expected<lux::cxx::SharedBytes<>, ModelCookFailure> textureSource(
            const CookContext& context,
            const aiString& texture_path,
            std::string& stable_key
        )
        {
            if (const aiTexture* embedded = context.scene.GetEmbeddedTexture(texture_path.C_Str()))
            {
                stable_key = "embedded:" + std::string{texture_path.C_Str()};
                if (embedded->mHeight == 0U)
                {
                    return lux::cxx::SharedBytes<>::copyOf({
                        reinterpret_cast<const std::byte*>(embedded->pcData),
                        embedded->mWidth
                    });
                }
                const std::size_t pixel_count =
                    static_cast<std::size_t>(embedded->mWidth) * embedded->mHeight;
                std::vector<std::uint8_t> rgba(pixel_count * 4U);
                for (std::size_t pixel = 0U; pixel < pixel_count; ++pixel)
                {
                    rgba[pixel * 4U + 0U] = embedded->pcData[pixel].r;
                    rgba[pixel * 4U + 1U] = embedded->pcData[pixel].g;
                    rgba[pixel * 4U + 2U] = embedded->pcData[pixel].b;
                    rgba[pixel * 4U + 3U] = embedded->pcData[pixel].a;
                }
                return rgbaToTga(
                    rgba,
                    static_cast<int>(embedded->mWidth),
                    static_cast<int>(embedded->mHeight),
                    ETextureProfile::BASE_COLOR
                );
            }

            const auto relative = std::filesystem::path{texture_path.C_Str()}.lexically_normal();
            stable_key = relative.generic_string();
            auto bytes = readOwned(context.source.parent_path() / relative);
            if (!bytes)
            {
                return lux::cxx::unexpected(failure(
                    EModelCookError::IO_FAILURE,
                    0U,
                    "cannot read external texture: " + stable_key
                ));
            }
            return *bytes;
        }

        [[nodiscard]] lux::cxx::expected<
            std::optional<ImportedTextureReference>,
            ModelCookFailure
        > cookTextureReference(
            CookContext& context,
            const aiMaterial& material,
            aiTextureType type,
            ETextureProfile profile
        )
        {
            aiString path;
            if (material.GetTexture(type, 0U, &path) != AI_SUCCESS)
                return std::optional<ImportedTextureReference>{};

            std::string stable_key;
            auto source = textureSource(context, path, stable_key);
            if (!source) return lux::cxx::unexpected(source.error());
            const std::string cache_key = stable_key + "|" + profileName(profile);
            if (const auto found = context.texture_by_key.find(cache_key);
                found != context.texture_by_key.end())
            {
                const auto& asset = context.product.textures[found->second];
                return std::optional<ImportedTextureReference>{ImportedTextureReference{
                    asset->id(),
                    textureConfiguration(profile).color_space
                }};
            }

            auto cook_source = repackImage(*source, profile);
            if (!cook_source)
            {
                return lux::cxx::unexpected(failure(
                    EModelCookError::TEXTURE_COOK_FAILED,
                    0U,
                    "cannot repack texture: " + stable_key
                ));
            }
            const auto texture_id = deriveId(context.model_info.id, "texture", cache_key);
            auto cooked = cookTexture(
                subAssetInfo(
                    context.model_info,
                    texture_id,
                    lux::asset::TextureAsset::asset_type,
                    std::filesystem::path{stable_key}.stem().string()
                ),
                *cook_source,
                textureConfiguration(profile)
            );
            if (!cooked)
            {
                return lux::cxx::unexpected(failure(
                    EModelCookError::TEXTURE_COOK_FAILED,
                    0U,
                    "TextureCooker rejected: " + stable_key
                ));
            }
            const std::size_t index = context.product.textures.size();
            context.texture_by_key.emplace(cache_key, index);
            context.product.textures.push_back(*cooked);
            return std::optional<ImportedTextureReference>{ImportedTextureReference{
                texture_id,
                textureConfiguration(profile).color_space
            }};
        }

        [[nodiscard]] lux::cxx::expected<void, ModelCookFailure> cookMaterials(CookContext& context)
        {
            context.product.materials.reserve(context.scene.mNumMaterials);
            for (std::uint32_t ordinal = 0U; ordinal < context.scene.mNumMaterials; ++ordinal)
            {
                const aiMaterial& source = *context.scene.mMaterials[ordinal];
                ImportedMaterialDescription imported;
                if (looksLikePbr(source))
                {
                    aiColor4D base{};
                    if (aiGetMaterialColor(&source, AI_MATKEY_BASE_COLOR, &base) == AI_SUCCESS)
                    {
                        imported.base_color = {base.r, base.g, base.b};
                        imported.opacity = base.a;
                    }
                    aiGetMaterialFloat(&source, AI_MATKEY_METALLIC_FACTOR, &imported.metallic);
                    aiGetMaterialFloat(&source, AI_MATKEY_ROUGHNESS_FACTOR, &imported.roughness);
                }
                else
                {
                    imported.legacy_lit = true;
                    aiColor4D diffuse{};
                    if (aiGetMaterialColor(&source, AI_MATKEY_COLOR_DIFFUSE, &diffuse) == AI_SUCCESS)
                    {
                        imported.base_color = {diffuse.r, diffuse.g, diffuse.b};
                        imported.opacity = diffuse.a;
                    }
                }

                aiColor4D emissive{};
                if (aiGetMaterialColor(&source, AI_MATKEY_COLOR_EMISSIVE, &emissive) == AI_SUCCESS)
                    imported.emissive = {emissive.r, emissive.g, emissive.b};
                int double_sided{};
                aiGetMaterialInteger(&source, AI_MATKEY_TWOSIDED, &double_sided);
                imported.double_sided = double_sided != 0;
                aiString alpha_mode;
                if (aiGetMaterialString(&source, AI_MATKEY_GLTF_ALPHAMODE, &alpha_mode) == AI_SUCCESS)
                {
                    const std::string_view value{alpha_mode.C_Str(), alpha_mode.length};
                    if (value == "MASK") imported.alpha_mode = lux::rdesc::EAlphaMode::Mask;
                    else if (value == "BLEND") imported.alpha_mode = lux::rdesc::EAlphaMode::Blend;
                }
                aiGetMaterialFloat(&source, AI_MATKEY_GLTF_ALPHACUTOFF, &imported.alpha_cutoff);

                auto base_color = cookTextureReference(
                    context,
                    source,
                    looksLikePbr(source) ? aiTextureType_BASE_COLOR : aiTextureType_DIFFUSE,
                    ETextureProfile::BASE_COLOR
                );
                if (!base_color) return lux::cxx::unexpected(base_color.error());
                imported.base_color_texture = *base_color;

                auto normal = cookTextureReference(
                    context,
                    source,
                    aiTextureType_NORMALS,
                    ETextureProfile::NORMAL
                );
                if (!normal) return lux::cxx::unexpected(normal.error());
                imported.normal_texture = *normal;

                auto metallic_roughness = cookTextureReference(
                    context,
                    source,
                    aiTextureType_METALNESS,
                    ETextureProfile::METALLIC_ROUGHNESS
                );
                if (!metallic_roughness) return lux::cxx::unexpected(metallic_roughness.error());
                if (!*metallic_roughness)
                {
                    metallic_roughness = cookTextureReference(
                        context,
                        source,
                        aiTextureType_DIFFUSE_ROUGHNESS,
                        ETextureProfile::METALLIC_ROUGHNESS
                    );
                    if (!metallic_roughness) return lux::cxx::unexpected(metallic_roughness.error());
                }
                imported.metallic_roughness_texture = *metallic_roughness;

                auto occlusion = cookTextureReference(
                    context,
                    source,
                    aiTextureType_AMBIENT_OCCLUSION,
                    ETextureProfile::OCCLUSION
                );
                if (!occlusion) return lux::cxx::unexpected(occlusion.error());
                imported.occlusion_texture = *occlusion;

                auto emissive_texture = cookTextureReference(
                    context,
                    source,
                    aiTextureType_EMISSIVE,
                    ETextureProfile::EMISSIVE
                );
                if (!emissive_texture) return lux::cxx::unexpected(emissive_texture.error());
                imported.emissive_texture = *emissive_texture;

                aiString source_name;
                aiGetMaterialString(&source, AI_MATKEY_NAME, &source_name);
                const std::string key = std::to_string(ordinal);
                const auto material_id = deriveId(context.model_info.id, "material", key);
                auto material = cookImportedMaterial(
                    subAssetInfo(
                        context.model_info,
                        material_id,
                        lux::asset::MaterialAsset::asset_type,
                        source_name.length == 0U ? "material" : source_name.C_Str()
                    ),
                    imported
                );
                if (!material)
                {
                    return lux::cxx::unexpected(failure(
                        EModelCookError::MATERIAL_COOK_FAILED,
                        ordinal,
                        material.error().detail
                    ));
                }
                context.product.materials.push_back(*material);
            }
            return {};
        }

        [[nodiscard]] lux::cxx::expected<void, ModelCookFailure> cookSkeleton(CookContext& context)
        {
            std::unordered_map<std::string, aiMatrix4x4> offsets;
            for (std::uint32_t mesh = 0U; mesh < context.scene.mNumMeshes; ++mesh)
            {
                const aiMesh& source = *context.scene.mMeshes[mesh];
                for (std::uint32_t bone = 0U; bone < source.mNumBones; ++bone)
                    offsets.try_emplace(source.mBones[bone]->mName.C_Str(), source.mBones[bone]->mOffsetMatrix);
            }
            if (offsets.empty()) return {};
            if (context.configuration.make_left_handed)
            {
                return lux::cxx::unexpected(failure(
                    EModelCookError::UNSUPPORTED_FEATURE,
                    0U,
                    "left-handed skinned import is not qualified"
                ));
            }

            auto skeleton = std::make_shared<lux::rdesc::Skeleton>();
            skeleton->bones.reserve(offsets.size());
            bool first_bone{};
            std::function<void(const aiNode*, std::int32_t, const Eigen::Affine3f&)> walk =
                [&](const aiNode* node, std::int32_t parent, const Eigen::Affine3f& accumulated)
            {
                std::int32_t self = -1;
                const std::string name = node->mName.C_Str();
                if (const auto found = offsets.find(name); found != offsets.end())
                {
                    auto local = toEigen(node->mTransformation);
                    if (!first_bone)
                    {
                        skeleton->global_transform = accumulated;
                        first_bone = true;
                    }
                    else if (!accumulated.matrix().isApprox(Eigen::Matrix4f::Identity()))
                    {
                        local = accumulated * local;
                    }
                    self = static_cast<std::int32_t>(skeleton->bones.size());
                    skeleton->bones.push_back({name, parent, local, toEigen(found->second)});
                    context.bone_by_name.emplace(name, self);
                }
                const auto child_parent = self >= 0 ? self : parent;
                const Eigen::Affine3f child_accumulated =
                    (self >= 0 || node == context.scene.mRootNode)
                        ? Eigen::Affine3f::Identity()
                        : Eigen::Affine3f{accumulated * toEigen(node->mTransformation)};
                for (std::uint32_t child = 0U; child < node->mNumChildren; ++child)
                    walk(node->mChildren[child], child_parent, child_accumulated);
            };
            walk(context.scene.mRootNode, -1, Eigen::Affine3f::Identity());
            if (skeleton->bones.empty())
            {
                return lux::cxx::unexpected(failure(
                    EModelCookError::INVALID_SKELETON,
                    0U,
                    "mesh bones have no matching scene nodes"
                ));
            }
            if (context.has_import_transform)
                skeleton->global_transform = context.import_transform * skeleton->global_transform;

            const auto skeleton_id = deriveId(context.model_info.id, "skeleton", "root");
            auto asset = lux::asset::SkeletonAsset::create(
                subAssetInfo(
                    context.model_info,
                    skeleton_id,
                    lux::asset::SkeletonAsset::asset_type,
                    "skeleton"
                ),
                std::move(skeleton)
            );
            if (!asset)
            {
                return lux::cxx::unexpected(failure(
                    EModelCookError::INVALID_SKELETON,
                    0U,
                    "typed SkeletonAsset validation failed"
                ));
            }
            context.product.skeleton = *asset;
            return {};
        }

        void fillBoneInfluences(
            const CookContext& context,
            const aiMesh& mesh,
            std::vector<lux::rdesc::Vertex>& vertices
        )
        {
            std::vector<std::vector<std::pair<std::int32_t, float>>> influences(mesh.mNumVertices);
            for (std::uint32_t bone = 0U; bone < mesh.mNumBones; ++bone)
            {
                const auto found = context.bone_by_name.find(mesh.mBones[bone]->mName.C_Str());
                if (found == context.bone_by_name.end()) continue;
                for (std::uint32_t weight = 0U; weight < mesh.mBones[bone]->mNumWeights; ++weight)
                {
                    const auto value = mesh.mBones[bone]->mWeights[weight];
                    if (value.mVertexId < vertices.size() && value.mWeight > 0.0F)
                        influences[value.mVertexId].push_back({found->second, value.mWeight});
                }
            }
            for (std::size_t vertex = 0U; vertex < vertices.size(); ++vertex)
            {
                auto& values = influences[vertex];
                std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) noexcept {
                    return left.second > right.second;
                });
                const std::size_t count = (std::min)(
                    values.size(),
                    static_cast<std::size_t>(lux::rdesc::max_bone_influence)
                );
                float sum{};
                for (std::size_t index = 0U; index < count; ++index) sum += values[index].second;
                if (sum <= 0.0F) continue;
                for (std::size_t index = 0U; index < count; ++index)
                {
                    vertices[vertex].bone.bone_ids[index] = values[index].first;
                    vertices[vertex].bone.weights[index] = values[index].second / sum;
                }
            }
        }

        [[nodiscard]] lux::cxx::expected<void, ModelCookFailure> cookMeshes(CookContext& context)
        {
            context.product.meshes.reserve(context.scene.mNumMeshes);
            for (std::uint32_t ordinal = 0U; ordinal < context.scene.mNumMeshes; ++ordinal)
            {
                const aiMesh& source = *context.scene.mMeshes[ordinal];
                if (source.mNumVertices == 0U || source.mNumFaces == 0U ||
                    source.mMaterialIndex >= context.product.materials.size())
                {
                    return lux::cxx::unexpected(failure(
                        EModelCookError::INVALID_MESH,
                        ordinal,
                        "mesh has no geometry or references an invalid material"
                    ));
                }
                auto mesh = std::make_shared<lux::rdesc::Mesh>();
                mesh->vertices.resize(source.mNumVertices);
                for (std::uint32_t vertex = 0U; vertex < source.mNumVertices; ++vertex)
                {
                    auto& destination = mesh->vertices[vertex];
                    destination.position = {
                        source.mVertices[vertex].x,
                        source.mVertices[vertex].y,
                        source.mVertices[vertex].z
                    };
                    if (source.mNormals)
                    {
                        destination.normal = {
                            source.mNormals[vertex].x,
                            source.mNormals[vertex].y,
                            source.mNormals[vertex].z
                        };
                    }
                    if (source.mTextureCoords[0])
                    {
                        destination.uv = {
                            source.mTextureCoords[0][vertex].x,
                            source.mTextureCoords[0][vertex].y
                        };
                    }
                    if (source.mTangents)
                    {
                        destination.tangent = {
                            source.mTangents[vertex].x,
                            source.mTangents[vertex].y,
                            source.mTangents[vertex].z
                        };
                    }
                    if (source.mBitangents)
                    {
                        destination.bitangent = {
                            source.mBitangents[vertex].x,
                            source.mBitangents[vertex].y,
                            source.mBitangents[vertex].z
                        };
                    }
                    for (std::uint8_t influence = 0U; influence < lux::rdesc::max_bone_influence; ++influence)
                    {
                        destination.bone.bone_ids[influence] = -1;
                        destination.bone.weights[influence] = 0.0F;
                    }
                }
                if (source.HasBones()) fillBoneInfluences(context, source, mesh->vertices);
                if (context.has_import_transform && !source.HasBones())
                {
                    for (auto& vertex : mesh->vertices)
                    {
                        vertex.position = context.import_transform * vertex.position;
                        vertex.normal = (context.import_rotation * vertex.normal).normalized();
                        vertex.tangent = context.import_rotation * vertex.tangent;
                        vertex.bitangent = context.import_rotation * vertex.bitangent;
                    }
                }

                mesh->indices.reserve(static_cast<std::size_t>(source.mNumFaces) * 3U);
                for (std::uint32_t face = 0U; face < source.mNumFaces; ++face)
                {
                    if (source.mFaces[face].mNumIndices != 3U)
                    {
                        return lux::cxx::unexpected(failure(
                            EModelCookError::INVALID_MESH,
                            ordinal,
                            "triangulation did not produce triangles"
                        ));
                    }
                    mesh->indices.insert(
                        mesh->indices.end(),
                        source.mFaces[face].mIndices,
                        source.mFaces[face].mIndices + 3U
                    );
                }
                lux::math::AABB bounds;
                for (const auto& vertex : mesh->vertices) bounds.merge(vertex.position);
                mesh->bounds = bounds;

                constexpr std::size_t minimum_lod_indices = 768U;
                if (!source.HasBones() && mesh->indices.size() >= minimum_lod_indices)
                {
                    constexpr std::array ratios{0.5F, 0.25F, 0.1F};
                    constexpr float maximum_error = 0.05F;
                    std::size_t previous = mesh->indices.size();
                    const float* positions = mesh->vertices.front().position.data();
                    for (const float ratio : ratios)
                    {
                        const auto target = (std::max)(
                            std::size_t{3U},
                            static_cast<std::size_t>(mesh->indices.size() * ratio)
                        );
                        std::vector<std::uint32_t> indices(mesh->indices.size());
                        float error{};
                        const auto count = meshopt_simplify(
                            indices.data(),
                            mesh->indices.data(),
                            mesh->indices.size(),
                            positions,
                            mesh->vertices.size(),
                            sizeof(lux::rdesc::Vertex),
                            target,
                            maximum_error,
                            0U,
                            &error
                        );
                        if (count == 0U || count >= previous) break;
                        indices.resize(count);
                        mesh->lods.push_back({std::move(indices), error});
                        previous = count;
                    }
                }

                const std::string key = std::to_string(ordinal);
                const auto mesh_id = deriveId(context.model_info.id, "mesh", key);
                auto asset = lux::asset::MeshAsset::create(
                    subAssetInfo(
                        context.model_info,
                        mesh_id,
                        lux::asset::MeshAsset::asset_type,
                        source.mName.length == 0U ? "mesh" : source.mName.C_Str()
                    ),
                    std::move(mesh)
                );
                if (!asset)
                {
                    return lux::cxx::unexpected(failure(
                        EModelCookError::INVALID_MESH,
                        ordinal,
                        "typed MeshAsset validation failed"
                    ));
                }
                context.product.meshes.push_back(*asset);
            }
            return {};
        }

        [[nodiscard]] lux::cxx::expected<void, ModelCookFailure> cookAnimations(CookContext& context)
        {
            if (!context.configuration.import_animations || !context.product.skeleton)
                return {};
            context.product.animations.reserve(context.scene.mNumAnimations);
            for (std::uint32_t ordinal = 0U; ordinal < context.scene.mNumAnimations; ++ordinal)
            {
                const aiAnimation& source = *context.scene.mAnimations[ordinal];
                const float ticks_per_second = source.mTicksPerSecond > 0.0
                    ? static_cast<float>(source.mTicksPerSecond)
                    : 25.0F;
                const float seconds_per_tick = 1.0F / ticks_per_second;
                auto clip = std::make_shared<lux::rdesc::AnimationClip>();
                clip->name = source.mName.length == 0U
                    ? "animation-" + std::to_string(ordinal)
                    : source.mName.C_Str();
                clip->duration = static_cast<float>(source.mDuration) * seconds_per_tick;
                clip->loop = true;
                for (std::uint32_t channel = 0U; channel < source.mNumChannels; ++channel)
                {
                    const aiNodeAnim& input = *source.mChannels[channel];
                    const auto bone = context.bone_by_name.find(input.mNodeName.C_Str());
                    if (bone == context.bone_by_name.end()) continue;
                    lux::rdesc::BoneTrack track;
                    track.bone_index = bone->second;
                    for (std::uint32_t key = 0U; key < input.mNumPositionKeys; ++key)
                    {
                        const float time = static_cast<float>(input.mPositionKeys[key].mTime) * seconds_per_tick;
                        if (!track.times_t.empty() && time <= track.times_t.back()) continue;
                        track.times_t.push_back(time);
                        track.translations.emplace_back(
                            input.mPositionKeys[key].mValue.x,
                            input.mPositionKeys[key].mValue.y,
                            input.mPositionKeys[key].mValue.z
                        );
                    }
                    for (std::uint32_t key = 0U; key < input.mNumRotationKeys; ++key)
                    {
                        const float time = static_cast<float>(input.mRotationKeys[key].mTime) * seconds_per_tick;
                        if (!track.times_r.empty() && time <= track.times_r.back()) continue;
                        track.times_r.push_back(time);
                        Eigen::Quaternionf value{
                            input.mRotationKeys[key].mValue.w,
                            input.mRotationKeys[key].mValue.x,
                            input.mRotationKeys[key].mValue.y,
                            input.mRotationKeys[key].mValue.z
                        };
                        track.rotations.push_back(value.normalized());
                    }
                    for (std::uint32_t key = 0U; key < input.mNumScalingKeys; ++key)
                    {
                        const float time = static_cast<float>(input.mScalingKeys[key].mTime) * seconds_per_tick;
                        if (!track.times_s.empty() && time <= track.times_s.back()) continue;
                        track.times_s.push_back(time);
                        track.scales.emplace_back(
                            input.mScalingKeys[key].mValue.x,
                            input.mScalingKeys[key].mValue.y,
                            input.mScalingKeys[key].mValue.z
                        );
                    }
                    if (!track.times_t.empty() || !track.times_r.empty() || !track.times_s.empty())
                        clip->tracks.push_back(std::move(track));
                }
                if (clip->tracks.empty()) continue;
                if (clip->duration <= 0.0F)
                {
                    for (const auto& track : clip->tracks)
                    {
                        if (!track.times_t.empty()) clip->duration = (std::max)(clip->duration, track.times_t.back());
                        if (!track.times_r.empty()) clip->duration = (std::max)(clip->duration, track.times_r.back());
                        if (!track.times_s.empty()) clip->duration = (std::max)(clip->duration, track.times_s.back());
                    }
                }
                if (clip->duration <= 0.0F) clip->duration = 1.0F;

                const std::string key = std::to_string(ordinal) + "|" + clip->name;
                const auto animation_id = deriveId(context.model_info.id, "animation", key);
                const std::string display_name = clip->name;
                auto asset = lux::asset::AnimationClipAsset::create(
                    subAssetInfo(
                        context.model_info,
                        animation_id,
                        lux::asset::AnimationClipAsset::asset_type,
                        display_name
                    ),
                    std::move(clip)
                );
                if (!asset)
                {
                    return lux::cxx::unexpected(failure(
                        EModelCookError::INVALID_ANIMATION,
                        ordinal,
                        "typed AnimationClipAsset validation failed"
                    ));
                }
                context.product.animations.push_back(*asset);
            }
            return {};
        }

        void buildNodes(
            const aiNode& source,
            lux::rdesc::ModelDescription& model
        )
        {
            const auto node_index = static_cast<std::uint32_t>(model.nodes.size());
            model.nodes.push_back({});
            model.nodes[node_index].local_transform = toEigen(source.mTransformation);
            model.nodes[node_index].primitives.assign(source.mMeshes, source.mMeshes + source.mNumMeshes);
            for (std::uint32_t child = 0U; child < source.mNumChildren; ++child)
            {
                const auto child_index = static_cast<std::uint32_t>(model.nodes.size());
                buildNodes(*source.mChildren[child], model);
                model.nodes[node_index].children.push_back(child_index);
            }
        }
    } // namespace

    lux::cxx::expected<ModelCookProduct, ModelCookFailure> cookModel(
        lux::asset::AssetInfo model_info,
        const std::filesystem::path& source,
        const ModelCookConfiguration& configuration
    ) noexcept
    {
        const bool invalid_rotation = !configuration.pre_rotation.coeffs().allFinite() ||
            configuration.pre_rotation.norm() <= 1.0e-6F;
        if (model_info.id.isNull() || source.empty())
            return lux::cxx::unexpected(failure(EModelCookError::INVALID_SOURCE, 0U, "invalid model source"));
        if (invalid_rotation || !std::isfinite(configuration.uniform_scale) || configuration.uniform_scale <= 0.0F)
        {
            return lux::cxx::unexpected(failure(
                EModelCookError::INVALID_CONFIGURATION,
                0U,
                "invalid model cook configuration"
            ));
        }
        try
        {
            Assimp::Importer importer;
            unsigned flags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace | aiProcess_FlipUVs;
            if (configuration.make_left_handed) flags |= aiProcess_MakeLeftHanded;
            const aiScene* scene = importer.ReadFile(source.string(), flags);
            if (scene == nullptr || scene->mRootNode == nullptr ||
                (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0U)
            {
                return lux::cxx::unexpected(failure(
                    EModelCookError::IMPORT_FAILED,
                    0U,
                    importer.GetErrorString()
                ));
            }

            CookContext context{*scene, source, configuration, model_info};
            context.import_rotation = configuration.pre_rotation.normalized().toRotationMatrix();
            context.import_transform.linear() = configuration.uniform_scale * context.import_rotation;
            context.has_import_transform =
                !context.import_transform.linear().isApprox(Eigen::Matrix3f::Identity());

            if (auto result = cookMaterials(context); !result)
                return lux::cxx::unexpected(result.error());
            if (auto result = cookSkeleton(context); !result)
                return lux::cxx::unexpected(result.error());
            if (auto result = cookMeshes(context); !result)
                return lux::cxx::unexpected(result.error());
            if (auto result = cookAnimations(context); !result)
                return lux::cxx::unexpected(result.error());

            auto description = std::make_shared<lux::rdesc::ModelDescription>();
            description->primitives.reserve(scene->mNumMeshes);
            for (std::uint32_t mesh = 0U; mesh < scene->mNumMeshes; ++mesh)
            {
                const auto material = scene->mMeshes[mesh]->mMaterialIndex;
                if (material >= context.product.materials.size())
                {
                    return lux::cxx::unexpected(failure(
                        EModelCookError::INVALID_MODEL,
                        mesh,
                        "primitive references an invalid material"
                    ));
                }
                description->primitives.push_back({
                    context.product.meshes[mesh]->id(),
                    context.product.materials[material]->id()
                });
            }
            buildNodes(*scene->mRootNode, *description);
            if (context.product.skeleton)
                description->skeleton = (*context.product.skeleton)->id();
            for (const auto& animation : context.product.animations)
                description->animations.push_back(animation->id());

            model_info.type = lux::asset::ModelAsset::asset_type;
            auto model = lux::asset::ModelAsset::create(std::move(model_info), std::move(description));
            if (!model)
            {
                return lux::cxx::unexpected(failure(
                    EModelCookError::INVALID_MODEL,
                    0U,
                    "typed ModelAsset validation failed"
                ));
            }
            context.product.model = *model;
            return std::move(context.product);
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(
                EModelCookError::ALLOCATION_FAILURE,
                0U,
                "allocation failure"
            ));
        }
        catch (...)
        {
            return lux::cxx::unexpected(failure(
                EModelCookError::IMPORT_FAILED,
                0U,
                "foreign importer failure"
            ));
        }
    }
} // namespace lux::toolchain
