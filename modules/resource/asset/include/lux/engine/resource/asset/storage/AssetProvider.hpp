#pragma once

// Provider is the storage-facing byte source. The returned image may be a
// complete .luxasset or another opaque record (for example a Scene Section).
// Interpretation, residency and reference counting belong to the caller.

#include <lux/engine/resource/asset/AssetId.hpp>
#include <lux/engine/resource/asset/AssetStorageError.hpp>
#include <lux/engine/resource/asset/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace lux::asset
{
    /// Ownership-erased bytes of a complete storage record. Callers must not
    /// assume the record is an AssetFileHeader image or that it is small.
    struct AssetBlob
    {
        lux::cxx::SharedBytes<> bytes;

        [[nodiscard]] static AssetBlob
        fromSharedArray(std::shared_ptr<const std::byte[]> owner, std::size_t size) noexcept
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

    /// UUID/relative-path to opaque bytes. Providers have no cache, residency,
    /// reference-counting, decoding, or asynchronous orchestration behavior.
    class LUX_ASSET_PUBLIC IAssetProvider
    {
    public:
        virtual ~IAssetProvider() = default;

        [[nodiscard]] virtual std::optional<AssetId> resolve(std::string_view rel_vpath) const = 0;

        [[nodiscard]] virtual bool contains(const AssetId& id) const = 0;

        [[nodiscard]] virtual lux::cxx::expected<AssetBlob, EAssetStorageError> open(const AssetId& id) const = 0;

        virtual void enumerate(const std::function<void(const ProviderEntry&)>& fn) const = 0;

        [[nodiscard]] virtual std::optional<std::string> pathOf(const AssetId& id) const = 0;
    };
} // namespace lux::asset
