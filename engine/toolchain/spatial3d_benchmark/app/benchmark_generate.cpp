// Spatial3D EntityScene benchmark composition root.
#include <lux/engine/authoring/project/ProjectManifest.hpp>
#include <lux/engine/authoring/world/WorldSourceCodec.hpp>
#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/engine/core/serialization/Archive.hpp>
#include <lux/engine/core/serialization/NameTable.hpp>
#include <lux/engine/ecs/serialization/TaggedPropertyArchive.hpp>
#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Skeleton.hpp>
#include <lux/engine/description/Texture.hpp>
#include <lux/engine/resource/asset/AssetHeaderProbe.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/content/BuiltinAssetIds.hpp>
#include <lux/engine/resource/asset/material/MaterialAsset.hpp>
#include <lux/engine/scene/SceneAsset.hpp>
#include <lux/engine/ecs/scene_format/EntitySection.hpp>
#include <lux/engine/resource/asset/material/MaterialSerDeser.hpp>
#include <lux/engine/resource/asset/mesh/MeshSerDeser.hpp>
#include <lux/engine/resource/asset/animation/SkeletonSerDeser.hpp>
#include <lux/engine/resource/asset/texture/TextureSerDeser.hpp>
#include <lux/engine/toolchain/asset/cook/PakCook.hpp>
#include <lux/engine/toolchain/spatial3d_scene/Spatial3DEntitySceneAdapter.hpp>

