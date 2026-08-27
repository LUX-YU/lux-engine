#include <lux/cxx/algorithm/sha256.hpp>

#include <lux/engine/resource/asset/storage/AssetProvider.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using namespace lux::asset;

    constexpr std::size_t kEntryCount = 1000u;
    constexpr std::size_t kPakGoldenSize = 163840u;
    constexpr std::string_view kPakGoldenSha256 = "18b617f54954c5ec5548c8f38b460603459c03bdf50ea598815c3791edf5e715";

    struct TempTree final
    {
        fs::path root;

        TempTree()
        {
            const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
            root = fs::temp_directory_path() / ("lux_asset_pak_contract_" + std::to_string(nonce));
            fs::create_directories(root);
        }

        ~TempTree()
        {
            std::error_code ignored;
            fs::remove_all(root, ignored);
        }
    };

    bool check(bool condition, std::string_view message)
    {
        if (condition)
            return true;
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }

    [[nodiscard]] AssetId makeId(std::uint64_t ordinal)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[0] = 0x42u;
        bytes[6] = 0x40u;
        bytes[8] = 0x80u;
        for (std::size_t index = 0u; index < 8u; ++index)
        {
            bytes[15u - index] = static_cast<std::uint8_t>(ordinal >> (index * 8u));
        }
        return AssetId{bytes};
    }

    [[nodiscard]] std::string bulkPath(std::size_t index)
    {
        std::ostringstream stream;
        stream << "Bulk/Asset_" << std::setw(5) << std::setfill('0') << index;
        return stream.str();
    }

    [[nodiscard]] std::vector<std::byte> payload(std::size_t index)
    {
        return {
            std::byte{0x4c},
            std::byte{0x55},
            std::byte{0x58},
            std::byte{static_cast<unsigned char>(index & 0xffu)},
            std::byte{static_cast<unsigned char>((index >> 8u) & 0xffu)},
            std::byte{0x01},
            std::byte{0x02},
            std::byte{0x03},
        };
    }

    [[nodiscard]] std::vector<PakWriteEntry> fixtures()
    {
        std::vector<PakWriteEntry> entries;
        entries.reserve(kEntryCount);
        for (std::size_t index = 0u; index < kEntryCount; ++index)
        {
            const auto bytes = payload(index);
            entries.push_back(PakWriteEntry{
                makeId(index + 1u),
                0x4b4c5542u,
                bulkPath(index),
                {},
                lux::cxx::SharedBytes<>::copyOf(bytes),
            }
            );
        }
        return entries;
    }

    [[nodiscard]] std::vector<std::byte> readAll(const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            return {};
        const auto end = stream.tellg();
        if (end <= 0)
            return {};
        std::vector<std::byte> bytes(static_cast<std::size_t>(end));
        stream.seekg(0, std::ios::beg);
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return stream ? bytes : std::vector<std::byte>{};
    }

    bool writeAll(const fs::path& path, std::span<const std::byte> bytes)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return stream.good();
    }

    bool flipByte(const fs::path& path, std::uint64_t offset)
    {
        std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekg(static_cast<std::streamoff>(offset));
        char value{};
        stream.read(&value, 1);
        if (!stream)
            return false;
        value ^= 0x5a;
        stream.seekp(static_cast<std::streamoff>(offset));
        stream.write(&value, 1);
        return stream.good();
    }

    [[nodiscard]] std::string sha256(std::span<const std::byte> bytes)
    {
        const auto digest = lux::cxx::algorithm::Sha256::hash(bytes);
        std::array<char, lux::cxx::algorithm::Sha256Digest::hex_size> text{};
        digest.formatHex(text);
        return {text.data(), text.size()};
    }

    bool testPublicWireAndProvider()
    {
        TempTree tree;
        const auto pak_a = tree.root / "a.luxpak";
        const auto pak_b = tree.root / "b.luxpak";
        std::string error;
        if (!check(
                writePakFile(pak_a, fixtures(), "/Game", &error),
                "public writer accepts opaque in-memory records") ||
            !check(writePakFile(pak_b, fixtures(), "/Game", &error), "second deterministic write succeeds"))
        {
            return false;
        }

        const auto pristine = readAll(pak_a);
        const auto second = readAll(pak_b);
        bool success = true;
        success &= check(pristine == second, "two writes are byte-identical");
        const auto actual_sha = sha256(pristine);
        if (kPakGoldenSha256.empty())
        {
            std::cout << "PAK_GOLDEN size=" << pristine.size() << " sha256=" << actual_sha << '\n';
            success = false;
        }
        else
        {
            success &= check(
                pristine.size() == kPakGoldenSize && actual_sha == kPakGoldenSha256,
                "LUXPAK v2 length/SHA-256 stays frozen"
            );
        }

        const auto inspection = inspectPak(pak_a);
        success &= check(
            inspection && inspection->mount_hint == "/Game" && inspection->entries.size() == kEntryCount,
            "public inspector validates the complete paged index"
        );
        if (!inspection)
            return false;

        const auto provider_result = PakAssetProvider::loadFromFile(pak_a);
        success &= check(provider_result.has_value(), "provider validates Header and root pages");
        if (!provider_result)
            return false;
        const auto provider = *provider_result;
        success &= check(
            provider->assetCount() == kEntryCount && provider->resolve(bulkPath(0u)) == makeId(1u) &&
                provider->resolve(bulkPath(517u)) == makeId(518u) && provider->resolve(bulkPath(999u)) == makeId(1000u),
            "provider traverses first, middle and last leaves"
        );
        const auto opened = provider->open(makeId(518u));
        success &= check(
            opened && opened->bytes.size() == 8u && inspection->entries[517u].magic_number == 0x4b4c5542u,
            "provider returns opaque bytes and inspection exposes raw magic"
        );

        std::size_t enumerated = 0u;
        provider->enumerate([&](const ProviderEntry& entry) {
            if (!entry.tombstone)
                ++enumerated;
        }
        );
        success &= check(enumerated == kEntryCount, "enumeration walks the full Entry tree");

        std::atomic<bool> concurrent_ok{true};
        std::vector<std::thread> workers;
        for (std::size_t worker = 0u; worker < 8u; ++worker)
        {
            workers.emplace_back([&, worker] {
                for (std::size_t iteration = 0u; iteration < 80u; ++iteration)
                {
                    const auto index = (worker * 113u + iteration * 17u) % kEntryCount;
                    const auto found = provider->resolve(bulkPath(index));
                    if (!found || *found != makeId(index + 1u) || !provider->open(*found))
                    {
                        concurrent_ok.store(false, std::memory_order_relaxed);
                        return;
                    }
                }
            }
            );
        }
        for (auto& worker : workers)
            worker.join();
        success &= check(concurrent_ok.load(std::memory_order_relaxed), "positional concurrent reads are independent");
        const auto stats = provider->stats();
        success &= check(
            stats.index_pages_resident <= 256u && stats.metadata_resident_bytes <= 256u * 4096u &&
                stats.index_page_hits != 0u && stats.index_page_misses != 0u,
            "paged lookup stays within the metadata LRU budget"
        );

        success &= check(writeAll(pak_a, pristine), "restore pristine header");
        success &= check(flipByte(pak_a, 0u), "corrupt header fixture");
        success &= check(!PakAssetProvider::loadFromFile(pak_a) && !inspectPak(pak_a), "corrupt header is rejected");

        success &= check(writeAll(pak_a, pristine), "restore index fixture");
        const auto payload_end = inspection->entries.back().offset + inspection->entries.back().size;
        const auto first_index_page = (payload_end + 4095u) & ~4095ull;
        success &= check(flipByte(pak_a, first_index_page + 100u), "corrupt descendant page fixture");
        const auto lazy_index_corrupt = PakAssetProvider::loadFromFile(pak_a);
        success &= check(
            !inspectPak(pak_a) && lazy_index_corrupt && !(*lazy_index_corrupt)->open(makeId(1u)),
            "corrupt descendant page fails inspection and lazy lookup"
        );

        success &= check(writeAll(pak_a, pristine), "restore payload fixture");
        const auto victim = inspection->entries[500u];
        success &= check(flipByte(pak_a, victim.offset), "corrupt payload fixture");
        const auto payload_corrupt = PakAssetProvider::loadFromFile(pak_a);
        success &= check(
            payload_corrupt && !(*payload_corrupt)->open(victim.id),
            "payload corruption is rejected lazily by SHA-256"
        );

        const auto truncated = std::span<const std::byte>(pristine).first(pristine.size() - 257u);
        success &= check(writeAll(pak_a, truncated), "write truncated fixture");
        success &= check(!PakAssetProvider::loadFromFile(pak_a) && !inspectPak(pak_a), "truncated Pak is rejected");
        return success;
    }

    bool testWriterValidation()
    {
        TempTree tree;
        const auto pak = tree.root / "validation.luxpak";
        const auto bytes_vector = payload(0u);
        const auto bytes = lux::cxx::SharedBytes<>::copyOf(bytes_vector);
        const auto good = PakWriteEntry{makeId(1u), 0x54534554u, "Opaque/Record", {}, bytes};
        std::string error;
        bool success = true;

        success &= check(!writePakFile({}, {good}, "/Game", &error), "empty output path is rejected");
        success &= check(!writePakFile(pak, {}, "/Game", &error), "empty entry list is rejected");
        success &= check(!writePakFile(pak, {good}, "Game", &error), "invalid mount hint is rejected");
        success &= check(
            !writePakFile(pak, {PakWriteEntry{{}, 0x54534554u, "Opaque/Nil", {}, bytes}}, "/Game", &error),
            "nil UUID is rejected"
        );
        success &= check(
            !writePakFile(pak, {PakWriteEntry{makeId(2u), 0u, "Opaque/Magic", {}, bytes}}, "/Game", &error),
            "zero magic is rejected"
        );
        success &= check(
            !writePakFile(pak, {PakWriteEntry{makeId(2u), 1u, "/Bad/Path", {}, bytes}}, "/Game", &error),
            "non-canonical VirtualPath is rejected"
        );
        success &= check(
            !writePakFile(
                pak,
                {
                    good,
                    PakWriteEntry{makeId(1u), 2u, "Opaque/Other", {}, bytes},
                },
                "/Game",
                &error
            ),
            "duplicate UUID is rejected"
        );
        success &= check(
            !writePakFile(
                pak,
                {
                    good,
                    PakWriteEntry{makeId(2u), 2u, "Opaque/Record", {}, bytes},
                },
                "/Game",
                &error
            ),
            "duplicate path is rejected"
        );
        success &= check(
            !writePakFile(
                pak,
                {
                    good,
                    PakWriteEntry{makeId(2u), 2u, "opaque/record", {}, bytes},
                },
                "/Game",
                &error
            ),
            "ASCII case-fold path collision is rejected"
        );
        success &= check(
            !writePakFile(pak, {PakWriteEntry{makeId(2u), 1u, "Opaque/None", {}, {}}}, "/Game", &error),
            "missing source is rejected"
        );

        const auto source_file = tree.root / "source.bin";
        success &= check(writeAll(source_file, bytes_vector), "create file source");
        success &= check(
            !writePakFile(
                pak,
                {PakWriteEntry{
                    makeId(2u),
                    1u,
                    "Opaque/Both",
                    source_file,
                    bytes,
                }},
                "/Game",
                &error
            ),
            "ambiguous file plus memory source is rejected"
        );
        success &= check(
            writePakFile(
                pak,
                {PakWriteEntry{
                    makeId(3u),
                    0x53524346u,
                    "Opaque/File",
                    source_file,
                    {},
                }},
                "/Game",
                &error
            ),
            "file-backed opaque source is supported"
        );
        const auto provider = PakAssetProvider::loadFromFile(pak);
        success &= check(
            provider && (*provider)->open(makeId(3u)),
            "file-backed entry is readable through the public Provider"
        );
        return success;
    }
}

int
main()
{
    const bool success = testPublicWireAndProvider() && testWriterValidation();
    return success ? 0 : 1;
}
