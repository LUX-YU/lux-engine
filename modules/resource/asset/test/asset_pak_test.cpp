//=============================================================================
// asset_pak_test.cpp
// -----------------------------------------------------------------------------
// VP-P3 coverage (design: .internal/plan/virtual-path-pak-design.md §4/§6/§7):
//   * cookDirectoryToPak over a real temp content tree; hard-reject table
//     (duplicate vpath via double extension, charset-violating file).
//   * DETERMINISM: cooking the same inputs twice yields byte-identical paks.
//   * THE PARITY TEST (frozen contract §7): the same content mounted loose
//     and as a pak => identical resolve() per path + byte-identical open()
//     per id + identical pathOf.
//   * Full pipeline: AssetManager.ensureAsset through a pak-mounted AssetVfs
//     (pak -> blob -> fromLuxAssetMemory -> registered).
//   * Negative decode table: truncation, footer magic/endian corruption,
//     index-hash mismatch, out-of-bounds entry, unknown-section skip
//     (forward compat), unknown compression = single-entry failure,
//     tombstone honored by the v1 reader.
//
// Index-surgery helpers rebuild footer hashes after patching index bytes —
// they reach the codec internals through the module's pinclude (same pattern
// as material_graph_codec_test).
//=============================================================================

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/AssetVfs.hpp>
#include <lux/engine/asset/LooseDirProvider.hpp>
#include <lux/engine/asset/PakAssetProvider.hpp>
#include <lux/engine/asset/PakCook.hpp>
#include <lux/engine/asset/PakCodec.hpp>
#include <lux/engine/asset/SkeletonAsset.hpp>
#include <lux/engine/asset/SkeletonSerDeser.hpp>
#include <lux/engine/description/Skeleton.hpp>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace lux::asset;
namespace fs = std::filesystem;

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

