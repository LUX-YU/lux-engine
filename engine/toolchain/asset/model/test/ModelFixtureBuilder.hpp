#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace lux::toolchain::test
{
    template <class Type>
    void append(std::vector<std::byte>& bytes, const Type& value)
    {
        const auto offset = bytes.size();
        bytes.resize(offset + sizeof(Type));
        std::memcpy(bytes.data() + offset, &value, sizeof(Type));
    }

    inline void writeBytes(const std::filesystem::path& path, std::span<const std::byte> bytes)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    inline void writeText(const std::filesystem::path& path, std::string_view text)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    inline std::filesystem::path writeStaticPbrFixture(const std::filesystem::path& root)
    {
        std::filesystem::create_directories(root);
        std::vector<std::byte> binary;
        const std::array positions{
            -0.5F, -0.5F, 0.0F,
             0.5F, -0.5F, 0.0F,
             0.0F,  0.5F, 0.0F
        };
        const std::array uvs{0.0F, 0.0F, 1.0F, 0.0F, 0.5F, 1.0F};
        const std::array<std::uint16_t, 3U> indices{0U, 1U, 2U};
        for (const float value : positions) append(binary, value);
        for (const float value : uvs) append(binary, value);
        for (const auto value : indices) append(binary, value);
        writeBytes(root / "static.bin", binary);
        std::vector<std::byte> ppm;
        constexpr std::string_view ppm_header = "P6\n2 2\n255\n";
        ppm.insert(
            ppm.end(),
            reinterpret_cast<const std::byte*>(ppm_header.data()),
            reinterpret_cast<const std::byte*>(ppm_header.data() + ppm_header.size())
        );
        constexpr std::array<std::uint8_t, 12U> ppm_pixels{
            255U, 0U, 0U, 255U, 255U, 255U,
            0U, 255U, 0U, 0U, 0U, 255U
        };
        for (const auto value : ppm_pixels) ppm.push_back(static_cast<std::byte>(value));
        writeBytes(root / "external.ppm", ppm);

        constexpr std::string_view embedded_png =
            "UDYKMSAxCjI1NQr/gAA=";
        const std::string json = std::string{R"({
  "asset":{"version":"2.0"},
  "buffers":[{"uri":"static.bin","byteLength":66}],
  "bufferViews":[
    {"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},
    {"buffer":0,"byteOffset":36,"byteLength":24,"target":34962},
    {"buffer":0,"byteOffset":60,"byteLength":6,"target":34963}
  ],
  "accessors":[
    {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-0.5,-0.5,0],"max":[0.5,0.5,0]},
    {"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},
    {"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}
  ],
  "images":[
    {"uri":"external.ppm"},
    {"uri":"data:image/x-portable-pixmap;base64,)"} + std::string{embedded_png} + R"("}
  ],
  "samplers":[{}],
  "textures":[{"sampler":0,"source":0},{"sampler":0,"source":1}],
  "materials":[
    {
      "name":"shared-mask",
      "pbrMetallicRoughness":{
        "baseColorTexture":{"index":0},
        "metallicRoughnessTexture":{"index":0},
        "metallicFactor":0.4,
        "roughnessFactor":0.6
      },
      "alphaMode":"MASK",
      "alphaCutoff":0.4,
      "doubleSided":true
    },
    {
      "name":"embedded-emissive",
      "pbrMetallicRoughness":{"baseColorFactor":[0.1,0.1,0.1,1]},
      "emissiveTexture":{"index":1},
      "emissiveFactor":[1,0.5,0.25]
    }
  ],
  "meshes":[
    {"name":"shared-mesh","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2,"material":0}]},
    {"name":"second-mesh","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2,"material":0}]},
    {"name":"third-mesh","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2,"material":1}]}
  ],
  "nodes":[
    {"name":"root","children":[1,2,3,4]},
    {"mesh":0,"translation":[1,0,0]},
    {"mesh":0,"translation":[-1,0,0]},
    {"mesh":1,"translation":[0,1,0]},
    {"mesh":2,"translation":[0,-1,0]}
  ],
  "scenes":[{"nodes":[0]}],
  "scene":0
})";
        const auto path = root / "static_pbr.gltf";
        writeText(path, json);
        return path;
    }

    inline std::filesystem::path writeSkinnedFixture(const std::filesystem::path& root)
    {
        std::filesystem::create_directories(root);
        std::vector<std::byte> binary;
        const std::array positions{
            -0.5F, 0.0F, 0.0F,
             0.5F, 0.0F, 0.0F,
             0.0F, 1.0F, 0.0F
        };
        for (const float value : positions) append(binary, value);
        const std::array<std::uint16_t, 12U> joints{
            0U, 0U, 0U, 0U,
            1U, 0U, 0U, 0U,
            0U, 1U, 0U, 0U
        };
        for (const auto value : joints) append(binary, value);
        const std::array weights{
            1.0F, 0.0F, 0.0F, 0.0F,
            1.0F, 0.0F, 0.0F, 0.0F,
            0.5F, 0.5F, 0.0F, 0.0F
        };
        for (const float value : weights) append(binary, value);
        const std::array<std::uint16_t, 3U> indices{0U, 1U, 2U};
        for (const auto value : indices) append(binary, value);
        append(binary, std::uint16_t{});
        for (std::uint32_t matrix = 0U; matrix < 2U; ++matrix)
            for (std::uint32_t element = 0U; element < 16U; ++element)
                append(binary, element % 5U == 0U ? 1.0F : 0.0F);
        append(binary, 0.0F);
        append(binary, 1.0F);
        const std::array translations{0.0F, 0.0F, 0.0F, 0.0F, 0.5F, 0.0F};
        for (const float value : translations) append(binary, value);
        writeBytes(root / "skinned.bin", binary);

        constexpr std::string_view json = R"({
  "asset":{"version":"2.0"},
  "buffers":[{"uri":"skinned.bin","byteLength":276}],
  "bufferViews":[
    {"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},
    {"buffer":0,"byteOffset":36,"byteLength":24,"target":34962},
    {"buffer":0,"byteOffset":60,"byteLength":48,"target":34962},
    {"buffer":0,"byteOffset":108,"byteLength":6,"target":34963},
    {"buffer":0,"byteOffset":116,"byteLength":128},
    {"buffer":0,"byteOffset":244,"byteLength":8},
    {"buffer":0,"byteOffset":252,"byteLength":24}
  ],
  "accessors":[
    {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-0.5,0,0],"max":[0.5,1,0]},
    {"bufferView":1,"componentType":5123,"count":3,"type":"VEC4"},
    {"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},
    {"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"},
    {"bufferView":4,"componentType":5126,"count":2,"type":"MAT4"},
    {"bufferView":5,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},
    {"bufferView":6,"componentType":5126,"count":2,"type":"VEC3"}
  ],
  "materials":[{"name":"skin-material","pbrMetallicRoughness":{"baseColorFactor":[0.8,0.6,0.4,1]}}],
  "meshes":[{
    "name":"skin-mesh",
    "primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2},"indices":3,"material":0}]
  }],
  "skins":[{"inverseBindMatrices":4,"joints":[1,2],"skeleton":1}],
  "nodes":[
    {"name":"root","children":[1,3]},
    {"name":"joint-root","children":[2]},
    {"name":"joint-child","translation":[0,1,0]},
    {"name":"skinned-mesh","mesh":0,"skin":0}
  ],
  "animations":[{
    "name":"joint-move",
    "samplers":[{"input":5,"output":6,"interpolation":"LINEAR"}],
    "channels":[{"sampler":0,"target":{"node":2,"path":"translation"}}]
  }],
  "scenes":[{"nodes":[0]}],
  "scene":0
})";
        const auto path = root / "skinned.gltf";
        writeText(path, json);
        return path;
    }
} // namespace lux::toolchain::test
