#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/AssetVfs.hpp>
#include <lux/engine/authoring/assets/LooseAssetProvider.hpp>
#include <lux/engine/resource/asset/PakAssetProvider.hpp>
#include <lux/engine/resource/asset/PakCodec.hpp>
#include <lux/engine/resource/asset/AssetHeaderProbe.hpp>
#include <lux/engine/resource/asset/SkeletonAsset.hpp>
#include <lux/engine/resource/asset/SkeletonSerDeser.hpp>
#include <lux/engine/toolchain/asset/cook/PakCook.hpp>
#include <lux/engine/description/Skeleton.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace lux::asset;
using namespace lux::toolchain;
using lux::authoring::LooseAssetProvider;
namespace fs = std::filesystem;

namespace
{
    int g_pass = 0;
    int g_fail = 0;

    void check(bool condition, const std::string& description)
    {
        if (condition)
        {
            std::cout << "  PASS  " << description << "\n";
            ++g_pass;
        }
        else
        {
            std::cout << "  FAIL  " << description << "\n";
            ++g_fail;
        }
    }

    void banner(const char* title)
    {
        std::cout << "\n" << std::string(60, '=') << "\n  "
                  << title << "\n" << std::string(60, '=') << "\n";
    }

    struct TempTree final
    {
        fs::path root;

        explicit TempTree(const char* tag)
        {
            root = fs::temp_directory_path()
                / (std::string{"lux_pak_v2_test_"} + tag);
            fs::remove_all(root);
            fs::create_directories(root);
        }

        ~TempTree()
        {
            std::error_code error;
            fs::remove_all(root, error);
        }
    };

    std::vector<std::byte> readAll(const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        std::vector<char> raw(
            std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{});
        std::vector<std::byte> bytes(raw.size());
        if (!raw.empty())
            std::memcpy(bytes.data(), raw.data(), raw.size());
        return bytes;
    }

