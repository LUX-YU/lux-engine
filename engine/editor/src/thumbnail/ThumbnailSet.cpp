#include <lux/engine/editor/thumbnail/ThumbnailSet.hpp>

#include <cstring>   // std::memcpy

namespace lux::editor
{
    namespace
    {
        // POD blob headers — memcpy'd, matching the engine's native-LE convention.
        struct ThumbnailSetHeader
        {
            std::uint32_t magic;     ///< kSetMagic
            std::uint32_t version;   ///< kSetVersion
            std::uint32_t count;     ///< number of images
            std::uint32_t reserved;  ///< 0
        };

        struct ThumbnailImageHeader
        {
            std::uint32_t width;
            std::uint32_t height;
            std::uint32_t encoding;  ///< EThumbnailEncoding
            std::uint32_t byte_size; ///< image byte count following this header
        };

        constexpr std::uint32_t kSetMagic   = 0x53544C58u; // 'X''L''T''S' (LE "XLTS")
        constexpr std::uint32_t kSetVersion = 1u;

        template <class T>
        void appendPod(std::vector<std::byte>& out, const T& v)
        {
            const auto* p = reinterpret_cast<const std::byte*>(&v);
            out.insert(out.end(), p, p + sizeof(T));
        }
    } // namespace

    const ThumbnailImage* ThumbnailSet::best(std::uint32_t w, std::uint32_t h) const noexcept
    {
        const ThumbnailImage* fit     = nullptr;
        const ThumbnailImage* largest = nullptr;
        for (const auto& img : images)
        {
            if (!largest ||
                static_cast<std::uint64_t>(img.width) * img.height >
                static_cast<std::uint64_t>(largest->width) * largest->height)
                largest = &img;

            if (img.width >= w && img.height >= h)
            {
                // Smallest image that still covers the request.
                if (!fit ||
                    static_cast<std::uint64_t>(img.width) * img.height <
                    static_cast<std::uint64_t>(fit->width) * fit->height)
                    fit = &img;
            }
        }
        return fit ? fit : largest;
    }

    std::vector<std::byte> encodeThumbnailSet(const ThumbnailSet& set)
    {
        std::vector<std::byte> out;

        ThumbnailSetHeader sh{kSetMagic, kSetVersion,
                              static_cast<std::uint32_t>(set.images.size()), 0u};
        appendPod(out, sh);

        for (const auto& img : set.images)
        {
            ThumbnailImageHeader ih{
                img.width, img.height,
                static_cast<std::uint32_t>(img.encoding),
                static_cast<std::uint32_t>(img.bytes.size())
            };
            appendPod(out, ih);
            out.insert(out.end(), img.bytes.begin(), img.bytes.end());
        }
        return out;
    }

    std::optional<ThumbnailSet> decodeThumbnailSet(std::span<const std::byte> blob)
    {
        if (blob.size() < sizeof(ThumbnailSetHeader))
            return std::nullopt;

        ThumbnailSetHeader sh{};
        std::memcpy(&sh, blob.data(), sizeof(sh));
        if (sh.magic != kSetMagic || sh.version != kSetVersion)
            return std::nullopt;

        ThumbnailSet set;
        set.images.reserve(sh.count);

        std::size_t off = sizeof(sh);
        for (std::uint32_t i = 0; i < sh.count; ++i)
        {
            if (off + sizeof(ThumbnailImageHeader) > blob.size())
                return std::nullopt;

            ThumbnailImageHeader ih{};
            std::memcpy(&ih, blob.data() + off, sizeof(ih));
            off += sizeof(ih);

            if (off + ih.byte_size > blob.size())
                return std::nullopt;

            ThumbnailImage img;
            img.width    = ih.width;
            img.height   = ih.height;
            img.encoding = static_cast<EThumbnailEncoding>(ih.encoding);
            img.bytes.assign(blob.data() + off, blob.data() + off + ih.byte_size);
            off += ih.byte_size;

            set.images.push_back(std::move(img));
        }
        return set;
    }

    std::optional<ThumbnailSet> readThumbnailSet(const lux::asset::LuxAsset& asset)
    {
        if (const auto* blob = asset.payload(kThumbnailPayloadTag))
            return decodeThumbnailSet(*blob);
        return std::nullopt;
    }

    void writeThumbnailSet(lux::asset::LuxAsset& asset, const ThumbnailSet& set)
    {
        asset.setPayload(kThumbnailPayloadTag, encodeThumbnailSet(set));
    }

} // namespace lux::editor
