#pragma once

#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace fixture
{
    inline void writeText(const std::filesystem::path& file, std::string_view text)
    {
        std::ofstream output(file, std::ios::binary);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        assert(output.good());
    }

    inline std::filesystem::path modelSource(const std::filesystem::path& directory)
    {
        std::filesystem::create_directories(directory);
        // Two named meshes, two named materials and two real external textures. Stable names must
        // survive reimport, while product files change independently of existing VFS snapshots.
        writeText(directory / "model.obj", R"(mtllib model.mtl
v -2 0 0
v -0.2 0 0
v -0.2 2 0
v -2 2 0
v 0.2 0 0
v 2 0 0
v 2 2 0
v 0.2 2 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
vn 0 0 1
o left
usemtl red
f 1/1/1 2/2/1 3/3/1
f 1/1/1 3/3/1 4/4/1
o right
usemtl green
f 5/1/1 6/2/1 7/3/1
f 5/1/1 7/3/1 8/4/1
)");
        writeText(directory / "model.mtl", R"(newmtl red
Kd 1 1 1
Ke 0.5 0.1 0.1
map_Kd red.tga
newmtl green
Kd 1 1 1
Ke 0.1 0.5 0.1
map_Kd green.tga
)");
        for (const bool green : {false, true})
        {
            std::array<unsigned char, 34> bytes{};
            bytes[2] = 2;
            bytes[12] = bytes[14] = 2;
            bytes[16] = 32;
            bytes[17] = 0x28;
            for (std::size_t pixel = 18; pixel < bytes.size(); pixel += 4)
            {
                bytes[pixel + (green ? 1 : 2)] = 255;
                bytes[pixel + 3] = 255;
            }
            std::ofstream output(directory / (green ? "green.tga" : "red.tga"), std::ios::binary);
            output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            assert(output.good());
        }
        return directory / "model.obj";
    }
} // namespace fixture