    void writeAll(const fs::path& path, std::span<const std::byte> bytes)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }

    void flipByte(const fs::path& path, std::uint64_t offset)
    {
        std::fstream stream(
            path,
            std::ios::binary | std::ios::in | std::ios::out);
        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        char value = 0;
        stream.read(&value, 1);
        value ^= static_cast<char>(0x5au);
        stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
        stream.write(&value, 1);
    }

    asset_id_t makeId(std::uint64_t ordinal)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[0] = 0x42u;
        for (std::size_t i = 0u; i < 8u; ++i)
        {
            bytes[15u - i] = static_cast<std::uint8_t>(
                ordinal >> (i * 8u));
        }
        return asset_id_t{bytes};
    }

    std::string bulkPath(std::size_t index)
    {
        std::ostringstream stream;
        stream << "Bulk/Asset_" << std::setw(5) << std::setfill('0') << index;
        return stream.str();
    }

    asset_id_t writeSkeletonAsset(
        const std::shared_ptr<AssetManager>& manager,
        const fs::path& file,
        std::string_view seed)
    {
        auto data = std::make_unique<lux::rdesc::Skeleton>();
        lux::rdesc::Bone_t bone;
        bone.name = "root";
        bone.parent_index = -1;
        bone.bind_local = Eigen::Affine3f::Identity();
        bone.inv_bind_world = Eigen::Affine3f::Identity();
        data->bones.push_back(std::move(bone));
        auto asset = manager->createAssetSeeded<SkeletonAsset>(
            seed, std::move(data));
        const auto id = asset->id();
        if (!manager->registerAsset(std::move(asset)))
            return {};
        SkeletonSerDeser codec(manager);
        if (codec.exportAsLuxAsset(id, file) != EAssetError::SUCCESS)
            return {};
        return id;
    }

    detail::PakHeader readHeader(const fs::path& path)
    {
        detail::PakHeader header;
        std::ifstream stream(path, std::ios::binary);
        std::string error;
        const auto size = fs::file_size(path);
        static_cast<void>(
            detail::readPakHeader(stream, size, header, &error));
        return header;
    }

    void testPagedIndexAndIntegrity()
    {
        banner("LUXPAK v2 paged index, lazy integrity and concurrency");
        TempTree tree("paged");
        const auto pak = tree.root / "bulk.luxpak";

        constexpr std::size_t kEntryCount = 1000u;
        std::vector<detail::PakWriteEntry> inputs;
        inputs.reserve(kEntryCount);
        for (std::size_t i = 0u; i < kEntryCount; ++i)
        {
            const std::array<std::byte, 8> payload{
                std::byte{0x4c}, std::byte{0x55}, std::byte{0x58},
                std::byte{static_cast<unsigned char>(i & 0xffu)},
                std::byte{static_cast<unsigned char>((i >> 8u) & 0xffu)},
                std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
            inputs.push_back(detail::PakWriteEntry{
                makeId(i + 1u),
                0u,
                bulkPath(i),
                {},
                lux::cxx::SharedBytes<>::copyOf(payload)});
        }
        std::string write_error;
        check(
            detail::writePakFile(pak, inputs, "/Game", &write_error),
            "writes 1000 payloads into immutable paged B+trees");
        if (!fs::exists(pak))
            return;

        const auto pristine = readAll(pak);
        const auto header = readHeader(pak);
        check(
            header.version == 2u && header.entry_count == kEntryCount
                && header.index_page_count > 2u,
            "header records v2 and a multi-page index");
        {
            std::ifstream stream(pak, std::ios::binary);
            detail::PakPage page;
            std::string error;
            check(
                detail::readPakPage(
                    stream,
                    fs::file_size(pak),
                    header.entry_root_offset,
                    page,
                    &error)
                    && detail::pakPageHeader(page).kind
                        == detail::EPakPageKind::ENTRY_INTERNAL,
                "entry root is internal; startup need not read its leaves");
        }

        auto provider_result = PakAssetProvider::loadFromFile(pak);
        check(provider_result.has_value(), "provider validates Header plus two roots");
        if (!provider_result)
            return;
        const auto provider = provider_result.value();
        check(provider->assetCount() == kEntryCount, "asset count comes from Header");
        const auto mounted_stats = provider->stats();
        check(
            mounted_stats.index_pages_resident == 2u
                && mounted_stats.metadata_resident_bytes
                    == 2u * detail::kPakPageSize,
            "mount retains only the two B+tree roots");
        check(
            provider->resolve(bulkPath(0u)) == makeId(1u)
                && provider->resolve(bulkPath(517u)) == makeId(518u)
                && provider->resolve(bulkPath(999u)) == makeId(1000u),
            "path lookup traverses first, middle and last leaves");
        check(
            provider->open(makeId(518u)).has_value(),
            "payload lookup and SHA-256 verification succeeds");

        std::size_t enumerated = 0u;
        provider->enumerate([&](const ProviderEntry& entry)
        {
            if (!entry.tombstone)
                ++enumerated;
        });
        check(
            enumerated == kEntryCount,
            "explicit enumeration walks the complete Entry tree");

        std::atomic<bool> concurrent_ok{true};
        std::vector<std::thread> workers;
        for (std::size_t worker = 0u; worker < 8u; ++worker)
        {
            workers.emplace_back([&, worker]
            {
                for (std::size_t iteration = 0u; iteration < 80u; ++iteration)
                {
                    const auto index = (worker * 113u + iteration * 17u)
                        % kEntryCount;
                    const auto id = provider->resolve(bulkPath(index));
                    if (!id || *id != makeId(index + 1u)
                        || !provider->open(*id))
                    {
                        concurrent_ok.store(false, std::memory_order_relaxed);
                        return;
                    }
                }
            });
        }
        for (auto& worker : workers)
            worker.join();
        check(
            concurrent_ok.load(std::memory_order_relaxed),
            "concurrent lookups use independent positional read cursors");
        const auto lookup_stats = provider->stats();
        check(
            lookup_stats.index_pages_resident <= 256u
                && lookup_stats.metadata_resident_bytes
                    <= 256u * detail::kPakPageSize
                && lookup_stats.index_page_hits != 0u
                && lookup_stats.index_page_misses != 0u,
            "paged lookup stays inside the fixed metadata LRU budget");

        writeAll(pak, pristine);
        flipByte(pak, 0u);
        check(
            !PakAssetProvider::loadFromFile(pak),
            "corrupt Header is rejected at mount");

        writeAll(pak, pristine);
        flipByte(pak, header.entry_root_offset + 100u);
        check(
            !PakAssetProvider::loadFromFile(pak),
            "corrupt Entry root digest is rejected at mount");

        writeAll(pak, pristine);
        detail::PakEntryChild first_child;
        {
            std::ifstream stream(pak, std::ios::binary);
            detail::PakPage root;
            std::string error;
            std::vector<detail::PakEntryChild> children;
            detail::readPakPage(
                stream,
                fs::file_size(pak),
                header.entry_root_offset,
                root,
                &error);
            detail::decodeEntryInternal(root, children, &error);
            first_child = children.front();
        }
        flipByte(pak, first_child.offset + 100u);
        auto lazy_corrupt = PakAssetProvider::loadFromFile(pak);
        check(
            lazy_corrupt.has_value(),
            "corrupt descendant is not scanned during O(1) mount");
        if (lazy_corrupt)
        {
            const auto opened = lazy_corrupt.value()->open(first_child.maximum_key);
            check(
                !opened && opened.error() == EAssetError::READ_FILE_FAIL,
                "corrupt descendant fails when that lookup first reaches it");
        }

        writeAll(pak, pristine);
        auto inspected = inspectPak(pak);
        check(
            inspected && inspected.value().entries.size() == kEntryCount
                && inspected.value().entries.front().content_digest !=
                    lux::cxx::algorithm::Sha256Digest{},
            "inspection explicitly walks pages and exposes SHA-256");
        if (inspected)
        {
            const auto victim = inspected.value().entries[500u];
            flipByte(pak, victim.offset);
            auto payload_corrupt = PakAssetProvider::loadFromFile(pak);
            check(
                payload_corrupt
                    && !payload_corrupt.value()->open(victim.id),
                "payload corruption is rejected by content digest");
        }
    }

    void testCookParityAndDeterminism()
    {
        banner("toolchain cook determinism and loose/Pak parity");
        TempTree tree("cook");
        const auto content = tree.root / "Content";
        fs::create_directories(content / "Characters");
        auto manager = std::make_shared<AssetManager>(runtimeAssetCodecCatalog());
        const auto hero = writeSkeletonAsset(
            manager,
            content / "Characters" / "Hero.luxasset",
            "pak-v2|hero");
        const auto prop = writeSkeletonAsset(
            manager,
            content / "Prop.luxasset",
            "pak-v2|prop");
        check(!hero.is_nil() && !prop.is_nil(), "asset fixtures are valid");

        const auto first = tree.root / "first.luxpak";
        const auto second = tree.root / "second.luxpak";
        const auto first_result = cookDirectoryToPak(content, first, "/Game");
        const auto second_result = cookDirectoryToPak(content, second, "/Game");
        check(
            first_result && second_result
                && first_result.value().asset_count == 2u,
            "toolchain cook produces two entries");
        check(readAll(first) == readAll(second), "clean cooks are byte-identical");

        auto loose = std::make_shared<LooseAssetProvider>(content);
        loose->rescan();
        auto packed_result = PakAssetProvider::loadFromFile(first);
        check(packed_result.has_value(), "cooked Pak mounts");
        if (!packed_result)
            return;
        const auto packed = packed_result.value();
        bool parity = true;
        std::size_t count = 0u;
        loose->enumerate([&](const ProviderEntry& entry)
        {
            ++count;
            const auto resolved = packed->resolve(entry.vpath);
            const auto loose_blob = loose->open(entry.id);
            const auto packed_blob = packed->open(entry.id);
            parity = parity && resolved && *resolved == entry.id
                && loose_blob && packed_blob
                && loose_blob.value().bytes.size()
                    == packed_blob.value().bytes.size()
                && std::memcmp(
                    loose_blob.value().bytes.data(),
                    packed_blob.value().bytes.data(),
                    loose_blob.value().bytes.size()) == 0;
        });
        check(count == 2u && parity, "loose and Pak resolve/open parity holds");

        AssetManager runtime{runtimeAssetCodecCatalog()};
        auto vfs = std::make_shared<AssetVfs>();
        vfs->mount({"/Game", packed, 0});
        runtime.setVfs(vfs);
        const auto loaded = runtime.ensureAsset(hero);
        check(
            loaded && loaded.value()->type() == EAssetType::SKELETON,
            "AssetManager decodes a Pak payload through the normal VFS path");
    }

    void testMixedSourceAndSceneImageCook()
    {
        banner("loose assets and opaque EntityScene images share one cook");
        TempTree tree("mixed");
        const auto content = tree.root / "Content";
        fs::create_directories(content / "Characters");
        const auto asset_path = content / "Characters" / "Hero.luxasset";
        auto manager = std::make_shared<AssetManager>(
            runtimeAssetCodecCatalog());
        const auto hero = writeSkeletonAsset(
            manager,
            asset_path,
            "pak-v2|mixed-hero");
        check(!hero.is_nil(), "mixed-cook loose asset fixture is valid");
        const std::array<std::byte, 5> authoring_tail{
            std::byte{0xa1},
            std::byte{0xa2},
            std::byte{0xa3},
            std::byte{0xa4},
            std::byte{0xa5}};
        {
            std::ofstream stream(
                asset_path,
                std::ios::binary | std::ios::app);
            stream.write(
                reinterpret_cast<const char*>(authoring_tail.data()),
                static_cast<std::streamsize>(authoring_tail.size()));
        }

        const auto scene_id = makeId(0x1801u);
        const auto section_id = makeId(0x1802u);
        const std::array<std::byte, 4> scene_image{
            std::byte{0x4c},
            std::byte{0x58},
            std::byte{0x53},
            std::byte{0x43}};
        const std::array<std::byte, 5> section_image{
            std::byte{0x4c},
            std::byte{0x58},
            std::byte{0x45},
            std::byte{0x53},
            std::byte{0x7f}};
        const auto scene_image_path = tree.root / "main.lxsc.image";
        const auto section_image_path = tree.root / "startup.lxes.image";
        writeAll(scene_image_path, scene_image);
        writeAll(section_image_path, section_image);
        std::vector<PakCookFileEntry> scene_entries;
        scene_entries.push_back({
            scene_id,
            EAssetType::ENTITY_SCENE,
            "Scenes/Main",
            scene_image_path});
        scene_entries.push_back({
            section_id,
            EAssetType::ENTITY_SECTION,
            "EntitySections/Main_Startup",
            section_image_path});

        const auto pak = tree.root / "mixed.luxpak";
        const auto cooked = cookSourcesAndFileEntriesToPak(
            {PakCookSource{content, ""}},
            std::move(scene_entries),
            pak,
            "/Game");
        check(
            cooked && cooked->asset_count == 3u &&
                cooked->authoring_bytes_stripped >= authoring_tail.size(),
            "mixed cook strips loose authoring bytes and publishes LXSC/LXES");

        auto provider = PakAssetProvider::loadFromFile(pak);
        bool readable = false;
        if (provider)
        {
            const auto scene = provider.value()->open(scene_id);
            const auto section = provider.value()->open(section_id);
            const auto asset = provider.value()->open(hero);
            readable = scene && section && asset &&
                scene->bytes.size() == scene_image.size() &&
                section->bytes.size() == section_image.size() &&
                std::memcmp(
                    scene->bytes.data(),
                    scene_image.data(),
                    scene_image.size()) == 0 &&
                std::memcmp(
                    section->bytes.data(),
                    section_image.data(),
                    section_image.size()) == 0;
        }
        check(
            readable,
            "explicit scene images remain opaque while ordinary assets remain readable");
        check(
            fs::exists(scene_image_path) && fs::exists(section_image_path),
            "mixed publication retains ownership of staged scene files");

        const auto inspected = inspectPak(pak);
        check(
            inspected && std::ranges::any_of(
                inspected->entries,
                [&](const auto& entry)
                {
                    return entry.id == scene_id &&
                        entry.type == EAssetType::ENTITY_SCENE;
                }) && std::ranges::any_of(
                inspected->entries,
                [&](const auto& entry)
                {
                    return entry.id == section_id &&
                        entry.type == EAssetType::ENTITY_SECTION;
                }),
            "mixed Pak index preserves both EntityScene entry types");

        const auto guarded_pak = tree.root / "guarded.luxpak";
        const std::array<std::byte, 6> original_destination{
            std::byte{0x53},
            std::byte{0x41},
            std::byte{0x46},
            std::byte{0x45},
            std::byte{0x21},
            std::byte{0x21}};
        writeAll(guarded_pak, original_destination);
        std::vector<PakCookFileEntry> conflicting;
        conflicting.push_back({
            hero,
            EAssetType::ENTITY_SECTION,
            "EntitySections/DuplicateId",
            section_image_path});
        conflicting.push_back({
            makeId(0x1803u),
            EAssetType::ENTITY_SECTION,
            "Characters/Hero",
            section_image_path});
        conflicting.push_back({
            makeId(0x1804u),
            EAssetType::ENTITY_SECTION,
            "characters/hero",
            section_image_path});
        const auto rejected = cookSourcesAndFileEntriesToPak(
            {PakCookSource{content, ""}},
            std::move(conflicting),
            guarded_pak,
            "/Game");
        check(
            !rejected &&
                rejected.error().find("duplicate uuid") != std::string::npos &&
                rejected.error().find("duplicate vpath") != std::string::npos &&
                rejected.error().find("case-insensitive vpath clash") !=
                    std::string::npos,
            "cross-input uuid, vpath and case-fold conflicts reject together");
        check(
            readAll(guarded_pak) ==
                std::vector<std::byte>(
                    original_destination.begin(),
                    original_destination.end()),
            "mixed-input rejection leaves the destination untouched");
    }

    void testMemoryCook()
    {
        banner("Owning in-memory images use the production paged Pak writer");
        TempTree tree("memory");
        const auto pak = tree.root / "memory.luxpak";
        const auto scene_id = makeId(0x2001u);
        const auto mesh_id = makeId(0x2002u);
        const std::array<std::byte, 4> scene_bytes{
            std::byte{0x4c}, std::byte{0x58},
            std::byte{0x53}, std::byte{0x43}};
        const std::array<std::byte, 3> mesh_bytes{
            std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
        std::vector<PakCookMemoryEntry> entries;
        entries.push_back({
            scene_id,
            EAssetType::ENTITY_SCENE,
            "Scenes/Play",
            lux::cxx::SharedBytes<>::copyOf(scene_bytes)});
        entries.push_back({
            mesh_id,
            EAssetType::MESH,
            "GeneratedMeshes/Test",
            lux::cxx::SharedBytes<>::copyOf(mesh_bytes)});
        const auto cooked = cookMemoryEntriesToPak(
            std::move(entries), pak, "/Game");
        check(
            cooked && cooked->asset_count == 2u &&
                cooked->payload_bytes == 7u,
            "memory images publish atomically without a loose staging tree");
        auto provider = PakAssetProvider::loadFromFile(pak);
        check(
            provider && provider.value()->open(scene_id) &&
                provider.value()->open(mesh_id),
            "memory-cooked EntityScene and generated asset are readable");
        const auto inspected = inspectPak(pak);
        check(
            inspected && inspected->entries.size() == 2u &&
                std::ranges::any_of(
                    inspected->entries,
                    [&](const auto& entry)
                    {
                        return entry.id == scene_id &&
                            entry.type == EAssetType::ENTITY_SCENE;
                    }) &&
                std::ranges::any_of(
                    inspected->entries,
                    [&](const auto& entry)
                    {
                        return entry.id == mesh_id &&
                            entry.type == EAssetType::MESH;
                    }),
            "memory entry types survive the paged Pak index");
        bool boot_scene_ok = false;
        if (provider)
        {
            const auto boot = resolveBootScene(*provider.value(), {});
            boot_scene_ok = boot && boot->id == scene_id &&
                boot->vpath == "Scenes/Play";
        }
        check(
            boot_scene_ok,
            "boot discovery selects the explicit ENTITY_SCENE type");
    }

    void testFileBackedCook()
    {
        banner("File-backed immutable images keep Pak payload memory bounded");
        TempTree tree("file-backed");
        const auto pak = tree.root / "file-backed.luxpak";
        const auto scene_id = makeId(0x3001u);
        const auto mesh_id = makeId(0x3002u);
        const auto scene_image = tree.root / "scene.image";
        const auto mesh_image = tree.root / "mesh.image";
        const std::array<std::byte, 4> scene_bytes{
            std::byte{0x4c}, std::byte{0x58},
            std::byte{0x53}, std::byte{0x43}};
        const std::array<std::byte, 3> mesh_bytes{
            std::byte{0x0a}, std::byte{0x0b}, std::byte{0x0c}};
        {
            std::ofstream stream(scene_image, std::ios::binary);
            stream.write(
                reinterpret_cast<const char*>(scene_bytes.data()),
                static_cast<std::streamsize>(scene_bytes.size()));
        }
        {
            std::ofstream stream(mesh_image, std::ios::binary);
            stream.write(
                reinterpret_cast<const char*>(mesh_bytes.data()),
                static_cast<std::streamsize>(mesh_bytes.size()));
        }
        std::vector<PakCookFileEntry> entries;
        entries.push_back({
            scene_id,
            EAssetType::ENTITY_SCENE,
            "Scenes/File",
            scene_image});
        entries.push_back({
            mesh_id, EAssetType::MESH, "GeneratedMeshes/File", mesh_image});
        const auto cooked = cookFileEntriesToPak(
            std::move(entries), pak, "/Game");
        check(
            cooked && cooked->asset_count == 2u &&
                cooked->payload_bytes == 7u,
            "file images publish through the paged atomic Pak writer");
        auto provider = PakAssetProvider::loadFromFile(pak);
        check(
            provider && provider.value()->open(scene_id) &&
                provider.value()->open(mesh_id),
            "file-backed entries remain lazily readable after publish");
        check(
            std::filesystem::exists(scene_image) &&
                std::filesystem::exists(mesh_image),
            "Pak publication does not take ownership of staged input files");
    }
}

int main()
{
    testPagedIndexAndIntegrity();
    testCookParityAndDeterminism();
    testMixedSourceAndSceneImageCook();
    testMemoryCook();
    testFileBackedCook();
    std::cout << "\n" << std::string(60, '=') << "\n  asset_pak_test: "
              << g_pass << " passed, " << g_fail << " failed\n"
              << std::string(60, '=') << "\n";
    return g_fail == 0 ? 0 : 1;
}
