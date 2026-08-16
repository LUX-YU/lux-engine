//=============================================================================
// Authoring/runtime VFS integration test.
// -----------------------------------------------------------------------------
// VP-P2 coverage (design: .internal/plan/virtual-path-pak-design.md §2/§3/§5):
//   * VirtualPath grammar: acceptance table, rejection table (charset, dot
//     segments, empty segments, relative form, length cap), accessors,
//     byte-sensitive comparison + foldCaseAscii diagnostics helper.
//   * LooseDirProvider: rescan over a real temp content tree (assets written
//     through SkeletonSerDeser::exportAsLuxAsset), resolve/contains/open/
//     enumerate/pathOf, deterministic vpath-collision winner, charset-
//     violating file skipped with diagnostic.
//   * AssetVfs: mount-priority shadowing, equal-priority head-insert (newest
//     wins), unmount restores, tombstone contract (claims the id, fails the
//     open, hidden from resolve/enumerate), absolute-path enumeration.
//   * AssetManager: setVfs + findAssetByPath + ensureAsset round-trip
//     (resolve -> load -> registered, idempotent second call),
//     no-vfs / nil-id error paths.
//
// Note: case-insensitive vpath CLASH detection in LooseDirProvider can't be
// exercised on a case-insensitive host filesystem (Windows refuses to hold
// "Foo" and "foo" side by side) — covered instead via foldCaseAscii unit
// checks here and via the cook-side rejection tests in VP-P3.
//=============================================================================

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/AssetVfs.hpp>
#include <lux/engine/authoring/assets/LooseAssetProvider.hpp>
#include <lux/engine/resource/asset/VirtualPath.hpp>
#include <lux/engine/resource/asset/SkeletonAsset.hpp>
#include <lux/engine/resource/asset/SkeletonSerDeser.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/description/Skeleton.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace lux::asset;
using lux::authoring::LooseAssetProvider;

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

static void test_shared_bytes_ownership()
{
    banner("SharedBytes: immutable owner and subrange lifetime");

    auto storage = std::make_shared<std::vector<std::byte>>(
        std::initializer_list<std::byte>{
            std::byte{0x10},
            std::byte{0x20},
            std::byte{0x30},
            std::byte{0x40}});
    auto shared = lux::cxx::SharedBytes<>::fromOwner(
        std::shared_ptr<const void>{storage, storage->data()},
        std::span<const std::byte>{storage->data(), storage->size()});
    check(!shared.empty(), "fromOwner accepts owned bytes");
    if (shared.empty())
        return;

    const auto expected_middle = shared.data() + 1u;
    auto middle = shared.subspan(1u, 2u);
    check(!middle.empty(), "subspan produces an in-range slice");
    if (middle.empty())
        return;
    check(
        middle.data() == expected_middle,
        "subspan aliases its owner at the requested range start");

    storage.reset();
    shared = lux::cxx::SharedBytes<>{};
    check(
        middle.size() == 2u &&
            middle.view()[0] == std::byte{0x20} &&
            middle.view()[1] == std::byte{0x30},
        "subspan remains valid after every caller reference is released");
    check(
        middle.subspan(3u).empty() &&
            middle.subspan(2u, 0u).empty(),
        "subspan returns an empty value for out-of-range or empty slices");
}