#include <uuid.h>

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    struct Scale final
    {
        std::string_view name;
        std::uint32_t terrain_edge;
        std::uint32_t instances_per_cell;
        std::uint32_t actors_per_cell;
    };

    struct Options final
    {
        fs::path output;
        fs::path engine_content;
        fs::path cc0_content;
        Scale scale{"smoke", 4u, 256u, 4u};
        std::uint64_t seed{0x4c5558574f524c44ull};
        std::uint32_t cc0_instance_percent{40u};
        std::string platform{"windows-x64"};
        bool authoring_only{false};
    };

    struct PageKey final
    {
        std::int64_t x{0};
        std::int64_t z{0};

        friend auto operator<=>(const PageKey&, const PageKey&) = default;
    };

    [[nodiscard]] std::unique_ptr<lux::asset::AssetInfo> makeAssetInfo(
        const uuids::uuid& id,
        lux::asset::EAssetType type,
        std::string_view display_name)
    {
        auto result = std::make_unique<lux::asset::AssetInfo>();
        result->id = id;
        result->type = type;
        result->date = 0u;
        const auto count = std::min(
            display_name.size(), sizeof(result->display_name) - 1u);
        std::copy_n(
            display_name.data(), count, result->display_name);
        result->display_name[count] = '\0';
        return result;
    }

    [[nodiscard]] std::optional<std::string_view> valueAfter(
        std::string_view argument,
        std::string_view prefix) noexcept
    {
        return argument.starts_with(prefix)
            ? std::optional{argument.substr(prefix.size())}
            : std::nullopt;
    }

    [[nodiscard]] bool parseUnsigned(
        std::string_view text,
        std::uint64_t& value) noexcept
    {
        if (text.empty())
            return false;
        std::uint64_t parsed = 0u;
        for (const char c : text)
        {
            if (c < '0' || c > '9')
                return false;
            const auto digit = static_cast<std::uint64_t>(c - '0');
            if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) /
                    10u)
            {
                return false;
            }
            parsed = parsed * 10u + digit;
        }
        value = parsed;
        return true;
    }

    [[nodiscard]] std::optional<Scale> scaleNamed(std::string_view name)
    {
        if (name == "smoke")
            return Scale{name, 4u, 256u, 4u};
        if (name == "1x")
            return Scale{name, 10u, 100u, 10u};
        if (name == "10x")
            return Scale{name, 32u, 98u, 10u};
        if (name == "100x")
            return Scale{name, 100u, 100u, 1u};
        return std::nullopt;
    }

    void usage()
    {
        std::puts(
            "usage: lux_world_benchmark_generate --output=<project> [options]\n"
            "  --scale=smoke|1x|10x|100x  deterministic content scale\n"
            "  --seed=N                     recipe seed\n"
            "  --platform=<variant>         Cook variant (default windows-x64)\n"
            "  --engine-content=<dir>       canonical built-in material directory\n"
            "  --cc0-content=<dir>          canonical CC0 benchmark asset directory\n"
            "  --cc0-instance-percent=N     diagnostic CC0 placement share (default 40)\n"
            "  --authoring-only             do not Cook/publish the Pak\n");
    }

    [[nodiscard]] std::optional<Options> parse(int argc, char** argv)
    {
        Options options;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument{argv[index]};
            if (argument == "--help" || argument == "-h")
            {
                usage();
                return std::nullopt;
            }
            if (argument == "--authoring-only")
            {
                options.authoring_only = true;
                continue;
            }
            if (const auto value = valueAfter(argument, "--output="))
                options.output = fs::path{*value};
            else if (const auto value = valueAfter(argument, "--scale="))
            {
                const auto scale = scaleNamed(*value);
                if (!scale)
                    return std::nullopt;
                options.scale = *scale;
            }
            else if (const auto value = valueAfter(argument, "--seed="))
            {
                if (!parseUnsigned(*value, options.seed))
                    return std::nullopt;
            }
            else if (const auto value = valueAfter(argument, "--platform="))
                options.platform = *value;
            else if (const auto value = valueAfter(argument, "--engine-content="))
                options.engine_content = fs::path{*value};
            else if (const auto value = valueAfter(argument, "--cc0-content="))
                options.cc0_content = fs::path{*value};
            else if (const auto value = valueAfter(
                         argument, "--cc0-instance-percent="))
            {
                std::uint64_t parsed = 0u;
                if (!parseUnsigned(*value, parsed) || parsed > 100u)
                    return std::nullopt;
                options.cc0_instance_percent =
                    static_cast<std::uint32_t>(parsed);
            }
            else
                return std::nullopt;
        }
        if (options.output.empty() || options.platform.empty())
            return std::nullopt;
        if (options.engine_content.empty())
        {
            std::error_code error;
            const auto executable = fs::absolute(argv[0], error);
            if (!error)
            {
                options.engine_content =
                    executable.parent_path().parent_path() /
                    "engine_content";
            }
        }
        if (options.cc0_content.empty())
            options.cc0_content = options.output / "Content" / "CC0";
        return options;
    }

    struct BenchmarkAssetImage final
    {
        uuids::uuid id;
        std::uint32_t magic_number{0u};
        std::string virtual_path;
        std::vector<std::byte> image;
    };

    struct CanonicalAssetSpec final
    {
        std::string_view relative_path;
        std::string_view sha256;
    };

    struct CanonicalBenchmarkAssets final
    {
        std::vector<BenchmarkAssetImage> images;
        std::vector<std::pair<uuids::uuid, uuids::uuid>> renderables;
    };

    constexpr std::array kCanonicalCc0Assets{
        CanonicalAssetSpec{"Models/fern_02_1k/fern_02_1k.luxmodel", "e1e7e21bfcc19a15ac3fddc899cdb32441653ee6644958e9dec40ad6823827bd"},
        CanonicalAssetSpec{"Models/fern_02_1k/GraphMaterial_0.luxasset", "c6794db83d1b80462351f7fc12c3df1cbb72e159660f1d868f6f1e5aeaf2ad96"},
        CanonicalAssetSpec{"Models/fern_02_1k/GraphMaterial_1.luxasset", "3e4d8a38b5e50e1b643c8d30c1ccc01e2317732e53f4e0cfd001659a27796d43"},
        CanonicalAssetSpec{"Models/fern_02_1k/GraphMaterial_2.luxasset", "75481c8e824a5eb1a473f8809c4ed52785df3715db5576bef1f15e6cb001b521"},
        CanonicalAssetSpec{"Models/fern_02_1k/GraphMaterial_3.luxasset", "00b8830c607c185d4f2d04de10301b4eb7df2d0212e9ff97cbe09a30f6d629f3"},
        CanonicalAssetSpec{"Models/fern_02_1k/Mesh_0.luxasset", "2bd1ede4966df5919e0e20e67e2f4800102d16dbbc7f30efc89b07714b8d2cd0"},
        CanonicalAssetSpec{"Models/fern_02_1k/Mesh_1.luxasset", "88de367a16dc8f0d9a74c0d1100cede5c259f4291900a0ddfe9404486a7308fb"},
        CanonicalAssetSpec{"Models/fern_02_1k/Mesh_2.luxasset", "f3c8d39042132acd5fe67e40167f386daf36dd6c4fc8d7e97e3330b4c5fabf06"},
        CanonicalAssetSpec{"Models/fern_02_1k/Mesh_3.luxasset", "071b56dd0a637fde7044392d16758ac7f6af164959d28d25a0e51bbdb3708e0d"},
        CanonicalAssetSpec{"Models/fern_02_1k/Texture_0.luxasset", "ab59f942e2e6328ae5590f4c84eedf5ab4b95604fc97aa6e9bdffd5c15ae47a2"},
        CanonicalAssetSpec{"Models/fern_02_1k/Texture_1.luxasset", "341d735df13229e27c70a6e1e31335518216a553c346735da4232b2a8364e66c"},
        CanonicalAssetSpec{"Models/fern_02_1k/Texture_2.luxasset", "cdca39bbfd565dd72d05a4a26099d2cc8c752e08ed86b89948fd7aa29cb26148"},
        CanonicalAssetSpec{"Models/rock_07_1k/rock_07_1k.luxmodel", "ed822e63db64c82d257c4e91c3fc67f228cbb05254f6ca6306c9fa519dcb51b6"},
        CanonicalAssetSpec{"Models/rock_07_1k/GraphMaterial_0.luxasset", "a0d9849bf983cda421b5e898a35d40fa8b64fa85d3505db7eda2fafaf5da9b9a"},
        CanonicalAssetSpec{"Models/rock_07_1k/Mesh_0.luxasset", "3a3d616ab9feddef3c36f863b0c92dec10c70a76da028c2ca5cd85e9a0211342"},
        CanonicalAssetSpec{"Models/rock_07_1k/Texture_0.luxasset", "860367e531150e82a5367c2466e014cfff7420b9dc642694fcd893fdda54f0cc"},
        CanonicalAssetSpec{"Models/rock_07_1k/Texture_1.luxasset", "5cdaddb9b1aa36fc21820846ca5385803453a937f153e4c7447667d93ae4fdc1"},
        CanonicalAssetSpec{"Models/rock_07_1k/Texture_2.luxasset", "ca76d278273d75337d44bbe7d56f6c3f97d1cf9a23f851533785cdb8d4970dd9"}};

    [[nodiscard]] uuids::uuid named(
        const uuids::uuid& namespace_id,
        std::string_view name)
    {
        return uuids::uuid_name_generator{namespace_id}(name);
    }

    [[nodiscard]] std::uint64_t mix(std::uint64_t value) noexcept
    {
        value += 0x9e3779b97f4a7c15ull;
        value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
        value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
        return value ^ (value >> 31u);
    }

    [[nodiscard]] double unit(std::uint64_t value) noexcept
    {
        return static_cast<double>(mix(value) >> 11u) *
            (1.0 / 9007199254740992.0);
    }

    [[nodiscard]] std::uint32_t benchmarkWaterCell(
        std::uint32_t terrain_edge) noexcept
    {
        return std::min<std::uint32_t>(8u, (terrain_edge - 1u) / 2u);
    }

    [[nodiscard]] float terrainHeight(
        double x,
        double z,
        std::uint32_t terrain_edge) noexcept
    {
        const auto ridge = std::abs(std::sin(x * 0.00063) *
            std::cos(z * 0.00051));
        const auto rolling = std::sin(x * 0.0031 + z * 0.0017) * 34.0 +
            std::cos(z * 0.0023 - x * 0.0011) * 22.0;
        const auto mountain = std::pow(ridge, 3.2) * 540.0;
        const auto water_center =
            (static_cast<double>(benchmarkWaterCell(terrain_edge)) + 0.5) *
            1024.0;
        const auto lake_basin = -70.0 * std::exp(
            -((x - water_center) * (x - water_center) +
                (z - water_center) * (z - water_center)) /
                8000000.0);
        return static_cast<float>(rolling + mountain + lake_basin + 30.0);
    }

    [[nodiscard]] lux::rdesc::Vertex vertex(
        float x,
        float y,
        float z,
        float u,
        float v,
        Eigen::Vector3f normal = {0.0f, 1.0f, 0.0f})
    {
        lux::rdesc::Vertex result{};
        result.position = {x, y, z};
        result.normal = normal;
        result.tangent = {1.0f, 0.0f, 0.0f};
        result.bitangent = {0.0f, 0.0f, 1.0f};
        result.uv = {u, v};
        return result;
    }

    void appendQuad(
        lux::rdesc::Mesh& mesh,
        Eigen::Vector3f a,
        Eigen::Vector3f b,
        Eigen::Vector3f c,
        Eigen::Vector3f d,
        Eigen::Vector3f normal,
        bool double_sided = false)
    {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(vertex(a.x(), a.y(), a.z(), 0.0f, 0.0f, normal));
        mesh.vertices.push_back(vertex(b.x(), b.y(), b.z(), 1.0f, 0.0f, normal));
        mesh.vertices.push_back(vertex(c.x(), c.y(), c.z(), 1.0f, 1.0f, normal));
        mesh.vertices.push_back(vertex(d.x(), d.y(), d.z(), 0.0f, 1.0f, normal));
        mesh.indices.insert(mesh.indices.end(), {
            base, base + 1u, base + 2u, base, base + 2u, base + 3u});
        if (double_sided)
        {
            mesh.indices.insert(mesh.indices.end(), {
                base + 2u, base + 1u, base, base + 3u, base + 2u, base});
        }
    }

    void appendTriangle(
        lux::rdesc::Mesh& mesh,
        Eigen::Vector3f a,
        Eigen::Vector3f b,
        Eigen::Vector3f c,
        Eigen::Vector3f normal)
    {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(vertex(a.x(), a.y(), a.z(), 0.0f, 0.0f, normal));
        mesh.vertices.push_back(vertex(b.x(), b.y(), b.z(), 0.5f, 1.0f, normal));
        mesh.vertices.push_back(vertex(c.x(), c.y(), c.z(), 1.0f, 0.0f, normal));
        mesh.indices.insert(mesh.indices.end(), {
            base, base + 1u, base + 2u});
    }

    void appendBox(
        lux::rdesc::Mesh& mesh,
        Eigen::Vector3f minimum,
        Eigen::Vector3f maximum)
    {
        const auto x0 = minimum.x();
        const auto y0 = minimum.y();
        const auto z0 = minimum.z();
        const auto x1 = maximum.x();
        const auto y1 = maximum.y();
        const auto z1 = maximum.z();
        appendQuad(mesh, {x0,y0,z1}, {x1,y0,z1}, {x1,y1,z1}, {x0,y1,z1}, {0,0,1});
        appendQuad(mesh, {x1,y0,z0}, {x0,y0,z0}, {x0,y1,z0}, {x1,y1,z0}, {0,0,-1});
        appendQuad(mesh, {x1,y0,z1}, {x1,y0,z0}, {x1,y1,z0}, {x1,y1,z1}, {1,0,0});
        appendQuad(mesh, {x0,y0,z0}, {x0,y0,z1}, {x0,y1,z1}, {x0,y1,z0}, {-1,0,0});
        appendQuad(mesh, {x0,y1,z1}, {x1,y1,z1}, {x1,y1,z0}, {x0,y1,z0}, {0,1,0});
        appendQuad(mesh, {x0,y0,z0}, {x1,y0,z0}, {x1,y0,z1}, {x0,y0,z1}, {0,-1,0});
    }

    [[nodiscard]] lux::rdesc::Mesh benchmarkMesh(std::uint32_t variant)
    {
        const auto family_variant = static_cast<float>(variant / 4u);
        const float half = 0.35f + family_variant * 0.11f;
        const float height = 1.2f + family_variant * 0.72f;
        lux::rdesc::Mesh result;
        if (variant == 31u)
        {
            appendBox(result, {-2.0f, -0.03f, -2.0f}, {2.0f, 0.03f, 2.0f});
            result.lods.push_back({result.indices, 0.10f});
            result.lods.push_back({result.indices, 0.30f});
            result.bounds = lux::math::AABB{
                {-2.0f, -0.03f, -2.0f}, {2.0f, 0.03f, 2.0f}};
            return result;
        }
        switch (variant % 4u)
        {
            case 0u: // rock / conifer silhouette
                result.vertices = {
                    vertex(-half, 0.0f, -half, 0.0f, 0.0f),
                    vertex( half, 0.0f, -half, 1.0f, 0.0f),
                    vertex( half, 0.0f,  half, 1.0f, 1.0f),
                    vertex(-half, 0.0f,  half, 0.0f, 1.0f),
                    vertex(0.0f, height, 0.0f, 0.5f, 0.5f)};
                result.indices = {
                    0u, 1u, 4u, 1u, 2u, 4u, 2u, 3u, 4u, 3u, 0u, 4u,
                    0u, 3u, 2u, 0u, 2u, 1u};
                result.lods.push_back({
                    {0u, 1u, 4u, 1u, 2u, 4u, 2u, 3u, 4u, 3u, 0u, 4u},
                    0.12f});
                result.lods.push_back({
                    {0u, 1u, 4u, 1u, 2u, 4u},
                    0.32f});
                break;
            case 1u: // building / wall / road proportions
            {
                const auto width = half * (1.0f + family_variant * 0.18f);
                appendBox(
                    result,
                    {-width, 0.0f, -half},
                    {width, height, half});
                const auto roof_height = height + std::max(0.35f, height * 0.28f);
                const auto slope = Eigen::Vector3f{0.0f, half, height * 0.28f}
                    .normalized();
                appendQuad(
                    result,
                    {-width * 1.08f, height, -half * 1.12f},
                    { width * 1.08f, height, -half * 1.12f},
                    { width * 1.08f, roof_height, 0.0f},
                    {-width * 1.08f, roof_height, 0.0f},
                    {0.0f, slope.y(), -slope.z()});
                appendQuad(
                    result,
                    {-width * 1.08f, roof_height, 0.0f},
                    { width * 1.08f, roof_height, 0.0f},
                    { width * 1.08f, height, half * 1.12f},
                    {-width * 1.08f, height, half * 1.12f},
                    {0.0f, slope.y(), slope.z()});
                appendTriangle(
                    result,
                    {-width, height, -half},
                    {0.0f, roof_height, 0.0f},
                    {width, height, -half},
                    {0.0f, 0.0f, -1.0f});
                appendTriangle(
                    result,
                    {width, height, half},
                    {0.0f, roof_height, 0.0f},
                    {-width, height, half},
                    {0.0f, 0.0f, 1.0f});
                result.lods.push_back({result.indices, 0.10f});
                result.lods.push_back({result.indices, 0.28f});
                break;
            }
            case 2u: // alpha-tested foliage card archetype
                appendQuad(result,
                    {-half, 0.0f, 0.0f}, {half, 0.0f, 0.0f},
                    {half, height, 0.0f}, {-half, height, 0.0f},
                    {0.0f, 0.0f, 1.0f}, true);
                appendQuad(result,
                    {0.0f, 0.0f, -half}, {0.0f, 0.0f, half},
                    {0.0f, height, half}, {0.0f, height, -half},
                    {1.0f, 0.0f, 0.0f}, true);
                result.lods.push_back({result.indices, 0.14f});
                result.lods.push_back({
                    std::vector<std::uint32_t>(
                        result.indices.begin(), result.indices.begin() + 12),
                    0.34f});
                break;
            default: // bridge / prop / tree-like compound silhouette
                appendBox(result,
                    {-half * 0.24f, 0.0f, -half * 0.24f},
                    { half * 0.24f, height, half * 0.24f});
                appendBox(result,
                    {-half * 1.8f, height * 0.58f, -half * 0.55f},
                    { half * 1.8f, height * 0.78f, half * 0.55f});
                result.lods.push_back({result.indices, 0.16f});
                result.lods.push_back({
                    std::vector<std::uint32_t>(
                        result.indices.begin() + 36, result.indices.end()),
                    0.38f});
                break;
        }
        const auto maximum_height = variant % 4u == 1u
            ? height + std::max(0.35f, height * 0.28f)
            : height;
        result.bounds = lux::math::AABB{
            {-half * 1.8f, 0.0f, -half * 1.12f},
            {half * 1.8f, maximum_height, half * 1.12f}};
        return result;
    }

    [[nodiscard]] lux::cxx::expected<
        std::vector<std::byte>, lux::asset::EAssetError>
    benchmarkSkyTexture(const uuids::uuid& id)
    {
        constexpr std::uint32_t width = 512u;
        constexpr std::uint32_t height = 256u;
        std::vector<std::byte> pixels(
            static_cast<std::size_t>(width) * height * 4u);
        const auto toByte = [](float value) noexcept
        {
            return static_cast<std::byte>(static_cast<std::uint8_t>(
                std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f));
        };
        for (std::uint32_t y = 0u; y < height; ++y)
        {
            const float v = static_cast<float>(y) /
                static_cast<float>(height - 1u);
            const float upper = std::clamp((v - 0.5f) * 2.0f, 0.0f, 1.0f);
            const float lower = std::clamp(v * 2.0f, 0.0f, 1.0f);
            std::array<float, 3u> color{};
            if (v >= 0.5f)
            {
                color = {
                    std::lerp(0.72f, 0.16f, upper),
                    std::lerp(0.82f, 0.38f, upper),
                    std::lerp(0.92f, 0.72f, upper)};
            }
            else
            {
                color = {
                    std::lerp(0.34f, 0.72f, lower),
                    std::lerp(0.40f, 0.82f, lower),
                    std::lerp(0.48f, 0.92f, lower)};
            }
            for (std::uint32_t x = 0u; x < width; ++x)
            {
                const float u = static_cast<float>(x) /
                    static_cast<float>(width - 1u);
                const float sun_u = std::min(
                    std::abs(u - 0.64f),
                    1.0f - std::abs(u - 0.64f));
                const float sun = std::exp(-(
                    sun_u * sun_u * 1800.0f +
                    (v - 0.61f) * (v - 0.61f) * 900.0f));
                const auto offset = (static_cast<std::size_t>(y) * width + x) *
                    4u;
                pixels[offset + 0u] = toByte(color[0] + sun * 0.85f);
                pixels[offset + 1u] = toByte(color[1] + sun * 0.62f);
                pixels[offset + 2u] = toByte(color[2] + sun * 0.30f);
                pixels[offset + 3u] = std::byte{0xff};
            }
        }

        lux::rdesc::TextureInfo info{};
        info.width = static_cast<int>(width);
        info.height = static_cast<int>(height);
        info.channel = 4;
        info.pixel_format = lux::rdesc::ETexturePixelFormat::RGBA8_SRGB;
        info.color_space = lux::rdesc::ETextureColorSpace::SRGB;
        info.layers = 1u;
        info.mip_count = 1u;
        info.mip_ranges[0] = {
            0u,
            static_cast<std::uint64_t>(pixels.size()),
            width,
            height};
        auto texture = lux::rdesc::Texture::copyOf(info, pixels);
        if (!texture)
        {
            return lux::cxx::unexpected(
                lux::asset::EAssetError::ASSET_DESERIALIZE_FAIL);
        }
        return lux::asset::TextureSerDeser::encodeData(id, *texture);
    }

    /// Toolchain must not link ECS merely to construct benchmark Authoring
    /// documents.  LXAD component payloads are intentionally self-describing,
    /// so the deterministic generator writes the small set of benchmark
    /// component records through the public tagged-property wire contract.
    /// Runtime still validates every schema name/version before publishing an
    /// entity and decodes these records through the normal ComponentTypeCatalog.
    class TaggedComponentFields final
    {
    public:
        TaggedComponentFields(
            std::vector<std::byte>& payload,
            lux::serialize::NameTable& names) noexcept
            : writer_(payload)
            , names_(names)
        {}

        TaggedComponentFields(const TaggedComponentFields&) = delete;
        TaggedComponentFields& operator=(const TaggedComponentFields&) = delete;

        ~TaggedComponentFields()
        {
            if (!finished_)
                finish();
        }

        void boolean(std::string_view name, bool value)
        {
            const std::uint8_t wire = value ? 1u : 0u;
            field(name, lux::ecs::serialization::EArchiveType::Bool, &wire,
                sizeof(wire));
        }

        void uint32(std::string_view name, std::uint32_t value)
        {
            field(name, lux::ecs::serialization::EArchiveType::UInt32, &value,
                sizeof(value));
        }

        void floating(std::string_view name, float value)
        {
            field(name, lux::ecs::serialization::EArchiveType::Float, &value,
                sizeof(value));
        }

        void vec2(std::string_view name, const std::array<float, 2>& value)
        {
            field(name, lux::ecs::serialization::EArchiveType::Vec2f, value.data(),
                sizeof(value));
        }

        void vec3(std::string_view name, const std::array<float, 3>& value)
        {
            field(name, lux::ecs::serialization::EArchiveType::Vec3f, value.data(),
                sizeof(value));
        }

        void asset(std::string_view name, const uuids::uuid& value)
        {
            const auto index = names_.intern(name);
            writer_.writePod(index);
            writer_.writePod(static_cast<std::uint8_t>(
                lux::ecs::serialization::EArchiveType::Uuid));
            writer_.writePod(std::uint32_t{16u});
            writer_.writeUuid(value);
        }

        void finish()
        {
            if (finished_)
                return;
            writer_.writePod(lux::ecs::serialization::kEndOfObject);
            finished_ = true;
        }

    private:
        void field(
            std::string_view name,
            lux::ecs::serialization::EArchiveType type,
            const void* data,
            std::size_t size)
        {
            const auto index = names_.intern(name);
            writer_.writePod(index);
            writer_.writePod(static_cast<std::uint8_t>(type));
            writer_.writePod(static_cast<std::uint32_t>(size));
            writer_.writeBytes(data, size);
        }

        lux::serialize::ArchiveWriter writer_;
        lux::serialize::NameTable& names_;
        bool finished_{false};
    };

    template <class Configure>
    void addActorComponent(
        lux::authoring::WorldActorDocument& actor,
        lux::serialize::NameTable& names,
        std::string schema,
        Configure&& configure)
    {
        lux::authoring::WorldActorComponentRecord record;
        record.schema_name = std::move(schema);
        record.schema_version = 1u;
        {
            TaggedComponentFields fields{record.tagged_payload, names};
            configure(fields);
            fields.finish();
        }
        actor.components.push_back(std::move(record));
    }

    void finishActorComponents(
        lux::authoring::WorldActorDocument& actor,
        const lux::serialize::NameTable& names)
    {
        actor.name_table.clear();
        lux::serialize::ArchiveWriter writer{actor.name_table};
        names.serialize(writer);
    }

    [[nodiscard]] bool writeText(
        const fs::path& path,
        std::string_view text)
    {
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        if (error)
            return false;
        auto temporary = path;
        temporary += ".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!stream)
                return false;
        }
        fs::remove(path, error);
        error.clear();
        fs::rename(temporary, path, error);
        return !error;
    }

    [[nodiscard]] bool writeBytes(
        const fs::path& path,
        std::span<const std::byte> bytes)
    {
        return writeText(path, std::string_view{
            reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    }

    [[nodiscard]] std::optional<std::vector<std::byte>> readBytes(
        const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            return std::nullopt;
        const auto end = stream.tellg();
        if (end <= 0)
            return std::nullopt;
        std::vector<std::byte> bytes(static_cast<std::size_t>(end));
        stream.seekg(0, std::ios::beg);
        stream.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!stream)
            return std::nullopt;
        return bytes;
    }

    [[nodiscard]] std::optional<CanonicalBenchmarkAssets>
    loadCanonicalBenchmarkAssets(const fs::path& root)
    {
        const auto virtualPath = [](std::string_view relative_path)
        {
            auto path = fs::path{relative_path};
            path.replace_extension();
            return "Benchmark/CC0/" + path.generic_string();
        };
        CanonicalBenchmarkAssets result;
        result.images.reserve(kCanonicalCc0Assets.size());
        for (const auto& spec : kCanonicalCc0Assets)
        {
            const auto path = root / fs::path{spec.relative_path};
            auto image = readBytes(path);
            if (!image)
            {
                std::fprintf(
                    stderr,
                    "missing canonical CC0 benchmark asset: %s\n",
                    path.string().c_str());
                return std::nullopt;
            }
            const auto digest = lux::cxx::algorithm::toHex(
                lux::cxx::algorithm::Sha256::hash(*image));
            if (digest != spec.sha256)
            {
                std::fprintf(
                    stderr,
                    "canonical CC0 digest mismatch: %s\n"
                    "  expected %.*s\n  actual   %s\n",
                    path.string().c_str(),
                    static_cast<int>(spec.sha256.size()),
                    spec.sha256.data(),
                    digest.c_str());
                return std::nullopt;
            }
            const auto header = lux::asset::readAssetHeader(path);
            const auto* codec =
                lux::asset::runtimeAssetCodecCatalog()->findByMagic(
                    header.magic);
            if (codec == nullptr ||
                header.id.is_nil())
            {
                std::fprintf(
                    stderr,
                    "invalid canonical CC0 asset header: %s\n",
                    path.string().c_str());
                return std::nullopt;
            }
            result.images.push_back({
                header.id,
                header.magic,
                virtualPath(spec.relative_path),
                std::move(*image)});
        }

        const auto idFor = [&](std::string_view relative_path)
            -> std::optional<uuids::uuid>
        {
            const auto virtual_path = virtualPath(relative_path);
            const auto found = std::ranges::find_if(
                result.images,
                [&](const BenchmarkAssetImage& asset)
                {
                    return asset.virtual_path == virtual_path;
                });
            return found == result.images.end()
                ? std::nullopt
                : std::optional{found->id};
        };
        constexpr std::array renderables{
            std::pair{"Models/rock_07_1k/Mesh_0.luxasset",
                      "Models/rock_07_1k/GraphMaterial_0.luxasset"},
            std::pair{"Models/fern_02_1k/Mesh_0.luxasset",
                      "Models/fern_02_1k/GraphMaterial_0.luxasset"},
            std::pair{"Models/fern_02_1k/Mesh_1.luxasset",
                      "Models/fern_02_1k/GraphMaterial_1.luxasset"},
            std::pair{"Models/fern_02_1k/Mesh_2.luxasset",
                      "Models/fern_02_1k/GraphMaterial_2.luxasset"},
            std::pair{"Models/fern_02_1k/Mesh_3.luxasset",
                      "Models/fern_02_1k/GraphMaterial_3.luxasset"}};
        result.renderables.reserve(renderables.size());
        for (const auto& [mesh_path, material_path] : renderables)
        {
            const auto mesh = idFor(mesh_path);
            const auto material = idFor(material_path);
            if (!mesh || !material)
                return std::nullopt;
            result.renderables.emplace_back(*mesh, *material);
        }
        return result;
    }

    [[nodiscard]] bool publishPak(
        lux::toolchain::CookedSpatial3DEntitySceneBundle bundle,
        std::span<const std::pair<uuids::uuid, std::vector<std::byte>>>
            source_meshes,
        std::span<const BenchmarkAssetImage> source_assets,
        const fs::path& pak)
    {
        const auto staging_root = pak.parent_path() /
            (pak.filename().string() + ".staging");
        std::vector<lux::toolchain::PakCookFileEntry> entries;
        std::vector<fs::path> staged_files;
        entries.reserve(
            1u + bundle.sections.size() + source_meshes.size() +
            source_assets.size() + bundle.generated_meshes.size());
        staged_files.reserve(entries.capacity());
        const auto add_bytes = [&](
            uuids::uuid id,
            std::uint32_t magic_number,
            std::string vpath,
            std::span<const std::byte> bytes) -> bool
        {
            const auto path = staging_root / "Images" /
                (uuids::to_string(id) + ".image");
            if (!writeBytes(path, bytes))
                return false;
            entries.push_back({id, magic_number, std::move(vpath), path});
            staged_files.push_back(path);
            return true;
        };
        if (!add_bytes(
                bundle.package.id,
                lux::scene::kSceneAssetMagic,
                "Scenes/Benchmark",
                bundle.encoded_package))
            return false;
        for (const auto& section : bundle.sections)
        {
            const auto key = uuids::to_string(section.record.id.value());
            const auto expected_source =
                "/Game/EntitySections/" + key;
            const auto* stored = std::get_if<
                lux::scene::StoredSectionSource>(
                    &section.record.source);
            if (!stored || stored->content_path != expected_source ||
                !add_bytes(
                    section.record.id.value(),
                    lux::ecs::scene_format::kEntitySectionImageMagic,
                    "EntitySections/" + key,
                    section.encoded_image))
            {
                return false;
            }
        }
        for (const auto& [id, image] : source_meshes)
        {
            if (!add_bytes(
                    id,
                    lux::asset::asset_magic_number_of<
                        lux::asset::EAssetType::MESH>::value,
                    "Benchmark/Mesh/" + uuids::to_string(id),
                    image))
            {
                return false;
            }
        }
        for (const auto& mesh : bundle.generated_meshes)
        {
            if (!add_bytes(
                    mesh.id,
                    lux::asset::asset_magic_number_of<
                        lux::asset::EAssetType::MESH>::value,
                    mesh.virtual_path,
                    mesh.encoded_image))
            {
                return false;
            }
        }
        for (const auto& asset : source_assets)
        {
            if (!add_bytes(
                    asset.id,
                    asset.magic_number,
                    asset.virtual_path,
                    asset.image))
            {
                return false;
            }
        }
        const auto result = lux::toolchain::cookFileEntriesToPak(
            std::move(entries), pak, "/Game");
        if (!result)
        {
            std::fprintf(stderr, "Pak publish failed: %s\n",
                result.error().c_str());
            return false;
        }
        for (const auto& path : staged_files)
        {
            std::error_code ignored;
            fs::remove(path, ignored);
        }
        std::error_code ignored;
        fs::remove_all(staging_root, ignored);
        return true;
    }

#include "benchmark_generate.Generate.inl"

} // namespace

int main(int argc, char** argv)
{
    const auto options = parse(argc, argv);
    if (!options)
    {
        if (argc > 1 && (std::string_view{argv[1]} == "--help" ||
                        std::string_view{argv[1]} == "-h"))
        {
            return 0;
        }
        usage();
        return 2;
    }
    return generate(*options);
}
