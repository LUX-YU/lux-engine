#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <lux/engine/toolchain/asset/model/ModelCooker.hpp>
using namespace lux::toolchain;
void write(const std::filesystem::path &file, std::string_view text)
{
    std::ofstream out(file, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    assert(out.good());
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    const std::filesystem::path root(argv[1]);
    assert(!std::filesystem::exists(root));
    std::filesystem::create_directories(root);
    write(root / "triangle.obj", "mtllib triangle.mtl\no Triangle\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 "
                                 "1\nusemtl paint\nf 1/1 2/2 3/3\n");
    write(root / "triangle.mtl", "newmtl paint\nKd 1 0.2 0.1\nmap_Kd color.tga\n");
    std::string tga(18, '\0');
    tga[2] = 2;
    tga[12] = 1;
    tga[14] = 1;
    tga[16] = 32;
    tga[17] = 0x28;
    tga.append("\x00\x80\xff\xff", 4);
    write(root / "color.tga", tga);
    ModelSource source{"triangle.obj", {}};
    const std::array<std::string, 1> primary{source.entry};
    auto files = readModelSourceFiles(root, primary);
    assert(files && files->size() == 1);
    source.files = std::move(*files);
    lux::asset::AssetInfo info;
    std::array<std::uint8_t, 16> bytes{};
    bytes.back() = 42;
    info.id = lux::asset::AssetId(bytes);
    std::size_t attempts{};
    ModelCookAttempt result;
    while (++attempts < 10)
    {
        result = cookModel(info, source);
        auto *requested = std::get_if<ModelSourceRequests>(&result);
        if (!requested)
        {
            break;
        }
        for (const auto &path : requested->paths)
        {
            std::printf("dependency: %s\n", path.c_str());
        }
        auto next = readModelSourceFiles(root, requested->paths);
        assert(next);
        for (auto &file : *next)
        {
            source.files.push_back(std::move(file));
        }
    }
    if (const auto *error = std::get_if<ModelCookFailure>(&result))
    {
        std::printf("COOK error=%u %s\n", unsigned(error->code), error->detail.c_str());
    }
    assert(std::holds_alternative<ModelCookProduct>(result));
    const auto &product = std::get<ModelCookProduct>(result);
    assert(product.model && product.meshes.size() == 1 && product.materials.size() >= 1 &&
           product.textures.size() == 1);
    // Prove the CPU entry has no hidden disk fallback with the complete owned source capture.
    std::filesystem::rename(root / "triangle.obj", root / "triangle.obj.offline");
    std::filesystem::rename(root / "triangle.mtl", root / "triangle.mtl.offline");
    std::filesystem::rename(root / "color.tga", root / "color.tga.offline");
    auto offline = cookModel(info, source);
    assert(std::holds_alternative<ModelCookProduct>(offline));
    assert(std::get<ModelCookProduct>(offline).meshes.front()->id() == product.meshes.front()->id());
    assert(std::get<ModelCookProduct>(offline).materials.back()->id() == product.materials.back()->id());
    auto invalid = source;
    invalid.files.push_back(source.files.front());
    assert(std::holds_alternative<ModelCookFailure>(cookModel(info, invalid)));
    const std::array<std::string, 1> escape{"../outside.obj"};
    assert(!readModelSourceFiles(root, escape));
    assert(!readModelSourceFiles(root, std::array<std::string, 1>{"triangle.obj.offline"}, 1));
    const auto missing = readModelSourceFiles(root, std::array<std::string, 1>{"absent.obj"});
    assert(missing && missing->front().state == EModelSourceState::MISSING);
    const auto captured_text = [](std::string text)
    {
        auto owner = std::make_shared<const std::string>(std::move(text));
        return lux::cxx::SharedBytes<>::fromOwner(owner, std::as_bytes(std::span(*owner)));
    };
    const auto vertices = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 2 0 0\nv 3 0 0\nv 2 1 0\n";
    ModelSource pair{"pair.obj",
                     {{"pair.obj", EModelSourceState::PRESENT,
                       captured_text(std::string(vertices) + "o Left\nf 1 2 3\no Right\nf 4 5 6\n")}}};
    auto before_order = cookModel(info, pair);
    assert(std::holds_alternative<ModelCookProduct>(before_order));
    pair.files.front().bytes = captured_text(std::string(vertices) + "o Right\nf 4 5 6\no Left\nf 1 2 3\n");
    auto after_order = cookModel(info, pair);
    assert(std::holds_alternative<ModelCookProduct>(after_order));
    const auto &before_meshes = std::get<ModelCookProduct>(before_order).meshes;
    const auto &after_meshes = std::get<ModelCookProduct>(after_order).meshes;
    assert(before_meshes.size() == 2 && after_meshes.size() == 2);
    for (const auto &mesh : before_meshes)
    {
        auto same = std::ranges::find_if(after_meshes,
                                         [&](const auto &other)
                                         {
                                             return other->info().display_name == mesh->info().display_name;
                                         });
        assert(same != after_meshes.end());
        if ((*same)->id() != mesh->id())
        {
            std::printf("REIMPORT_IDENTITY_MISMATCH mesh=%s before=%s after=%s\n", mesh->info().display_name.data(),
                        uuids::to_string(mesh->id().uuid()).c_str(), uuids::to_string((*same)->id().uuid()).c_str());
            return 32;
        }
    }
    std::puts("named submesh reorder retains the identity of both mesh assets");
    std::printf("PASS model source closure: attempts=%zu files=%zu mesh=%zu material=%zu texture=%zu; offline capture "
                "and exact rejection\n",
                attempts, source.files.size(), product.meshes.size(), product.materials.size(),
                product.textures.size());
}
