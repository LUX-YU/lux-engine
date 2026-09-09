#include <lux/engine/material/Cooker.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <Eigen/Geometry>
#include <array>
#include <cstdio>
#include <cstring>

namespace
{
    lux::asset::AssetInfo info(std::uint8_t tail, lux::asset::AssetTypeId type, const char* name)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[0] = 0x53;
        bytes[1] = 0x56;
        bytes[2] = 1;
        bytes.back() = tail;
        lux::asset::AssetInfo result;
        result.id = lux::asset::AssetId{bytes};
        result.type = type;
        std::strncpy(result.display_name.data(), name, result.display_name.size() - 1);
        return result;
    }

    void face(lux::rdesc::Mesh& mesh, Eigen::Vector3f normal, Eigen::Vector3f center, float size)
    {
        const Eigen::Vector3f tangent = std::abs(normal.y()) > 0.5F ?
            Eigen::Vector3f::UnitX().eval() : Eigen::Vector3f::UnitY().eval();
        const Eigen::Vector3f bitangent = normal.cross(tangent);
        const std::array<Eigen::Vector2f, 4> corners{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
        const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
        for (const auto& corner : corners)
        {
            lux::rdesc::Vertex vertex{};
            vertex.position = center + size * (corner.x() * tangent + corner.y() * bitangent);
            vertex.normal = normal;
            vertex.tangent = tangent;
            vertex.bitangent = bitangent;
            vertex.uv = (corner + Eigen::Vector2f::Ones()) * 0.5F;
            for (auto& bone : vertex.bone.bone_ids)
                bone = -1;
            mesh.vertices.push_back(vertex);
        }
        for (const auto index : {0U, 1U, 2U, 0U, 2U, 3U})
            mesh.indices.push_back(first + index);
    }

    template<class Asset>
    bool append(std::vector<lux::asset::PakWriteEntry>& entries, const Asset& asset, std::string path)
    {
        auto bytes = lux::asset::TAssetSerDeser<Asset>::encode(asset, lux::asset::AssetEncodeLimits{16 * 1024 * 1024});
        if (!bytes)
            return false;
        auto owner = std::make_shared<const std::vector<std::byte>>(std::move(*bytes));
        auto shared = lux::cxx::SharedBytes<>::fromOwner(owner, std::span<const std::byte>{*owner});
        entries.push_back({asset.id(), Asset::primary_magic, std::move(path), {}, std::move(shared)});
        return true;
    }
}

int main(int argc, char** argv)
{
    if (argc != 2 && !(argc == 3 && std::string_view{argv[2]} == "--omit-ground"))
    {
        std::fprintf(stderr, "usage: lux_scene_seed output.luxpak [--omit-ground]\n");
        return 2;
    }
    try
    {
        std::vector<lux::asset::PakWriteEntry> entries;
        for (int model = 0; model < 2; ++model)
        {
            if (model == 1 && argc == 3)
                continue;
            auto mesh = std::make_shared<lux::rdesc::Mesh>();
            if (model == 0)
            {
                for (int axis = 0; axis < 3; ++axis)
                    for (const float sign : {-1.0F, 1.0F})
                    {
                        Eigen::Vector3f normal = Eigen::Vector3f::Zero();
                        normal[axis] = sign;
                        face(*mesh, normal, normal * 0.5F, 0.5F);
                    }
                mesh->bounds = lux::math::AABB{{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}};
            }
            else
            {
                face(*mesh, {0, 1, 0}, {0, 0, 0}, 6.0F);
                mesh->bounds = lux::math::AABB{{-6, 0, -6}, {6, 0, 6}};
            }
            const auto name = model == 0 ? "Meshes/Cube" : "Meshes/Ground";
            auto asset = lux::asset::MeshAsset::create(info(10 + model, lux::asset::MeshAsset::asset_type, name), mesh);
            if (!asset || !append(entries, **asset, name))
                return 1;
        }
        const std::array<Eigen::Vector3f, 3> colors{{{0.8F, 0.15F, 0.06F}, {0.08F, 0.35F, 0.8F}, {0.4F, 0.45F, 0.4F}}};
        for (std::uint8_t index = 0; index < colors.size(); ++index)
        {
            lux::material::ImportedMaterialDescription material;
            material.base_color = colors[index];
            material.metallic = 0.0F;
            material.roughness = 0.5F;
            material.double_sided = true;
            const auto name = "Materials/Material-" + std::to_string(index);
            auto asset = lux::material::cookImportedMaterial(
                info(20 + index, lux::asset::MaterialAsset::asset_type, name.c_str()), material);
            if (!asset)
            {
                std::fprintf(stderr, "Material cook failed: %s\n", asset.error().message.c_str());
                return 1;
            }
            if (!append(entries, **asset, name))
                return 1;
        }
        std::string error;
        if (!lux::asset::writePakFile(argv[1], std::move(entries), "/Seed", &error))
        {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