//-----------------------------------------------------------------------------
// VirtualPath grammar
//-----------------------------------------------------------------------------
static void test_virtual_path_grammar()
{
    banner("VirtualPath: acceptance / rejection / accessors");

    // Acceptance.
    {
        auto p = VirtualPath::parse("/Game/Materials/M_Box");
        check(p.has_value(), "parse /Game/Materials/M_Box");
        if (p.has_value())
        {
            check(p.value().root()    == "Game",            "root() == Game");
            check(p.value().relPath() == "Materials/M_Box", "relPath() == Materials/M_Box");
            check(p.value().name()    == "M_Box",           "name() == M_Box");
            check(p.value().str()     == "/Game/Materials/M_Box", "str() canonical");
        }
    }
    check(VirtualPath::parse("/Engine/Meshes/SM_Cube").has_value(),
          "parse /Engine/Meshes/SM_Cube");
    check(VirtualPath::parse("/Game/A/B/C/D/E/Deep_Asset").has_value(),
          "parse deep nesting");
    check(VirtualPath::parse("/Game/M").has_value(),
          "parse minimal /Game/M");

    // Rejection table.
    struct Reject { const char* text; EVirtualPathError want; const char* why; };
    const Reject rejects[] = {
        { "",                        EVirtualPathError::EMPTY,              "empty string" },
        { "Materials/M_Box",         EVirtualPathError::NOT_ABSOLUTE,       "relative form" },
        { "/Game",                   EVirtualPathError::MISSING_ASSET_NAME, "bare root" },
        { "/Game/",                  EVirtualPathError::EMPTY_SEGMENT,      "trailing slash" },
        { "/Game//M_Box",            EVirtualPathError::EMPTY_SEGMENT,      "double slash" },
        { "/Game/Materials/M_Box/",  EVirtualPathError::EMPTY_SEGMENT,      "trailing slash after name" },
        { "/Game/./M_Box",           EVirtualPathError::ILLEGAL_CHARACTER,  "dot segment" },
        { "/Game/../M_Box",          EVirtualPathError::ILLEGAL_CHARACTER,  "dotdot segment" },
        { "/Game/M_Box.luxasset",    EVirtualPathError::ILLEGAL_CHARACTER,  "extension dot" },
        { "/Game/Pkg:Sub",           EVirtualPathError::ILLEGAL_CHARACTER,  "colon (reserved)" },
        { "/Game\\Materials\\M_Box", EVirtualPathError::ILLEGAL_CHARACTER,  "backslashes" },
        { "/Game/M*Box",             EVirtualPathError::ILLEGAL_CHARACTER,  "asterisk" },
        { "/Game/M?Box",             EVirtualPathError::ILLEGAL_CHARACTER,  "question mark" },
        { "/Game/M<Box",             EVirtualPathError::ILLEGAL_CHARACTER,  "angle bracket" },
        { "/Game/M|Box",             EVirtualPathError::ILLEGAL_CHARACTER,  "pipe" },
        { "/Game/M\"Box",            EVirtualPathError::ILLEGAL_CHARACTER,  "quote" },
        { "/Game/M\tBox",            EVirtualPathError::ILLEGAL_CHARACTER,  "control char" },
    };
    for (const auto& r : rejects)
    {
        auto p = VirtualPath::parse(r.text);
        check(!p.has_value() && p.error() == r.want,
              std::string("reject: ") + r.why);
    }

    // Length cap.
    {
        std::string long_path = "/Game/";
        long_path.append(VirtualPath::kMaxLength, 'a');
        auto p = VirtualPath::parse(long_path);
        check(!p.has_value() && p.error() == EVirtualPathError::TOO_LONG,
              "reject: over kMaxLength");
    }

    // UTF-8 passes (bytes >= 0x80 are legal name chars).
    check(VirtualPath::parse("/Game/\xE6\x9D\x90\xE8\xB4\xA8/M_Box").has_value(),
          "accept UTF-8 segment bytes");

    // Relative validator (provider-side form).
    check(!VirtualPath::validateRelative("Materials/M_Box").has_value(),
          "validateRelative accepts canonical relative");
    check(VirtualPath::validateRelative("/Materials/M_Box").has_value(),
          "validateRelative rejects leading slash");
    check(VirtualPath::validateRelative("Materials/M.Box").has_value(),
          "validateRelative rejects dot");

    // Byte-sensitive comparison + folding diagnostics helper.
    {
        auto a = VirtualPath::parse("/Game/M_Box");
        auto b = VirtualPath::parse("/Game/m_box");
        check(a.has_value() && b.has_value() && !(a.value() == b.value()),
              "byte-sensitive: /Game/M_Box != /Game/m_box");
        check(foldCaseAscii("/Game/M_Box") == foldCaseAscii("/Game/m_box"),
              "foldCaseAscii equates the pair (diagnostics only)");
        check(foldCaseAscii("\xE6\x9D\x90") == "\xE6\x9D\x90",
              "foldCaseAscii leaves non-ASCII bytes untouched");
    }
}

