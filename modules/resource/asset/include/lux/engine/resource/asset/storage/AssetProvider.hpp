#pragma once

// Provider is the storage-facing byte source. The returned image may be a
// complete .luxasset or another opaque record (for example a Scene Section).
// Interpretation, residency and reference counting belong to the caller.

#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/resource/asset/AssetStorageError.hpp>
#include <lux/engine/resource/asset/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace lux::asset
{
    class AssetReadSource;
    class AssetContentWriter;
    enum class EAssetContentWrite : std::uint8_t;
    class AssetVfsView;

    /// Ownership-erased bytes of a complete storage record. Callers must not
    /// assume the record is an AssetFileHeader image or that it is small.
    struct AssetBlob
    {
        lux::cxx::SharedBytes<> bytes;

        [[nodiscard]] static AssetBlob fromSharedArray(
            std::shared_ptr<const std::byte[]> owner,
            std::size_t size
        ) noexcept
        {
            if (!owner || size == 0u)
                return {};
            const auto* data = owner.get();
            std::shared_ptr<const void> erased{owner, data};
            auto shared = lux::cxx::SharedBytes<>::fromOwner(std::move(erased), std::span<const std::byte>{data, size});
            return shared.empty() ? AssetBlob{} : AssetBlob{std::move(shared)};
        }

        [[nodiscard]] static AssetBlob fromShared(lux::cxx::SharedBytes<> value) noexcept
        {
            return AssetBlob{std::move(value)};
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return !bytes.empty();
        }
    };

    struct ProviderEntry
    {
        AssetId id{};
        std::uint32_t magic_number{0u};
        std::string vpath;
        bool tombstone{false};
    };

    /// UUID/relative-path to opaque bytes. Providers have no residency,
    /// decoding, or asynchronous orchestration behavior. All const read verbs
    /// must support concurrent calls: AssetVfsView can invoke one provider from
    /// multiple runtime/Editor worker threads. Mutable caches therefore require
    /// provider-owned synchronization and must not share a stream cursor.
    class LUX_ASSET_PUBLIC IAssetProvider
    {
    public:
        virtual ~IAssetProvider() = default;

        [[nodiscard]] virtual std::optional<AssetId> resolve(std::string_view rel_vpath) const = 0;

        [[nodiscard]] virtual bool contains(const AssetId& id) const = 0;

        // Reject oversized records before allocating/reading their payload. The bound applies
        // to the uncompressed image, including providers with compressed storage.
        [[nodiscard]] virtual lux::cxx::expected<AssetBlob, EAssetStorageError>
        open(const AssetId& id, std::size_t max_bytes = SIZE_MAX) const = 0;

        virtual void enumerate(const std::function<void(const ProviderEntry&)>& fn) const = 0;

        // Lookup an existing identity without materializing its data. Providers preserve their
        // native index (including paged Pak lookup); no full-catalog scan is required.
        [[nodiscard]] virtual cxx::expected<ProviderEntry, EAssetStorageError> describe(AssetId id) const = 0;

        // Replace an existing record; immutable providers remain explicitly read-only.
        [[nodiscard]] virtual cxx::expected<void, EAssetStorageError>
        replace(AssetId, std::span<const std::byte>)
        {
            return cxx::unexpected(EAssetStorageError::UNSUPPORTED);
        }

        // Physical paths affected by a directory notification. Immutable providers have no changes.
        [[nodiscard]] virtual cxx::expected<std::vector<AssetId>, EAssetStorageError>
        refresh(std::span<const std::filesystem::path>)
        {
            return std::vector<AssetId>{};
        }

        [[nodiscard]] virtual std::optional<std::string> pathOf(const AssetId& id) const = 0;

    protected:
        friend class AssetVfsView;
        friend class AssetContentWriter;
        [[nodiscard]] virtual cxx::expected<AssetReadSource, EAssetStorageError> openSource(AssetId id) noexcept;
        [[nodiscard]] virtual cxx::expected<AssetContentWriter, EAssetStorageError>
        beginContentWrite(
            AssetId id, const AssetReadSource* base, EAssetContentWrite mode, std::string_view new_path
        ) noexcept;
        [[nodiscard]] virtual cxx::expected<void, EAssetStorageError>
        publishContentRoot(
            AssetId id, const AssetReadSource* base, const std::filesystem::path& destination,
            std::span<const std::byte> root
        ) noexcept;
    };
} // namespace lux::asset
