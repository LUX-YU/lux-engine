#include <lux/engine/resource/asset/texture/TextureAtlasAssets.hpp>

#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/resource/asset/detail/CookedAssetWriter.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace lux::asset
{
    namespace
    {
        inline constexpr std::uint32_t kAtlasMagic = 0x5341534CU;
        inline constexpr std::uint32_t kAtlasTrailer = 0x4541534CU;
        inline constexpr std::uint32_t kClipMagic = 0x5343534CU;
        inline constexpr std::uint32_t kClipTrailer = 0x4543534CU;
        inline constexpr std::uint32_t kEndian = 0x01020304U;
        inline constexpr std::uint32_t kVersion = 1U;
        inline constexpr std::uint32_t kMaxString = 64U * 1024U;
        inline constexpr std::uint32_t kMaxItems = 65535U;

        [[nodiscard]] AssetDecodeFailure decodeFailure(EAssetDecodeError code) noexcept
        {
            return AssetDecodeFailure{code, 0U};
        }

        [[nodiscard]] AssetEncodeFailure encodeFailure(EAssetEncodeError code) noexcept
        {
            return AssetEncodeFailure{code, 0U};
        }

        [[nodiscard]] bool canonicalize(std::vector<AssetAuxiliaryPayload>& values) noexcept
        {
            std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) noexcept {
                return left.tag < right.tag;
            });
            for (std::size_t index = 0U; index < values.size(); ++index)
                if (values[index].tag == 0U || values[index].bytes.empty() ||
                    (index != 0U && values[index - 1U].tag == values[index].tag)) return false;
            return true;
        }

        struct Writer final
        {
            std::vector<std::byte> bytes;

            template <class Type>
            void pod(const Type& value)
            {
                const auto offset = bytes.size();
                bytes.resize(offset + sizeof(Type));
                std::memcpy(bytes.data() + offset, &value, sizeof(Type));
            }

            void raw(const void* data, std::size_t size)
            {
                const auto* first = static_cast<const std::byte*>(data);
                bytes.insert(bytes.end(), first, first + size);
            }

            void string(const std::string& value)
            {
                pod(static_cast<std::uint32_t>(value.size()));
                raw(value.data(), value.size());
            }

            void id(AssetId value)
            {
                const auto bytes_value = value.bytes();
                raw(bytes_value.data(), bytes_value.size());
            }
        };

        struct Reader final
        {
            std::span<const std::byte> bytes;
            std::size_t offset{};

            template <class Type>
            [[nodiscard]] bool pod(Type& value) noexcept
            {
                if (sizeof(Type) > bytes.size() - offset) return false;
                std::memcpy(&value, bytes.data() + offset, sizeof(Type));
                offset += sizeof(Type);
                return true;
            }

            [[nodiscard]] bool raw(void* target, std::size_t size) noexcept
            {
                if (size > bytes.size() - offset) return false;
                std::memcpy(target, bytes.data() + offset, size);
                offset += size;
                return true;
            }

            [[nodiscard]] bool string(std::string& value) noexcept
            {
                std::uint32_t size{};
                if (!pod(size) || size > kMaxString || size > bytes.size() - offset) return false;
                value.resize(size);
                return raw(value.data(), size);
            }

            [[nodiscard]] bool id(AssetId& value) noexcept
            {
                std::array<std::uint8_t, 16U> bytes_value{};
                if (!raw(bytes_value.data(), bytes_value.size()))
                    return false;
                value = AssetId{bytes_value};
                return true;
            }
        };

        [[nodiscard]] bool finite(const Eigen::Vector4f& value) noexcept
        {
            return value.allFinite();
        }

        [[nodiscard]] bool validAtlas(const lux::rdesc::TextureAtlas& atlas) noexcept
        {
            if (atlas.name.empty() || atlas.name.size() > kMaxString ||
                atlas.texture.isNull() || atlas.frames.size() > kMaxItems)
            {
                return false;
            }
            for (std::size_t index = 0U; index < atlas.frames.size(); ++index)
            {
                const auto& frame = atlas.frames[index];
                if (frame.name.empty() || frame.name.size() > kMaxString || !finite(frame.uv_rect) ||
                    !frame.pivot.allFinite()) return false;
                for (std::size_t previous = 0U; previous < index; ++previous)
                    if (atlas.frames[previous].name == frame.name) return false;
            }
            return true;
        }

        [[nodiscard]] bool validClip(const lux::rdesc::FlipbookClip& clip) noexcept
        {
            if (clip.name.empty() || clip.name.size() > kMaxString ||
                clip.atlas.isNull() || clip.frames.empty() ||
                clip.frames.size() > kMaxItems || clip.events.size() > kMaxItems)
            {
                return false;
            }
            for (const auto& frame : clip.frames)
                if (!std::isfinite(frame.duration) || frame.duration <= 0.0F) return false;
            for (const auto& event : clip.events)
                if (event.frame_index >= clip.frames.size()) return false;
            return true;
        }

        [[nodiscard]] std::vector<std::byte> encodeAtlas(const lux::rdesc::TextureAtlas& atlas)
        {
            Writer writer;
            writer.pod(kAtlasMagic);
            writer.pod(kEndian);
            writer.pod(kVersion);
            writer.string(atlas.name);
            writer.id(atlas.texture);
            writer.pod(static_cast<std::uint32_t>(atlas.frames.size()));
            for (const auto& frame : atlas.frames)
            {
                writer.string(frame.name);
                for (int index = 0; index < 4; ++index) writer.pod(frame.uv_rect[index]);
                writer.pod(frame.pivot.x());
                writer.pod(frame.pivot.y());
            }
            writer.pod(kAtlasTrailer);
            return std::move(writer.bytes);
        }

        [[nodiscard]] bool decodeAtlas(std::span<const std::byte> bytes, lux::rdesc::TextureAtlas& atlas) noexcept
        {
            Reader reader{bytes};
            std::uint32_t magic{}, endian{}, version{}, count{}, trailer{};
            if (!reader.pod(magic) || !reader.pod(endian) || !reader.pod(version) ||
                magic != kAtlasMagic || endian != kEndian || version != kVersion ||
                !reader.string(atlas.name) || !reader.id(atlas.texture) ||
                !reader.pod(count) || count > kMaxItems)
            {
                return false;
            }
            atlas.frames.resize(count);
            for (auto& frame : atlas.frames)
            {
                if (!reader.string(frame.name)) return false;
                for (int index = 0; index < 4; ++index) if (!reader.pod(frame.uv_rect[index])) return false;
                if (!reader.pod(frame.pivot.x()) || !reader.pod(frame.pivot.y())) return false;
            }
            return reader.pod(trailer) && trailer == kAtlasTrailer && reader.offset == reader.bytes.size() &&
                validAtlas(atlas);
        }

        [[nodiscard]] std::vector<std::byte> encodeClip(const lux::rdesc::FlipbookClip& clip)
        {
            Writer writer;
            writer.pod(kClipMagic);
            writer.pod(kEndian);
            writer.pod(kVersion);
            writer.string(clip.name);
            writer.id(clip.atlas);
            writer.pod(static_cast<std::uint8_t>(clip.loop ? 1U : 0U));
            writer.pod(std::uint8_t{});
            writer.pod(std::uint8_t{});
            writer.pod(std::uint8_t{});
            writer.pod(static_cast<std::uint32_t>(clip.frames.size()));
            for (const auto& frame : clip.frames)
            {
                writer.pod(frame.frame_index);
                writer.pod(frame.duration);
            }
            writer.pod(static_cast<std::uint32_t>(clip.events.size()));
            for (const auto& event : clip.events)
            {
                writer.pod(event.frame_index);
                writer.pod(event.event_id);
            }
            writer.pod(kClipTrailer);
            return std::move(writer.bytes);
        }

        [[nodiscard]] bool decodeClip(std::span<const std::byte> bytes, lux::rdesc::FlipbookClip& clip) noexcept
        {
            Reader reader{bytes};
            std::uint32_t magic{}, endian{}, version{}, frame_count{}, event_count{}, trailer{};
            std::uint8_t loop{}, reserved{};
            if (!reader.pod(magic) || !reader.pod(endian) || !reader.pod(version) ||
                magic != kClipMagic || endian != kEndian || version != kVersion ||
                !reader.string(clip.name) || !reader.id(clip.atlas) ||
                !reader.pod(loop) || loop > 1U || !reader.pod(reserved) || reserved != 0U ||
                !reader.pod(reserved) || reserved != 0U || !reader.pod(reserved) || reserved != 0U ||
                !reader.pod(frame_count) || frame_count > kMaxItems)
            {
                return false;
            }
            clip.loop = loop != 0U;
            clip.frames.resize(frame_count);
            for (auto& frame : clip.frames)
                if (!reader.pod(frame.frame_index) || !reader.pod(frame.duration)) return false;
            if (!reader.pod(event_count) || event_count > kMaxItems) return false;
            clip.events.resize(event_count);
            for (auto& event : clip.events)
                if (!reader.pod(event.frame_index) || !reader.pod(event.event_id)) return false;
            return reader.pod(trailer) && trailer == kClipTrailer && reader.offset == reader.bytes.size() &&
                validClip(clip);
        }

        template <class Asset, class Data, class Decode>
        [[nodiscard]] lux::cxx::expected<std::shared_ptr<const Asset>, AssetDecodeFailure> decodeTyped(
            AssetId requested,
            lux::cxx::SharedBytes<> bytes,
            const AssetDecodeLimits& limits,
            Decode&& decode
        ) noexcept
        {
            auto image = inspectCookedAssetImage(requested, std::move(bytes), limits);
            if (!image) return lux::cxx::unexpected(image.error());
            if (image->magic() != Asset::primary_magic)
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_MAGIC));
            if (image->metadata().legacy_type_tag != Asset::legacy_type_tag)
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_TYPE));
            if (!image->data().empty() || image->information().empty())
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_LAYOUT));
            try
            {
                auto data = std::make_shared<Data>();
                if (!decode(image->information().view(), *data))
                    return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
                std::vector<AssetAuxiliaryPayload> auxiliary(
                    image->auxiliaryPayloads().begin(), image->auxiliaryPayloads().end()
                );
                return Asset::create(
                    AssetInfo{
                        image->metadata().id,
                        Asset::asset_type,
                        image->metadata().date,
                        image->metadata().display_name,
                        image->metadata().source_path,
                        image->metadata().source_mtime
                    },
                    std::move(data),
                    std::move(auxiliary)
                );
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::ALLOCATION_FAILURE));
            }
        }

        template <class Asset>
        [[nodiscard]] lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure> encodeTyped(
            const Asset& asset,
            const AssetEncodeLimits& limits,
            std::vector<std::byte> information
        ) noexcept
        {
            return detail::encodeCookedAssetImage(
                detail::CookedAssetWriteRequest{
                    Asset::primary_magic,
                    Asset::legacy_type_tag,
                    asset.info(),
                    information,
                    {},
                    asset.auxiliaryPayloads()
                },
                limits
            );
        }
    } // namespace

    TextureAtlasAsset::TextureAtlasAsset(
        AssetInfo info,
        std::shared_ptr<const lux::rdesc::TextureAtlas> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
        : TAsset(std::move(info), std::move(data), std::move(auxiliary))
    {
    }

    lux::cxx::expected<std::shared_ptr<const TextureAtlasAsset>, AssetDecodeFailure>
    TextureAtlasAsset::create(
        AssetInfo info,
        std::shared_ptr<const lux::rdesc::TextureAtlas> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
    {
        if (info.id.isNull() || !data || !validAtlas(*data) || !canonicalize(auxiliary))
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
        info.type = asset_type;
        try
        {
            return std::shared_ptr<const TextureAtlasAsset>(
                new TextureAtlasAsset(std::move(info), std::move(data), std::move(auxiliary))
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::ALLOCATION_FAILURE));
        }
    }

    FlipbookClipAsset::FlipbookClipAsset(
        AssetInfo info,
        std::shared_ptr<const lux::rdesc::FlipbookClip> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
        : TAsset(std::move(info), std::move(data), std::move(auxiliary))
    {
    }

    lux::cxx::expected<std::shared_ptr<const FlipbookClipAsset>, AssetDecodeFailure>
    FlipbookClipAsset::create(
        AssetInfo info,
        std::shared_ptr<const lux::rdesc::FlipbookClip> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
    {
        if (info.id.isNull() || !data || !validClip(*data) || !canonicalize(auxiliary))
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
        info.type = asset_type;
        try
        {
            return std::shared_ptr<const FlipbookClipAsset>(
                new FlipbookClipAsset(std::move(info), std::move(data), std::move(auxiliary))
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::ALLOCATION_FAILURE));
        }
    }

    lux::cxx::expected<std::shared_ptr<const TextureAtlasAsset>, AssetDecodeFailure>
    TAssetSerDeser<TextureAtlasAsset>::decode(
        AssetId requested,
        lux::cxx::SharedBytes<> image,
        const AssetDecodeLimits& limits
    ) noexcept
    {
        return decodeTyped<TextureAtlasAsset, lux::rdesc::TextureAtlas>(
            requested, std::move(image), limits, &decodeAtlas
        );
    }

    lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
    TAssetSerDeser<TextureAtlasAsset>::encode(
        const TextureAtlasAsset& asset,
        const AssetEncodeLimits& limits
    ) noexcept
    {
        if (!validAtlas(asset.data()))
            return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::INVALID_ASSET));
        try { return encodeTyped(asset, limits, encodeAtlas(asset.data())); }
        catch (const std::bad_alloc&) {
            return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::ALLOCATION_FAILURE));
        }
    }

    lux::cxx::expected<std::shared_ptr<const FlipbookClipAsset>, AssetDecodeFailure>
    TAssetSerDeser<FlipbookClipAsset>::decode(
        AssetId requested,
        lux::cxx::SharedBytes<> image,
        const AssetDecodeLimits& limits
    ) noexcept
    {
        return decodeTyped<FlipbookClipAsset, lux::rdesc::FlipbookClip>(
            requested, std::move(image), limits, &decodeClip
        );
    }

    lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
    TAssetSerDeser<FlipbookClipAsset>::encode(
        const FlipbookClipAsset& asset,
        const AssetEncodeLimits& limits
    ) noexcept
    {
        if (!validClip(asset.data()))
            return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::INVALID_ASSET));
        try { return encodeTyped(asset, limits, encodeClip(asset.data())); }
        catch (const std::bad_alloc&) {
            return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::ALLOCATION_FAILURE));
        }
    }
} // namespace lux::asset
