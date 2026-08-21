/// Smoke test: round-trip ProjectManifest + Project::newOnDisk / openFromDisk.
///
/// Not gated by the regular test framework — just an opt-in executable
/// (`-DENABLE_EDITOR_TEST=ON`) the developer runs by hand. Prints PASS /
/// FAIL per checkpoint; returns nonzero on any failure.

#include <lux/engine/authoring/project/Project.hpp>
#include <lux/engine/authoring/project/ProjectManifest.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

namespace
{
    int fail_count = 0;

    void expect(bool cond, const char* msg)
    {
        if (cond)
            std::printf("  [PASS] %s\n", msg);
        else
        {
            std::printf("  [FAIL] %s\n", msg);
            ++fail_count;
        }
    }
} // namespace

int main()
{
    using lux::authoring::Project;
    using lux::authoring::ProjectManifest;

    namespace fs = std::filesystem;

    // Use a temp directory so the test is self-contained and re-runnable.
    const auto tmp_root = fs::temp_directory_path() / "lux_test_project";
    std::error_code ec;
    fs::remove_all(tmp_root, ec);

    std::printf("=== Phase B smoke: ProjectManifest round-trip ===\n");

    // ── 1) makeDefault produces a sensible manifest ────────────────
    auto m = ProjectManifest::makeDefault("TestGame");
    expect(m.name == "TestGame",                    "makeDefault.name");
    expect(m.display_name == "TestGame",            "makeDefault.display_name");
    expect(!m.project_guid.is_nil(),                "makeDefault.project_guid != nil");
    expect(!m.engine_version.empty(),               "makeDefault.engine_version filled");
    expect(m.default_world.empty(),                 "makeDefault.default_world initially empty");
    m.extensions.push_back(lux::authoring::ProjectExtensionEntry{
        lux::authoring::ProjectExtensionId{"org.lux.physics2d"},
        "Plugins/physics2d",
        lux::authoring::EProjectExtensionTarget::RUNTIME,
        1u,
        0u});

    // ── 2) save + load round-trip ──────────────────────────────────
    fs::create_directories(tmp_root, ec);
    const auto manifest_path = tmp_root / "TestGame.luxproject";
    {
        auto saved = m.saveToFile(manifest_path);
        expect(saved.has_value(),                   "manifest save");
        expect(fs::exists(manifest_path),           "manifest file written");
    }

    auto loaded = ProjectManifest::loadFromFile(manifest_path);
    expect(loaded.has_value(),                      "manifest load");
    if (loaded)
    {
        expect(loaded->name == m.name,                       "loaded.name == saved.name");
        expect(loaded->display_name == m.display_name,       "loaded.display_name == saved.display_name");
        expect(loaded->project_guid == m.project_guid,       "loaded.project_guid == saved.project_guid");
        expect(loaded->engine_version == m.engine_version,   "loaded.engine_version == saved.engine_version");
        expect(loaded->default_world == m.default_world,     "loaded.default_world == saved.default_world");
        expect(loaded->extensions.size() == 1u &&
                   loaded->extensions.front().id.name() ==
                       "org.lux.physics2d" &&
                   loaded->extensions.front().path.generic_string() ==
                       "Plugins/physics2d",
               "loaded extensions == saved extensions");
    }

    std::printf("=== Phase B smoke: Project::newOnDisk + openFromDisk ===\n");

    // Clean slate.
    fs::remove_all(tmp_root, ec);

    auto created = Project::newOnDisk(tmp_root, "TestGame");
    expect(created.has_value(),                                       "Project::newOnDisk");
    if (created)
    {
        expect(fs::exists(created->manifestPath()),                   "manifest exists at expected path");
        expect(fs::exists(created->contentRoot()),                    "Content/ created");
        expect(fs::exists(created->worldsRoot()),                     "Worlds/ created");
        expect(fs::exists(created->sourceRoot()),                     "Source/ created");
        expect(fs::exists(created->cacheRoot()),                      "Content/.lux created");
        expect(created->manifest().name == "TestGame",                "new project manifest.name");
        expect(created->scanAssetFiles().empty(),                     "Content/ scan empty in new project");
        expect(created->scanWorldFiles().empty(),                     "Worlds/ scan empty in new project");
    }

    // Re-open from disk.
    if (created)
    {
        auto reopened = Project::openFromDisk(created->manifestPath());
        expect(reopened.has_value(),                                   "Project::openFromDisk");
        if (reopened)
        {
            expect(reopened->manifest().project_guid == created->manifest().project_guid,
                                                                       "reopened project_guid matches");
            expect(reopened->root() == created->root(),                "reopened root matches");
        }
    }

    // ── 3) error cases ─────────────────────────────────────────────
    std::printf("=== Phase B smoke: error paths ===\n");
    {
        auto missing = Project::openFromDisk(tmp_root / "does_not_exist.luxproject");
        expect(!missing.has_value(),                                  "open: missing file → error");
    }
    {
        // Creating into an existing populated project should refuse.
        auto re_create = Project::newOnDisk(tmp_root, "TestGame");
        expect(!re_create.has_value(),                                "new: existing project → error");
    }

    // Cleanup.
    fs::remove_all(tmp_root, ec);

    if (fail_count == 0)
    {
        std::printf("\nAll checks passed.\n");
        return 0;
    }
    else
    {
        std::printf("\n%d check(s) FAILED.\n", fail_count);
        return 1;
    }
}
