#include <cassert>
#include <consumer/Domain.hpp>
#include <consumer/Gui.hpp>
#include <cstdio>
#include <fstream>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/editor/scene/FieldEdit.hpp>
#include <lux/engine/simulation/ecs/ComponentDecode.hpp>

int sceneWorkflow(const std::filesystem::path &);

void pakRoundTrip(const std::filesystem::path &root)
{
    using namespace lux::asset;
    std::filesystem::create_directories(root);
    const auto id = [](std::string_view value) { return AssetId(*uuids::uuid::from_string(value)); };
    const auto first = id("10000000-0000-0000-0000-000000000001");
    const auto second = id("10000000-0000-0000-0000-000000000002");
    const auto third = id("10000000-0000-0000-0000-000000000003");
    auto bytes = std::make_shared<const std::string>("payload");
    const auto owned = lux::cxx::SharedBytes<>::fromOwner(bytes, std::as_bytes(std::span(*bytes)));
    std::vector<PakWriteEntry> entries{{first, 17, "Shared/Path", {}, owned},
        {second, 17, "Shared/Path", {}, {}, true}, {third, 17, {}, {}, {}, true}};
    std::string error;
    const auto path = root / "tombstones.luxpak";
    assert(writePakFile(path, entries, "/Game", &error));
    std::ifstream file(path, std::ios::binary);
    auto image = std::make_shared<const std::string>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    auto decoded = decodePak(lux::cxx::SharedBytes<>::fromOwner(image, std::as_bytes(std::span(*image))), 3);
    if (!decoded)
    {
        std::printf("disk Pak failure: %s\n", decoded.error().c_str());
    }
    assert(decoded && decoded->entries.size() == 3 && !decoded->entries[0].metadata.tombstone);
    assert(decoded->entries[1].metadata.tombstone && decoded->entries[1].bytes.empty());
    assert(decoded->entries[2].metadata.tombstone && decoded->entries[2].metadata.vpath.empty());
    entries[1].source_bytes = owned;
    assert(!writePakFile(root / "invalid.luxpak", entries, "/Game", &error));
    assert(!std::filesystem::exists(root / "invalid.luxpak"));
    std::puts("PASS installed Pak: file tombstones, shared/empty virtual paths, exact payload rejection");
}

int main(int argc, char **argv)
{
    using namespace lux::simulation::ecs;
    const auto descriptors = consumer::schemas();
    assert(descriptors.size() == 2);
    const auto &schema = descriptors.front();
    assert(schema.editor_visible && schema.decode_emplace && schema.capture);
    assert(!descriptors.back().editor_visible && !descriptors.back().capture);
    const auto binding = consumer::binding();
    assert(binding.type == schema.cpp_type && binding.draw);

    Registry registry;
    const auto entity = registry.create();
    registry.emplace<consumer::Component>(entity);
    WorldEntityMap identities;
    auto capture = schema.capture(registry, entity, {});
    assert(capture);
    auto encoded = capture->encode(identities, 1024 * 1024);
    assert(encoded);
    registry.get<consumer::Component>(entity).sequence.front().name = "edited after capture";
    const auto restored = registry.create();
    auto decoded = schema.decode_emplace(registry, identities, restored, 1, *encoded);
    assert(decoded);
    const auto &value = registry.get<consumer::Component>(restored);
    assert(value.sequence.front().name == "Unicode 中文");
    assert(value.flags.size() == 2 && value.flags[0] && !value.flags[1]);
    assert(value.map.at("key").size() == 2 && value.lookup.at(3) == "value");
    assert(lux::editor::scene::FieldValue<consumer::Component>::valid(value));
    assert(lux::editor::scene::FieldValue<consumer::Component>::equal(value, consumer::Component{}));

    std::puts("PASS installed component: separate domain/GUI DLLs, typed generated binding, nested codec and immutable "
              "capture");
    assert(argc == 2);
    const auto run = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::u8path(argv[1]) / run;
    pakRoundTrip(root / "pak");
    return sceneWorkflow(root / "scene");
}
