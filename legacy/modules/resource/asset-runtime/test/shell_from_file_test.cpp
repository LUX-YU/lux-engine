//=============================================================================
// shell_from_file_test.cpp
// -----------------------------------------------------------------------------
// Acceptance test for makeShellFromFile (open-world W2b lazy startup registration).
//
// makeShellFromFile reads ONLY a .luxasset file header (no data section) and
// produces an info-only shell for lazily-loadable types, reusing the existing
// version-aware header decode (compat::upgradeAssetInfo for v1). This test
// hand-writes minimal headers — no SerDeser / no data — and asserts:
//   * a current-version (v2) lazy-type header → a typed shell, data()==null
//   * a legacy v1 header → the SAME shell (info upgraded, not byte-garbage)
//   * a non-lazy type (MODEL) → UNSUPPORTED (caller falls back to eager)
//   * a truncated file → ABNORMAL_FILE_SIZE
//   * an unknown version → UNSUPPORTED_VERSION
//   * a missing file → FILE_OPEN_FAIL
//=============================================================================

#include <lux/engine/resource/asset/AssetCodecCatalog.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>        // MeshAsset (the shelled concrete type)

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace lux::asset;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const std::string& desc)
{
    if (cond) { std::cout << "  PASS  " << desc << "\n"; ++g_pass; }
    else      { std::cout << "  FAIL  " << desc << "\n"; ++g_fail; }
}

static void banner(const char* title)
{
    std::cout << "\n" << std::string(60, '=') << "\n"
              << "  " << title << "\n"
              << std::string(60, '=') << "\n";
}

// Write @p bytes verbatim to a temp file and return its path.
static std::filesystem::path writeTemp(const char* name, const void* bytes, std::size_t len)
{
    auto p = std::filesystem::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(len));
    f.close();
    return p;
}

// Build a current-version (v2) AssetFileHeader for @p type with a nil id.
static AssetFileHeader makeV2Header(EAssetType type, std::uint32_t magic)
{
    AssetFileHeader h{};
    h.magic_number = magic;
    h.version      = current_asset_version;
    h.info_offset  = sizeof(AssetFileHeader);
    h.info_size    = sizeof(AssetInfo);
    h.data_offset  = h.info_offset + h.info_size;
    h.data_size    = 0;
    h.info.type    = type;          // makeAssetShell dispatches on info.type
    h.info.date    = 123456789ull;
    return h;
}

static void test_v2_mesh_shell()
{
    banner("v2 MESH header -> MeshAsset shell, no data");
    const auto h = makeV2Header(EAssetType::MESH,
                                asset_magic_number_of<EAssetType::MESH>::value);
    const auto path = writeTemp("lux_shell_v2_mesh.luxasset", &h, sizeof(h));

    auto r = makeShellFromFile(*runtimeAssetCodecCatalog(), path);
    check(r.has_value(), "makeShellFromFile succeeded");
    if (r.has_value())
    {
        check(r.value() != nullptr, "shell pointer non-null");
        check(r.value()->info()->type == EAssetType::MESH, "shell type == MESH");
        check(!r.value()->hasData(), "shell carries NO data (data-less)");
        check(r.value()->as<MeshAsset>() != nullptr, "shell is a MeshAsset");
        check(r.value()->info()->date == 123456789ull, "AssetInfo carried through");
    }
    std::filesystem::remove(path);
}

static void test_v1_upgrade_shell()
{
    banner("legacy v1 MESH header -> upgraded MeshAsset shell (no corruption)");
    compat::AssetFileHeaderV1 v1{};
    v1.magic_number = asset_magic_number_of<EAssetType::MESH>::value;
    v1.version      = asset_version_v1;
    v1.info_offset  = sizeof(compat::AssetFileHeaderV1);
    v1.info_size    = sizeof(compat::AssetInfoV1);
    v1.data_offset  = v1.info_offset + v1.info_size;
    v1.data_size    = 0;
    v1.info.type    = EAssetType::MESH;
    v1.info.date    = 42ull;
    const auto path = writeTemp("lux_shell_v1_mesh.luxasset", &v1, sizeof(v1));

    auto r = makeShellFromFile(*runtimeAssetCodecCatalog(), path);
    check(r.has_value(), "v1 makeShellFromFile succeeded");
    if (r.has_value())
    {
        check(r.value()->info()->type == EAssetType::MESH, "upgraded type == MESH");
        check(r.value()->info()->date == 42ull, "upgraded date preserved (real upgrade, not garbage)");
        check(!r.value()->hasData(), "v1 shell carries NO data");
    }
    std::filesystem::remove(path);
}

static void test_nonlazy_unsupported()
{
    banner("v2 MODEL header -> UNSUPPORTED (caller falls back to eager)");
    const auto h = makeV2Header(EAssetType::MODEL,
                                asset_magic_number_of<EAssetType::MODEL>::value);
    const auto path = writeTemp("lux_shell_v2_model.luxasset", &h, sizeof(h));

    auto r = makeShellFromFile(*runtimeAssetCodecCatalog(), path);
    check(!r.has_value(), "MODEL returns an error (no shell)");
    if (!r.has_value())
        check(r.error() == EAssetError::UNSUPPORTED, "error == UNSUPPORTED");
    std::filesystem::remove(path);
}

static void test_truncated()
{
    banner("truncated file -> ABNORMAL_FILE_SIZE");
    const std::uint32_t just_magic = asset_magic_number_of<EAssetType::MESH>::value;
    const auto path = writeTemp("lux_shell_trunc.luxasset", &just_magic, sizeof(just_magic));

    auto r = makeShellFromFile(*runtimeAssetCodecCatalog(), path);
    check(!r.has_value(), "truncated returns an error");
    if (!r.has_value())
        check(r.error() == EAssetError::ABNORMAL_FILE_SIZE, "error == ABNORMAL_FILE_SIZE");
    std::filesystem::remove(path);
}

static void test_bad_version()
{
    banner("unknown version -> UNSUPPORTED_VERSION");
    auto h = makeV2Header(EAssetType::MESH, asset_magic_number_of<EAssetType::MESH>::value);
    h.version = 999999u;   // neither v1 nor current
    const auto path = writeTemp("lux_shell_badver.luxasset", &h, sizeof(h));

    auto r = makeShellFromFile(*runtimeAssetCodecCatalog(), path);
    check(!r.has_value(), "bad version returns an error");
    if (!r.has_value())
        check(r.error() == EAssetError::UNSUPPORTED_VERSION, "error == UNSUPPORTED_VERSION");
    std::filesystem::remove(path);
}

static void test_missing_file()
{
    banner("missing file -> FILE_OPEN_FAIL");
    auto r = makeShellFromFile(
        *runtimeAssetCodecCatalog(),
        std::filesystem::temp_directory_path() / "lux_shell_does_not_exist.luxasset");
    check(!r.has_value(), "missing file returns an error");
    if (!r.has_value())
        check(r.error() == EAssetError::FILE_OPEN_FAIL, "error == FILE_OPEN_FAIL");
}

int main()
{
    test_v2_mesh_shell();
    test_v1_upgrade_shell();
    test_nonlazy_unsupported();
    test_truncated();
    test_bad_version();
    test_missing_file();

    std::cout << "\nshell_from_file_test: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