//-----------------------------------------------------------------------------
// Fixture helpers
//-----------------------------------------------------------------------------
static asset_id_t writeSkeletonAsset(
    const std::shared_ptr<AssetManager>& mgr,
    const fs::path&                      file,
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
    fs::path root;
    explicit TempTree(const char* tag)
    {
        root = fs::temp_directory_path() / (std::string("lux_pak_test_") + tag);
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~TempTree()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

static std::vector<std::byte> readAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::vector<char> raw(
        (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    std::memcpy(out.data(), raw.data(), raw.size());
    return out;
}

static void writeAll(const fs::path& p, const std::vector<std::byte>& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

//-----------------------------------------------------------------------------
// Index surgery: load pak bytes, let @p mutate edit the INDEX region (and/or
// grow it), then rewrite footer offset/size/hash so the file revalidates.
//-----------------------------------------------------------------------------
static void patchIndex(
    const fs::path& pak,
    const std::function<void(std::vector<std::byte>& index)>& mutate)
{
    auto bytes = readAll(pak);
    detail::PakFooter footer{};
    std::memcpy(&footer, bytes.data() + bytes.size() - sizeof(footer),
                sizeof(footer));

    std::vector<std::byte> index(
        bytes.begin() + static_cast<std::ptrdiff_t>(footer.index_offset),
        bytes.begin() + static_cast<std::ptrdiff_t>(footer.index_offset
                                                    + footer.index_size));
    mutate(index);

    std::vector<std::byte> out(
        bytes.begin(),
        bytes.begin() + static_cast<std::ptrdiff_t>(footer.index_offset));
    out.insert(out.end(), index.begin(), index.end());

    footer.index_size = index.size();
    footer.index_hash = detail::fnv1a64(index.data(), index.size());
    const auto* fb = reinterpret_cast<const std::byte*>(&footer);
    out.insert(out.end(), fb, fb + sizeof(footer));
    writeAll(pak, out);
}

// Locate the byte offset of ENTB entry @p i inside the index blob.
static std::size_t entbEntryOffset(const std::vector<std::byte>& index,
                                   std::size_t i)
{
    // u32 magic + u32 endian + u32 version + (u32 len + mount_hint bytes)
    std::uint32_t hint_len = 0;
    std::memcpy(&hint_len, index.data() + 12, 4);
    const std::size_t entb_payload = 12 + 4 + hint_len + 4 + 8; // + tag + size
    return entb_payload + i * detail::kPakEntryBytes;
}

//-----------------------------------------------------------------------------
static void test_cook_and_determinism()
{
    banner("cook: success, rejection table, determinism");

    TempTree tree("cook");
    auto mgr = std::make_shared<AssetManager>();
    fs::create_directories(tree.root / "content" / "Characters");
    const auto id_a = writeSkeletonAsset(
        mgr, tree.root / "content" / "Characters" / "A_Skel.luxasset", "pak|a");
    const auto id_b = writeSkeletonAsset(
        mgr, tree.root / "content" / "B_Skel.luxasset", "pak|b");
    check(!id_a.is_nil() && !id_b.is_nil(), "fixture baked");

    // Happy path.
    const fs::path pak1 = tree.root / "out1.luxpak";
    auto r1 = cookDirectoryToPak(tree.root / "content", pak1, "/Game");
    check(r1.has_value() && r1.value().asset_count == 2, "cook succeeds (2 assets)");

    // Determinism: same inputs, second output byte-identical.
    const fs::path pak2 = tree.root / "out2.luxpak";
    auto r2 = cookDirectoryToPak(tree.root / "content", pak2, "/Game");
    check(r2.has_value(), "second cook succeeds");
    check(readAll(pak1) == readAll(pak2), "DETERMINISM: byte-identical paks");

    // Bad mount hint.
    check(!cookDirectoryToPak(tree.root / "content",
                              tree.root / "x.luxpak", "Game").has_value(),
          "cook rejects slashless mount hint");

    // Rejection: duplicate vpath via double extension.
    fs::copy_file(tree.root / "content" / "B_Skel.luxasset",
                  tree.root / "content" / "B_Skel.luxmodel");
    {
        auto r = cookDirectoryToPak(tree.root / "content",
                                    tree.root / "dup.luxpak", "/Game");
        check(!r.has_value()
                  && r.error().find("duplicate vpath") != std::string::npos,
              "cook hard-rejects duplicate vpath (double extension)");
    }
    fs::remove(tree.root / "content" / "B_Skel.luxmodel");

    // Rejection: charset-violating file (LooseDirProvider only diagnoses;
    // cook must be loud).
    {
        std::ofstream bad(tree.root / "content" / "bad.name.luxasset",
                          std::ios::binary);
        bad << "junk";
    }
    {
        auto r = cookDirectoryToPak(tree.root / "content",
                                    tree.root / "bad.luxpak", "/Game");
        check(!r.has_value()
                  && r.error().find("not addressable") != std::string::npos,
              "cook hard-rejects charset-violating file");
    }
    fs::remove(tree.root / "content" / "bad.name.luxasset");

    // pak-inspect smoke.
    {
        auto info = inspectPak(pak1);
        check(info.has_value() && info.value().entries.size() == 2
                  && info.value().mount_hint == "/Game",
              "inspectPak reads back mount hint + 2 entries");
        bool joined = true;
        for (const auto& e : info.value().entries)
            joined = joined && !e.vpath.empty()
                  && e.type == EAssetType::SKELETON && !e.tombstone;
        check(joined, "inspect entries joined with PATH rows + typed");
    }
}

//-----------------------------------------------------------------------------
static void test_parity_and_pipeline()
{
    banner("PARITY: loose vs pak + ensureAsset through a pak");

    TempTree tree("parity");
    auto mgr = std::make_shared<AssetManager>();
    fs::create_directories(tree.root / "content" / "Characters");
    const auto id_a = writeSkeletonAsset(
        mgr, tree.root / "content" / "Characters" / "Hero_Skel.luxasset",
        "parity|hero");
    const auto id_b = writeSkeletonAsset(
        mgr, tree.root / "content" / "Top_Skel.luxasset", "parity|top");
    check(!id_a.is_nil() && !id_b.is_nil(), "fixture baked");

    const fs::path pak = tree.root / "content.luxpak";
    check(cookDirectoryToPak(tree.root / "content", pak, "/Game").has_value(),
          "cook for parity");

    auto loose = std::make_shared<LooseDirProvider>(tree.root / "content");
    loose->rescan();
    auto pak_loaded = PakAssetProvider::loadFromFile(pak);
    check(pak_loaded.has_value(), "PakAssetProvider loads the cooked pak");
    if (!pak_loaded.has_value())
        return;
    auto pakp = pak_loaded.value();
    check(pakp->mountHint() == "/Game", "mount hint round-trips");
    check(pakp->assetCount() == 2, "assetCount == 2");

    // Per-entry parity: same resolve, same pathOf, byte-identical open.
    std::size_t compared = 0;
    bool resolve_ok = true, open_ok = true, path_ok = true;
    loose->enumerate([&](const ProviderEntry& e)
    {
        ++compared;
        const auto via_pak = pakp->resolve(e.vpath);
        resolve_ok = resolve_ok && via_pak.has_value() && *via_pak == e.id;
        path_ok = path_ok
               && pakp->pathOf(e.id).value_or("!") ==
                      loose->pathOf(e.id).value_or("?");
        auto lb = loose->open(e.id);
        auto pb = pakp->open(e.id);
        open_ok = open_ok && lb.has_value() && pb.has_value()
               && lb.value().size == pb.value().size
               && std::memcmp(lb.value().data.get(), pb.value().data.get(),
                              lb.value().size) == 0;
    });
    check(compared == 2, "parity walked both assets");
    check(resolve_ok, "PARITY: resolve identical per path");
    check(path_ok,    "PARITY: pathOf identical per id");
    check(open_ok,    "PARITY: open bytes identical per id");

    // Whole-VFS parity.
    AssetVfs vfs_loose, vfs_pak;
    vfs_loose.mount({ "/Game", loose, 0 });
    vfs_pak.mount({ "/Game", pakp, 0 });
    check(vfs_loose.resolve("/Game/Characters/Hero_Skel")
              == vfs_pak.resolve("/Game/Characters/Hero_Skel"),
          "PARITY: AssetVfs resolve identical");

    // Full pipeline: ensureAsset through the pak mount.
    AssetManager runtime;
    auto vfs = std::make_shared<AssetVfs>();
    vfs->mount({ "/Game", pakp, 0 });
    runtime.setVfs(vfs);
    const auto found = runtime.findAssetByPath("/Game/Characters/Hero_Skel");
    check(found == id_a, "runtime resolves through the pak");
    auto ensured = runtime.ensureAsset(found);
    check(ensured.has_value() && ensured.value() != nullptr
              && ensured.value()->type() == EAssetType::SKELETON,
          "ensureAsset loads a SKELETON from pak bytes");
}

//-----------------------------------------------------------------------------
static void test_negative_decode()
{
    banner("negative decode table");

    TempTree tree("neg");
    auto mgr = std::make_shared<AssetManager>();
    fs::create_directories(tree.root / "content");
    const auto id_a = writeSkeletonAsset(
        mgr, tree.root / "content" / "A_Skel.luxasset", "neg|a");
    const auto id_b = writeSkeletonAsset(
        mgr, tree.root / "content" / "B_Skel.luxasset", "neg|b");
    const fs::path pak = tree.root / "base.luxpak";
    check(cookDirectoryToPak(tree.root / "content", pak, "/Game").has_value(),
          "fixture pak cooked");
    const auto pristine = readAll(pak);

    auto reloads = [&](const char* what) -> bool
    {
        auto r = PakAssetProvider::loadFromFile(pak);
        const bool ok = r.has_value();
        if (!ok)
            std::cout << "        (" << what << ": " << r.error() << ")\n";
        return ok;
    };

    // Truncated footer.
    {
        auto bytes = pristine;
        bytes.resize(bytes.size() - 10);
        writeAll(pak, bytes);
        check(!reloads("truncated"), "reject: truncated file");
    }
    // Corrupt footer magic.
    {
        auto bytes = pristine;
        bytes[bytes.size() - 64] ^= std::byte{ 0xFF };
        writeAll(pak, bytes);
        check(!reloads("footer magic"), "reject: corrupt footer magic");
    }
    // Endian flip.
    {
        auto bytes = pristine;
        bytes[bytes.size() - 64 + 4] ^= std::byte{ 0xFF };
        writeAll(pak, bytes);
        check(!reloads("endian"), "reject: endian mismatch");
    }
    // Index hash mismatch (flip one index byte WITHOUT fixing the footer).
    {
        auto bytes = pristine;
        detail::PakFooter footer{};
        std::memcpy(&footer, bytes.data() + bytes.size() - 64, 64);
        bytes[static_cast<std::size_t>(footer.index_offset) + 1]
            ^= std::byte{ 0xFF };
        writeAll(pak, bytes);
        check(!reloads("hash"), "reject: index hash mismatch");
    }
    // Out-of-bounds entry offset (patch + rehash so ONLY bounds can fail).
    {
        writeAll(pak, pristine);
        patchIndex(pak, [&](std::vector<std::byte>& index)
        {
            const std::size_t e0 = entbEntryOffset(index, 0);
            const std::uint64_t huge = 1ull << 40;
            std::memcpy(index.data() + e0 + 16, &huge, 8); // offset field
        });
        check(!reloads("bounds"), "reject: entry payload out of bounds");
    }
    // Unknown section is SKIPPED (forward compat): inject one before ENTB.
    {
        writeAll(pak, pristine);
        patchIndex(pak, [&](std::vector<std::byte>& index)
        {
            std::uint32_t hint_len = 0;
            std::memcpy(&hint_len, index.data() + 12, 4);
            const std::size_t insert_at = 12 + 4 + hint_len;
            std::vector<std::byte> section;
            const std::uint32_t tag = detail::pakFourcc('F','U','T','R');
            const std::uint64_t size = 4;
            const std::uint32_t payload = 0xDEADBEEF;
            section.resize(4 + 8 + 4);
            std::memcpy(section.data() + 0,  &tag, 4);
            std::memcpy(section.data() + 4,  &size, 8);
            std::memcpy(section.data() + 12, &payload, 4);
            index.insert(index.begin() + static_cast<std::ptrdiff_t>(insert_at),
                         section.begin(), section.end());
        });
        auto r = PakAssetProvider::loadFromFile(pak);
        check(r.has_value(), "unknown section: pak still loads (skipped)");
        if (r.has_value())
            check(r.value()->open(id_a).has_value(),
                  "unknown section: entries still open");
    }
    // Unknown compression value = SINGLE-entry failure, sibling intact.
    {
        writeAll(pak, pristine);
        // Find which ENTB slot holds id_a (entries are uuid-sorted).
        std::size_t slot_a = 0;
        {
            auto info = inspectPak(pak);
            for (std::size_t i = 0; i < info.value().entries.size(); ++i)
                if (info.value().entries[i].id == id_a)
                    slot_a = i;
        }
        patchIndex(pak, [&](std::vector<std::byte>& index)
        {
            const std::size_t e = entbEntryOffset(index, slot_a);
            index[e + 44] = std::byte{ 0x7F }; // compression byte
        });
        auto r = PakAssetProvider::loadFromFile(pak);
        check(r.has_value(), "unknown compression: pak still loads");
        if (r.has_value())
        {
            auto bad  = r.value()->open(id_a);
            auto good = r.value()->open(id_b);
            check(!bad.has_value()
                      && bad.error() == EAssetError::UNSUPPORTED,
                  "unknown compression: that entry fails UNSUPPORTED");
            check(good.has_value(),
                  "unknown compression: sibling entry still opens");
        }
    }
    // Tombstone honored by the v1 reader (v1 writer never emits one).
    {
        writeAll(pak, pristine);
        std::size_t slot_a = 0;
        {
            auto info = inspectPak(pak);
            for (std::size_t i = 0; i < info.value().entries.size(); ++i)
                if (info.value().entries[i].id == id_a)
                    slot_a = i;
        }
        patchIndex(pak, [&](std::vector<std::byte>& index)
        {
            const std::size_t e = entbEntryOffset(index, slot_a);
            index[e + 45] = std::byte{ 0x02 }; // flags: tombstone
        });
        auto r = PakAssetProvider::loadFromFile(pak);
        check(r.has_value(), "tombstone: pak loads");
        if (r.has_value())
        {
            check(r.value()->contains(id_a),
                  "tombstone: contains() still true (claims the id)");
            check(!r.value()->open(id_a).has_value(),
                  "tombstone: open() fails");
            check(!r.value()->resolve("A_Skel").has_value(),
                  "tombstone: resolve() excluded");
            check(!r.value()->pathOf(id_a).has_value(),
                  "tombstone: pathOf() excluded");
            bool emitted_tomb = false;
            std::size_t visible = 0;
            r.value()->enumerate([&](const ProviderEntry& e)
            {
                if (e.tombstone) emitted_tomb = true;
                else ++visible;
            });
            check(emitted_tomb && visible == 1,
                  "tombstone: enumerate emits the claim, hides the asset");
        }
    }
}

//-----------------------------------------------------------------------------
int main()
{
    test_cook_and_determinism();
    test_parity_and_pipeline();
    test_negative_decode();

    std::cout << "\n" << std::string(60, '=') << "\n"
              << "  asset_pak_test: " << g_pass << " passed, "
              << g_fail << " failed\n"
              << std::string(60, '=') << "\n";
    if (g_fail == 0)
        std::cout << "asset_pak_test: all checks passed\n";
    return g_fail == 0 ? 0 : 1;
}
