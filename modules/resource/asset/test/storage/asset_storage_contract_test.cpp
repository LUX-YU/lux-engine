#include <lux/engine/resource/asset/storage/AssetProvider.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    using namespace lux::asset;

    [[nodiscard]] AssetId id(std::uint8_t value)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[0] = 0x51u;
        bytes[6] = 0x40u;
        bytes[8] = 0x80u;
        bytes[15] = value;
        return AssetId{bytes};
    }

    class FakeProvider final : public IAssetProvider
    {
    public:
        struct Record final
        {
            ProviderEntry entry;
            lux::cxx::SharedBytes<> bytes;
        };

        void
        add(AssetId record_id,
            std::string path,
            std::uint32_t magic,
            std::initializer_list<std::byte> bytes,
            bool tombstone = false
        )
        {
            std::vector<std::byte> owned(bytes);
            records_.push_back(Record{
                ProviderEntry{
                    record_id,
                    magic,
                    std::move(path),
                    tombstone,
                },
                lux::cxx::SharedBytes<>::copyOf(owned),
            }
            );
        }

        [[nodiscard]] std::optional<AssetId> resolve(std::string_view path) const override
        {
            for (const auto& record : records_)
            {
                if (record.entry.vpath == path)
                    return record.entry.tombstone ? std::optional<AssetId>{} : std::optional<AssetId>{record.entry.id};
            }
            return std::nullopt;
        }

        [[nodiscard]] bool contains(const AssetId& record_id) const override
        {
            for (const auto& record : records_)
                if (record.entry.id == record_id)
                    return true;
            return false;
        }

        [[nodiscard]] lux::cxx::expected<AssetBlob, EAssetStorageError> open(const AssetId& record_id) const override
        {
            for (const auto& record : records_)
            {
                if (record.entry.id != record_id)
                    continue;
                if (record.entry.tombstone)
                    return lux::cxx::unexpected(EAssetStorageError::NOT_FOUND);
                return AssetBlob::fromShared(record.bytes);
            }
            return lux::cxx::unexpected(EAssetStorageError::NOT_FOUND);
        }

        void enumerate(const std::function<void(const ProviderEntry&)>& fn) const override
        {
            for (const auto& record : records_)
                fn(record.entry);
        }

        [[nodiscard]] std::optional<std::string> pathOf(const AssetId& record_id) const override
        {
            for (const auto& record : records_)
            {
                if (record.entry.id == record_id && !record.entry.tombstone)
                    return record.entry.vpath;
            }
            return std::nullopt;
        }

    private:
        std::vector<Record> records_;
    };

    bool check(bool condition, std::string_view message)
    {
        if (condition)
            return true;
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
}

int
main()
{
    using namespace lux::asset;

    const auto base_id = id(1u);
    const auto patch_id = id(2u);
    const auto deleted_id = id(3u);
    auto base = std::make_shared<FakeProvider>();
    base->add(
        base_id,
        "Opaque/Section",
        0x5345584cu,
        {std::byte{0x4c}, std::byte{0x58}, std::byte{0x45}, std::byte{0x53}}
    );
    base->add(deleted_id, "Opaque/Deleted", 0x4353584cu, {std::byte{0x4c}});

    auto patch = std::make_shared<FakeProvider>();
    patch->add(
        patch_id,
        "Opaque/Section",
        0x5345584cu,
        {std::byte{0x50}, std::byte{0x41}, std::byte{0x54}, std::byte{0x43}}
    );
    patch->add(deleted_id, "Opaque/Deleted", 0x4353584cu, {}, true);

    auto vfs = std::make_shared<AssetVfs>();
    bool success = true;
    success &= check(vfs->mount({"Game", base, 0}) == kInvalidMountId, "illegal mount root is rejected");
    const auto base_mount = vfs->mount({"/Game", base, 0});
    const auto patch_mount = vfs->mount({"/Game", patch, 10});
    success &= check(
        base_mount != kInvalidMountId && patch_mount != kInvalidMountId && vfs->mountCount() == 2u,
        "base and patch providers mount"
    );
    success &=
        check(vfs->resolve("/Game/Opaque/Section") == patch_id, "higher-priority provider shadows the base path");
    const auto opened = vfs->open(patch_id);
    success &= check(
        opened && opened->bytes.size() == 4u && opened->bytes.data()[0] == std::byte{0x50},
        "opaque Section bytes are returned without Asset interpretation"
    );
    success &= check(
        vfs->resolve("/Game/Opaque/Deleted") == deleted_id && !vfs->open(deleted_id),
        "tombstone claims the UUID while the base path remains resolvable"
    );

    std::size_t enumerated = 0u;
    vfs->enumerate([&](const ProviderEntry& entry) {
        if (entry.vpath == "/Game/Opaque/Section" && entry.id == patch_id && entry.magic_number == 0x5345584cu)
        {
            ++enumerated;
        }
    }
    );
    success &= check(enumerated == 1u, "shadow-aware enumeration exposes one winning opaque record");

    const auto direct_open = vfs->open(patch_id);
    success &= check(
        direct_open && direct_open->bytes.size() == 4u,
        "VFS is a synchronous byte mechanism without runtime ownership state"
    );

    vfs->unmount(patch_mount);
    success &= check(vfs->resolve("/Game/Opaque/Section") == base_id, "unmount restores the base provider");

    auto newest = std::make_shared<FakeProvider>();
    newest->add(patch_id, "Opaque/Section", 0x5345584cu, {std::byte{0x4e}});
    vfs->mount({"/Game", newest, 0});
    success &= check(vfs->resolve("/Game/Opaque/Section") == patch_id, "newest equal-priority mount wins");
    return success ? 0 : 1;
}