//-----------------------------------------------------------------------------
// Temp content tree helpers
//-----------------------------------------------------------------------------

// Write a real, loadable SkeletonAsset with a deterministic id to @p file.
static asset_id_t writeSkeletonAsset(
    const std::shared_ptr<AssetManager>& mgr,
    const std::filesystem::path&         file,
    std::string_view                     seed)
{
    auto data = std::make_unique<lux::rdesc::Skeleton>();
    lux::rdesc::Bone_t bone;
    bone.name           = "root";
    bone.parent_index   = -1;
    bone.bind_local     = Eigen::Affine3f::Identity();
    bone.inv_bind_world = Eigen::Affine3f::Identity();
    data->bones.push_back(std::move(bone));

    auto asset = mgr->createAssetSeeded<SkeletonAsset>(seed, std::move(data));
    const asset_id_t id = asset->id();
    if (!mgr->registerAsset(std::move(asset)))
        return asset_id_t{};

    SkeletonSerDeser ser(mgr);
    if (ser.exportAsLuxAsset(id, file) != EAssetError::SUCCESS)
        return asset_id_t{};
    return id;
}

struct TempTree
{
    std::filesystem::path root;

    explicit TempTree(const char* tag)
    {
        root = std::filesystem::temp_directory_path()
             / (std::string("lux_vfs_test_") + tag);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
    }
    ~TempTree()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

//-----------------------------------------------------------------------------
// LooseDirProvider
//-----------------------------------------------------------------------------
static void test_loose_dir_provider()
{
    banner("LooseDirProvider: scan / resolve / open / collisions");

    TempTree tree("loose");
    auto bake_mgr = std::make_shared<AssetManager>(
        runtimeAssetCodecCatalog());

    std::filesystem::create_directories(tree.root / "Characters");
    const auto id_hero = writeSkeletonAsset(
        bake_mgr, tree.root / "Characters" / "Hero_Skel.luxasset", "vfs|hero");
    const auto id_top = writeSkeletonAsset(
        bake_mgr, tree.root / "Top_Skel.luxasset", "vfs|top");
    check(!id_hero.is_nil() && !id_top.is_nil(), "fixture assets baked");

    // A file whose stem violates the charset -> skipped with diagnostic.
    {
        std::ofstream bad(tree.root / "bad.name.luxasset", std::ios::binary);
        bad << "junk";
    }

    LooseAssetProvider provider(tree.root);
    const std::size_t n = provider.rescan();
    check(n == 2, "rescan indexes exactly the two valid assets");
    check(!provider.diagnostics().empty(),
          "charset-violating file produced a diagnostic");

    // resolve: byte-sensitive, extensionless, subdir-aware.
    check(provider.resolve("Characters/Hero_Skel").value_or(asset_id_t{}) == id_hero,
          "resolve Characters/Hero_Skel");
    check(provider.resolve("Top_Skel").value_or(asset_id_t{}) == id_top,
          "resolve Top_Skel (root level)");
    check(!provider.resolve("characters/hero_skel").has_value(),
          "resolve is byte-case-sensitive");
    check(!provider.resolve("Characters/Hero_Skel.luxasset").has_value(),
          "resolve rejects extensionful form");

    check(provider.contains(id_hero), "contains(id_hero)");
    check(provider.pathOf(id_hero).value_or("") == "Characters/Hero_Skel",
          "pathOf returns mount-relative vpath");
    check(provider.filePathOf(id_hero).has_value(), "filePathOf present");

    // open: bytes identical to the on-disk file.
    {
        auto blob = provider.open(id_hero);
        check(blob.has_value() && !blob->bytes.empty(), "open(id_hero) returns bytes");
        if (blob.has_value())
        {
            std::ifstream f(tree.root / "Characters" / "Hero_Skel.luxasset",
                            std::ios::binary);
            std::vector<char> disk(
                (std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());
            check(disk.size() == blob->bytes.size()
                      && std::memcmp(disk.data(), blob->bytes.data(),
                                     disk.size()) == 0,
                  "open bytes == file bytes");
        }
        check(!provider.open(asset_id_t{}).has_value(), "open(nil) fails");
    }

    // enumerate.
    {
        std::vector<ProviderEntry> seen;
        provider.enumerate([&](const ProviderEntry& e) { seen.push_back(e); });
        check(seen.size() == 2, "enumerate yields 2 entries");
        bool types_ok = true;
        for (const auto& e : seen)
            types_ok = types_ok && e.type == EAssetType::SKELETON && !e.tombstone;
        check(types_ok, "enumerate carries probed types, no tombstones");
    }

    // Same-vpath collision (same stem, both storage extensions):
    // deterministic winner = byte-smaller relative file path.
    {
        std::filesystem::copy_file(
            tree.root / "Top_Skel.luxasset",
            tree.root / "Top_Skel.luxmodel");
        LooseAssetProvider p2(tree.root);
        p2.rescan();
        // "Top_Skel.luxasset" < "Top_Skel.luxmodel" byte-wise.
        check(p2.resolve("Top_Skel").has_value(),
              "collision: vpath still resolves");
        bool noted = false;
        for (const auto& d : p2.diagnostics())
            noted = noted || d.find("vpath collision") != std::string::npos;
        check(noted, "collision: diagnostic recorded");
        check(p2.filePathOf(id_top).value_or("").extension() == ".luxasset",
              "collision: byte-smaller file path wins deterministically");
    }
}

//-----------------------------------------------------------------------------
// AssetVfs mount semantics (fake provider for full contract control)
//-----------------------------------------------------------------------------
namespace
{
    class FakeProvider final : public IAssetProvider
    {
    public:
        struct Item
        {
            asset_id_t  id;
            std::string vpath;     // empty for tombstones
            std::string bytes;     // payload stand-in
            bool        tombstone{ false };
        };
        std::vector<Item> items;

        std::optional<asset_id_t>
        resolve(std::string_view rel) const override
        {
            for (const auto& i : items)
                if (!i.tombstone && i.vpath == rel)
                    return i.id;
            return std::nullopt;
        }

        bool contains(const asset_id_t& id) const override
        {
            for (const auto& i : items)
                if (i.id == id)
                    return true;
            return false;
        }

        lux::cxx::expected<AssetBlob, EAssetError>
        open(const asset_id_t& id) const override
        {
            for (const auto& i : items)
            {
                if (i.id != id)
                    continue;
                if (i.tombstone)
                    return lux::cxx::unexpected(EAssetError::ASSET_NOT_EXIST);
                auto data = std::make_shared<std::byte[]>(i.bytes.size());
                std::memcpy(data.get(), i.bytes.data(), i.bytes.size());
                return AssetBlob::fromSharedArray(
                    std::move(data), i.bytes.size());
            }
            return lux::cxx::unexpected(EAssetError::ASSET_NOT_EXIST);
        }

        void enumerate(
            const std::function<void(const ProviderEntry&)>& fn) const override
        {
            for (const auto& i : items)
                fn(ProviderEntry{ i.id, EAssetType::SKELETON, i.vpath,
                                  i.tombstone });
        }

        std::optional<std::string>
        pathOf(const asset_id_t& id) const override
        {
            for (const auto& i : items)
                if (i.id == id && !i.tombstone)
                    return i.vpath;
            return std::nullopt;
        }
    };

    asset_id_t makeId(unsigned char tag)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes.fill(tag);
        bytes[6] = 0x40 | (bytes[6] & 0x0F); // version nibble, cosmetic
        bytes[8] = 0x80 | (bytes[8] & 0x3F);
        return asset_id_t{ bytes };
    }
}

static void test_vfs_mount_semantics()
{
    banner("AssetVfs: priority shadowing / head-insert / tombstone");

    const auto id_base    = makeId(0x11);
    const auto id_overlay = makeId(0x22);
    const auto id_only    = makeId(0x33);
    const auto id_dead    = makeId(0x44);

    auto base = std::make_shared<FakeProvider>();
    base->items = {
        { id_base, "Materials/M_Box", "BASE",  false },
        { id_only, "Meshes/SM_Only",  "ONLY",  false },
        { id_dead, "Meshes/SM_Dead",  "DEAD",  false },
    };

    auto overlay = std::make_shared<FakeProvider>();
    overlay->items = {
        { id_overlay, "Materials/M_Box", "OVERLAY", false },
        { id_dead,    "",                "",        true  }, // tombstone
    };

    AssetVfs vfs;
    check(vfs.mount({ "Game", nullptr, 0 }) == kInvalidMountId,
          "mount rejects null provider");
    check(vfs.mount({ "Game", base, 0 }) == kInvalidMountId,
          "mount rejects slashless root");
    check(vfs.mount({ "/Gm/ae", base, 0 }) == kInvalidMountId,
          "mount rejects multi-segment root");

    const MountId m_base = vfs.mount({ "/Game", base, 0 });
    check(m_base != kInvalidMountId, "mount base @0");

    // Single mount sanity.
    check(vfs.resolve("/Game/Materials/M_Box") == id_base,
          "resolve hits base before overlay mounts");
    check(vfs.resolve("/Engine/Materials/M_Box").is_nil(),
          "unmounted root resolves nil");
    check(vfs.resolve("Materials/M_Box").is_nil(),
          "relative form resolves nil (parser rejects)");

    // Higher priority shadows.
    const MountId m_overlay = vfs.mount({ "/Game", overlay, 10 });
    check(vfs.resolve("/Game/Materials/M_Box") == id_overlay,
          "higher-priority overlay wins the path");
    check(vfs.resolve("/Game/Meshes/SM_Only") == id_only,
          "non-shadowed path falls through to base");

    // open: id is the identity — both ids stay openable from their mounts.
    {
        auto b = vfs.open(id_base);
        check(b.has_value() && b.value().bytes.size() == 4
                  && std::memcmp(b.value().bytes.data(), "BASE", 4) == 0,
              "open(id_base) still serves base bytes (uuid identity)");
    }

    // Tombstone: claims the id, fails the open, hides from resolve/enumerate.
    {
        auto d = vfs.open(id_dead);
        check(!d.has_value(), "tombstone shadows the base asset's open");
        // The base mount still carries the PATH row, so the path resolves —
        // shadow-delete is an OPEN-level contract (uuid identity), exactly
        // like a patch pak deleting an asset the base pak still indexes.
        check(vfs.resolve("/Game/Meshes/SM_Dead") == id_dead,
              "tombstone: path still resolves via base (delete blocks open)");
        std::vector<std::string> paths;
        vfs.enumerate([&](const ProviderEntry& e) { paths.push_back(e.vpath); });
        bool has_dead = false, has_box_once = false, abs_ok = true;
        int box_count = 0;
        for (const auto& p : paths)
        {
            if (p == "/Game/Meshes/SM_Dead") has_dead = true;
            if (p == "/Game/Materials/M_Box") ++box_count;
            abs_ok = abs_ok && !p.empty() && p.front() == '/';
        }
        has_box_once = box_count == 1;
        check(!has_dead, "enumerate hides the tombstoned asset entirely");
        check(has_box_once, "enumerate emits a shadowed path exactly once");
        check(abs_ok, "enumerate emits absolute vpaths");
        check(paths.size() == 2, "enumerate total = M_Box(overlay) + SM_Only");
    }

    check(vfs.pathOf(id_overlay).value_or("") == "/Game/Materials/M_Box",
          "pathOf returns absolute winning path");

    // Equal priority: newest mount wins (head-insert).
    {
        auto late = std::make_shared<FakeProvider>();
        const auto id_late = makeId(0x55);
        late->items = { { id_late, "Materials/M_Box", "LATE", false } };
        const MountId m_late = vfs.mount({ "/Game", late, 10 });
        check(vfs.resolve("/Game/Materials/M_Box") == id_late,
              "equal priority: newest mount wins");
        vfs.unmount(m_late);
        check(vfs.resolve("/Game/Materials/M_Box") == id_overlay,
              "unmount restores the previous winner");
    }

    // Unmount overlay entirely -> base resolves again, tombstone gone.
    vfs.unmount(m_overlay);
    check(vfs.resolve("/Game/Materials/M_Box") == id_base,
          "unmount overlay: base path visible again");
    check(vfs.open(id_dead).has_value(),
          "unmount overlay: tombstoned asset openable again");
    vfs.unmount(m_base);
    check(vfs.mountCount() == 0, "all mounts removed");
}

//-----------------------------------------------------------------------------
// AssetManager round-trip: findAssetByPath -> ensureAsset
//-----------------------------------------------------------------------------
static void test_manager_roundtrip()
{
    banner("AssetManager: setVfs + findAssetByPath + ensureAsset");

    TempTree tree("mgr");
    std::filesystem::create_directories(tree.root / "Characters");

    asset_id_t id;
    {
        auto bake_mgr = std::make_shared<AssetManager>(
            runtimeAssetCodecCatalog());
        id = writeSkeletonAsset(
            bake_mgr, tree.root / "Characters" / "Hero_Skel.luxasset",
            "vfs|roundtrip");
        check(!id.is_nil(), "fixture baked");
    }

    // Fresh manager that has never seen the asset.
    AssetManager mgr{runtimeAssetCodecCatalog()};
    check(mgr.findAssetByPath("/Game/Characters/Hero_Skel").is_nil(),
          "findAssetByPath nil before setVfs");
    check(!mgr.ensureAsset(id).has_value(), "ensureAsset fails before setVfs");

    auto provider = std::make_shared<LooseAssetProvider>(tree.root);
    provider->rescan();
    auto vfs = std::make_shared<AssetVfs>();
    vfs->mount({ "/Game", provider, 0 });
    mgr.setVfs(vfs);

    const asset_id_t found = mgr.findAssetByPath("/Game/Characters/Hero_Skel");
    check(found == id, "findAssetByPath resolves to the baked id");
    check(mgr.queryInfo(found) == nullptr,
          "findAssetByPath did NOT load (pure resolution)");

    auto ensured = mgr.ensureAsset(found);
    check(ensured.has_value() && ensured.value() != nullptr,
          "ensureAsset loads through the VFS");
    check(mgr.hasAsset(found),
          "ensureAsset registered the asset (批E2:onLoaded 订阅面已退役,"
          "到位与否按 id 重查)");
    if (ensured.has_value())
    {
        check(ensured.value()->id() == id, "loaded asset carries the id");
        check(ensured.value()->type() == EAssetType::SKELETON,
              "loaded asset is a skeleton");
        auto again = mgr.ensureAsset(found);
        check(again.has_value() && again.value() == ensured.value(),
              "second ensureAsset returns the same pointer (no reload)");
    }

    check(!mgr.ensureAsset(asset_id_t{}).has_value(), "ensureAsset(nil) fails");
    const auto missing = mgr.ensureAsset(
        mgr.generateUUID("vfs|never-baked"));
    check(!missing.has_value()
              && missing.error() == EAssetError::ASSET_NOT_EXIST,
          "ensureAsset(unknown id) -> ASSET_NOT_EXIST");
}

//-----------------------------------------------------------------------------
int main()
{
    test_shared_bytes_ownership();
    test_virtual_path_grammar();
    test_loose_dir_provider();
    test_vfs_mount_semantics();
    test_manager_roundtrip();

    std::cout << "\n" << std::string(60, '=') << "\n"
              << "  asset_vfs_test: " << g_pass << " passed, "
              << g_fail << " failed\n"
              << std::string(60, '=') << "\n";
    if (g_fail == 0)
        std::cout << "asset_vfs_test: all checks passed\n";
    return g_fail == 0 ? 0 : 1;
}
