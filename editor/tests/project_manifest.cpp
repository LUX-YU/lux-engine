#include <cassert>
#include <fstream>
#include <iostream>
#include <lux/engine/editor/ProjectManifest.hpp>

using namespace lux::editor;

int main(int argc, char** argv)
{
    assert(argc == 2);
    const auto root = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(root);
    const auto file = root / "项目 Manifest.luxproj";
    std::filesystem::remove(file);
    uuids::uuid_name_generator ids{*uuids::uuid::from_string("629c02e8-504e-4ebe-b633-ce8fc2269954")};
    ProjectManifest manifest{1, ids("project"), "Example"};
    auto encoded = encodeProjectManifest(manifest);
    assert(encoded);
    auto decoded = decodeProjectManifest(*encoded);
    assert(decoded && *decoded == manifest);
    assert(writeProjectManifestAtomic(file, manifest, EProjectWrite::CREATE));
    assert(*readProjectManifest(file) == *decoded);
    auto duplicate_file = writeProjectManifestAtomic(file, manifest, EProjectWrite::CREATE);
    assert(!duplicate_file && duplicate_file.error().code == EProjectError::DESTINATION_EXISTS);

    const auto a = lux::asset::AssetId{ids("a")};
    const auto b = lux::asset::AssetId{ids("b")};
    manifest.plugins = {{"lux.builtin.scene_render", 1}};
    manifest.scenes = {{a, "Main", "Beginner/Main.luxscene", "lux.editor.scene.3d"}};
    manifest.startup_scene = a;
    // Numeric segments are part of canonical identifiers, e.g. the actual 3D profile.
    encoded = encodeProjectManifest(manifest);
    assert(encoded);
    decoded = decodeProjectManifest(*encoded);
    assert(decoded && *decoded == manifest);
    manifest.scenes.push_back({b, "Other", "Other.luxscene", "vendor.scene.flat"});
    assert(writeProjectManifestAtomic(file, manifest, EProjectWrite::REPLACE));
    auto read = readProjectManifest(file);
    assert(read && *read == manifest);
    const auto expect = [&](EProjectError code, auto edit)
    {
        auto invalid = manifest;
        edit(invalid);
        auto result = validateProjectManifest(invalid);
        assert(!result && result.error().code == code);
        assert(!encodeProjectManifest(invalid));
    };
    expect(EProjectError::UNSUPPORTED_VERSION, [](auto& m) { m.format_version = 2; });
    expect(EProjectError::INVALID_IDENTITY, [](auto& m) { m.id = {}; });
    expect(EProjectError::INVALID_NAME, [](auto& m) { m.name.clear(); });
    expect(EProjectError::DUPLICATE_IDENTITY, [](auto& m) { m.scenes[1].id = m.scenes[0].id; });
    expect(EProjectError::DUPLICATE_IDENTITY, [](auto& m) { m.plugins.push_back(m.plugins.front()); });
    expect(EProjectError::DUPLICATE_PATH, [](auto& m) { m.scenes[1].path = "beginner/main.luxscene"; });
    expect(EProjectError::INVALID_PROFILE, [](auto& m) { m.scenes[0].profile = "lux..3d"; });
    for (const auto path :
         {"../x", "a/../x", "/x", "C:/x", "a\\x", "a//x", "a/", "a/./b", "a?.scene", "con.scene", "A/LPT9.scene"})
    {
        expect(EProjectError::INVALID_PATH, [&](auto& m) { m.scenes[0].path = path; });
    }
    expect(
        EProjectError::INVALID_STARTUP_SCENE,
        [&](auto& m) { m.startup_scene = lux::asset::AssetId{ids("missing")}; }
    );
    for (const auto text : {"", "{", "[]", "{\"format_version\":-1}", "{\"format_version\":1,\"id\":7}"})
    {
        assert(!decodeProjectManifest(text));
    }
    auto future = decodeProjectManifest("{\"format\":\"lux.editor.project\",\"format_version\":2}");
    assert(!future && future.error().code == EProjectError::UNSUPPORTED_VERSION);
    const auto valid_json = encodeProjectManifest(manifest).value();
    assert(valid_json.find("lux.editor.project") != valid_json.npos);
    const auto invalidJson = [&](std::string text)
    {
        auto result = decodeProjectManifest(text);
        assert(!result && result.error().code == EProjectError::INVALID_FORMAT);
    };
    const auto replaced = [&](std::string_view from, std::string_view to)
    {
        auto text = valid_json;
        auto position = text.find(from);
        assert(position != text.npos);
        text.replace(position, from.size(), to);
        return text;
    };
    invalidJson(replaced("lux.editor.project", "another.format"));
    invalidJson(replaced("\"format\"", "\"unknown_format\""));
    invalidJson("{\"name\":\"Duplicate\"," + valid_json.substr(1));
    invalidJson("{\"na\\u006de\":\"Escaped duplicate\"," + valid_json.substr(1));
    invalidJson("{\"startup_scen\":null," + valid_json.substr(1));
    invalidJson(replaced("\"version\": 1", "\"version\": 1, \"version\": 1"));
    invalidJson(replaced("\"version\": 1", "\"version\": 1, \"extra\": 1"));
    invalidJson(replaced("\"profile\":", "\"path\": \"Duplicate.scene\", \"profile\":"));
    invalidJson(replaced("\"profile\":", "\"extra\": true, \"profile\":"));
    expect(EProjectError::INVALID_PLUGIN, [](auto& m) { m.plugins[0].id = "1.plugin"; });
    auto huge = decodeProjectManifest(std::string(1024 * 1024 + 1, ' '));
    assert(!huge && huge.error().code == EProjectError::LIMIT);
    std::stop_source stopped;
    stopped.request_stop();
    auto cancelled = writeProjectManifestAtomic(
        file,
        ProjectManifest{1, ids("other"), "Other"},
        EProjectWrite::REPLACE,
        stopped.get_token()
    );
    assert(!cancelled && cancelled.error().code == EProjectError::CANCELLED);
    assert(*readProjectManifest(file) == manifest);
    assert(!readProjectManifest(file, stopped.get_token()));
    assert(!readProjectManifest(root / "missing.luxproj"));
    assert(!writeProjectManifestAtomic(root / "missing-parent" / "p.luxproj", manifest, EProjectWrite::CREATE));
    for (const auto& entry : std::filesystem::directory_iterator(root))
    {
        assert(entry.path().filename().string().find(".tmp.") == std::string::npos);
    }
    std::cout << "PS0: real manifest roundtrips, validation, CREATE/REPLACE, IO and cancellation passed\n";
}
