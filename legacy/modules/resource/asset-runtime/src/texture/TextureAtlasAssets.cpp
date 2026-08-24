/**
 * @file TextureAtlasAssets.cpp
 * @brief TextureAtlas / Flipbook cooked codecs and SerDesers.
 */

#include <lux/engine/resource/asset/texture/TextureAtlasSerDeser.hpp>
#include <lux/engine/resource/asset/texture/TextureAtlasDescriptionCodec.hpp>

#include <cstring>
#include <fstream>
#include <span>
#include <vector>

namespace lux::asset
{
    using lux::cxx::unexpected;

    // ── codec (private) ──────────────────────────────────────────────────────

    namespace detail
    {
        namespace
        {
            struct Writer
            {
                std::vector<std::byte>& out;
                void bytes(const void* p, std::size_t n)
                {
                    const auto* b = static_cast<const std::byte*>(p);
                    out.insert(out.end(), b, b + n);
                }
                void u8 (std::uint8_t v)  { bytes(&v, 1); }
                void u32(std::uint32_t v) { bytes(&v, 4); }
                void f32(float v)         { bytes(&v, 4); }
                void str(const std::string& s)
                {
                    u32(static_cast<std::uint32_t>(s.size()));
                    bytes(s.data(), s.size());
                }
            };

            struct Reader
            {
                std::span<const std::byte> in;
                std::size_t                pos{0};
                std::string*               err;
                bool                       failed{false};

                bool fail(const char* what)
                {
                    if (err && !failed) *err = what;
                    failed = true;
                    return false;
                }
                bool bytes(void* p, std::size_t n)
                {
                    if (failed) return false;
                    if (pos + n > in.size()) return fail("unexpected end of blob");
                    std::memcpy(p, in.data() + pos, n);
                    pos += n;
                    return true;
                }
                std::uint8_t  u8 () { std::uint8_t v{};  bytes(&v, 1); return v; }
                std::uint32_t u32() { std::uint32_t v{}; bytes(&v, 4); return v; }
                float         f32() { float v{};         bytes(&v, 4); return v; }
                bool str(std::string& s)
                {
                    const std::uint32_t n = u32();
                    if (failed) return false;
                    if (n > kMaxS2StringLen) return fail("string too long");
                    s.resize(n);
                    return n == 0 || bytes(s.data(), n);
                }
            };

            bool readFraming(Reader& r, std::uint32_t magic)
            {
                if (r.u32() != magic)          return r.fail("wrong description magic");
                if (r.u32() != kS2EndianTag)   return r.fail("wrong endian tag");
                if (r.u32() != kS2SchemaVersion) return r.fail("unsupported schema version");
                return !r.failed;
            }
        }

        std::vector<std::byte> encodeTextureAtlasDescription(const lux::rdesc::TextureAtlas& atlas)
        {
            std::vector<std::byte> out;
            Writer w{out};
            w.u32(kSaDescMagic);
            w.u32(kS2EndianTag);
            w.u32(kS2SchemaVersion);
            w.str(atlas.name);
            w.bytes(atlas.texture_uuid.data(), atlas.texture_uuid.size());
            w.u32(static_cast<std::uint32_t>(atlas.frames.size()));
            for (const auto& f : atlas.frames)
            {
                w.str(f.name);
                for (int i = 0; i < 4; ++i) w.f32(f.uv_rect[i]);
                w.f32(f.pivot.x());
                w.f32(f.pivot.y());
            }
            w.u32(kSaDescTrailer);
            return out;
        }

        bool decodeTextureAtlasDescription(std::span<const std::byte> blob,
                                          lux::rdesc::TextureAtlas&   out,
                                          std::string*               error_out) noexcept
        {
            Reader r{blob, 0, error_out};
            if (!readFraming(r, kSaDescMagic)) return false;
            if (!r.str(out.name)) return false;
            if (!r.bytes(out.texture_uuid.data(), out.texture_uuid.size())) return false;
            const std::uint32_t n = r.u32();
            if (r.failed) return false;
            if (n > kMaxS2FrameCount) return r.fail("frame count too large");
            out.frames.resize(n);
            for (auto& f : out.frames)
            {
                if (!r.str(f.name)) return false;
                for (int i = 0; i < 4; ++i) f.uv_rect[i] = r.f32();
                f.pivot.x() = r.f32();
                f.pivot.y() = r.f32();
                if (r.failed) return false;
            }
            if (r.u32() != kSaDescTrailer) return r.fail("wrong trailer magic");
            return !r.failed;
        }

        std::vector<std::byte> encodeFlipbookClipDescription(const lux::rdesc::FlipbookClip& clip)
        {
            std::vector<std::byte> out;
            Writer w{out};
            w.u32(kScDescMagic);
            w.u32(kS2EndianTag);
            w.u32(kS2SchemaVersion);
            w.str(clip.name);
            w.bytes(clip.atlas_uuid.data(), clip.atlas_uuid.size());
            w.u8(clip.loop ? 1u : 0u);
            w.u8(0); w.u8(0); w.u8(0);
            w.u32(static_cast<std::uint32_t>(clip.frames.size()));
            for (const auto& f : clip.frames)
            {
                w.u32(f.frame_index);
                w.f32(f.duration);
            }
            w.u32(static_cast<std::uint32_t>(clip.events.size()));
            for (const auto& e : clip.events)
            {
                w.u32(e.frame_index);
                w.u32(e.event_id);
            }
            w.u32(kScDescTrailer);
            return out;
        }

        bool decodeFlipbookClipDescription(std::span<const std::byte>  blob,
                                             lux::rdesc::FlipbookClip& out,
                                             std::string*                error_out) noexcept
        {
            Reader r{blob, 0, error_out};
            if (!readFraming(r, kScDescMagic)) return false;
            if (!r.str(out.name)) return false;
            if (!r.bytes(out.atlas_uuid.data(), out.atlas_uuid.size())) return false;
            out.loop = (r.u8() != 0);
            r.u8(); r.u8(); r.u8();
            const std::uint32_t nf = r.u32();
            if (r.failed) return false;
            if (nf > kMaxS2FrameCount) return r.fail("frame count too large");
            out.frames.resize(nf);
            for (auto& f : out.frames)
            {
                f.frame_index = r.u32();
                f.duration    = r.f32();
                if (r.failed) return false;
                if (!(f.duration > 0.f)) return r.fail("non-positive frame duration");
            }
            const std::uint32_t ne = r.u32();
            if (r.failed) return false;
            if (ne > kMaxS2EventCount) return r.fail("event count too large");
            out.events.resize(ne);
            for (auto& e : out.events)
            {
                e.frame_index = r.u32();
                e.event_id    = r.u32();
                if (r.failed) return false;
            }
            if (r.u32() != kScDescTrailer) return r.fail("wrong trailer magic");
            return !r.failed;
        }
    } // namespace detail

    // ── serdesers (both mirror AnimationClipSerDeser exactly) ────────────────

    namespace
    {
        EAssetError readAll(std::istream& ifs, std::vector<std::byte>& out)
        {
            ifs.seekg(0, std::ios::end);
            const std::streamoff n = ifs.tellg();
            if (n < 0) return EAssetError::ABNORMAL_FILE_SIZE;
            ifs.seekg(0, std::ios::beg);
            out.resize(static_cast<std::size_t>(n));
            if (n == 0) return EAssetError::SUCCESS;
            if (!ifs.read(reinterpret_cast<char*>(out.data()),
                          static_cast<std::streamsize>(out.size())))
                return EAssetError::READ_FILE_FAIL;
            return EAssetError::SUCCESS;
        }

        /// The shared "decode a pure-metadata .luxasset image" skeleton: header
        /// checks + info-section decode via @p decode_fn into a fresh @p T.
        template <EAssetType Type, class T, class DecodeFn>
        lux::cxx::expected<std::unique_ptr<T>, EAssetError>
        decodeMetadataImage(const void* bytes, std::size_t len, DecodeFn&& decode_fn) noexcept
        {
            if (bytes == nullptr)
                return unexpected(EAssetError::ABNORMAL_FILE_SIZE);

            const auto file = std::span<const std::byte>{
                static_cast<const std::byte*>(bytes), len};

            AssetFileHeader header{};
            if (auto ec = AssetSerDeser::loadHeaderRaw<Type>(file, header); ec != EAssetError::SUCCESS)
                return unexpected(ec);
            if (header.magic_number != asset_magic_number_of<Type>::value)
                return unexpected(EAssetError::WRONG_FILE_HEADER);
            if (header.info_offset != assetFileHeaderSize(header.version))
                return unexpected(EAssetError::WRONG_FILE_HEADER);
            if (header.data_offset != header.info_offset + header.info_size)
                return unexpected(EAssetError::WRONG_FILE_HEADER);
            if (header.data_size != 0)
                return unexpected(EAssetError::WRONG_FILE_HEADER);
            if (file.size() < header.data_offset)
                return unexpected(EAssetError::ABNORMAL_FILE_SIZE);

            std::span<const std::byte> blob(
                file.data() + header.info_offset,
                static_cast<std::size_t>(header.info_size));

            auto data = std::make_unique<T>();

            std::string err;
            if (!decode_fn(blob, *data, &err))
                return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
            return std::move(data);
        }

        template <EAssetType Type>
        EAssetError writeMetadataImage(const AssetInfo& info,
                                       const std::vector<std::byte>& blob,
                                       std::ofstream& ofs)
        {
            const auto header_bytes = AssetSerDeser::makeHeaderRaw<Type>(info, blob.size(), /*data_size=*/0);
            ofs.write(reinterpret_cast<const char*>(header_bytes.data()),
                      static_cast<std::streamsize>(header_bytes.size()));
            if (!blob.empty())
                ofs.write(reinterpret_cast<const char*>(blob.data()),
                          static_cast<std::streamsize>(blob.size()));
            return ofs.good() ? EAssetError::SUCCESS : EAssetError::WRITE_FILE_FAIL;
        }
    }

    TextureAtlasSerDeser::TextureAtlasSerDeser(std::shared_ptr<AssetManager> manager)
        : TAssetSerDeser<TextureAtlasLoadConfig>(std::move(manager))
    {
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    TextureAtlasSerDeser::fromFileStream(std::ifstream& /*ifs*/)
    {
        return unexpected(EAssetError::UNSUPPORTED);   // no standalone external format
    }

    lux::cxx::expected<std::unique_ptr<lux::rdesc::TextureAtlas>, EAssetError>
    TextureAtlasSerDeser::decodeData(const void* bytes, std::size_t len) noexcept
    {
        return decodeMetadataImage<
            EAssetType::TEXTURE_ATLAS,
            lux::rdesc::TextureAtlas>(
            bytes, len, &detail::decodeTextureAtlasDescription);
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    TextureAtlasSerDeser::fromLuxAssetStream(std::istream& ifs)
    {
        std::vector<std::byte> file;
        if (auto ec = readAll(ifs, file); ec != EAssetError::SUCCESS)
            return unexpected(ec);

        AssetFileHeader header{};
        if (auto ec = loadHeaderRaw<EAssetType::TEXTURE_ATLAS>(file, header);
            ec != EAssetError::SUCCESS)
            return unexpected(ec);

        auto decoded = decodeData(file.data(), file.size());
        if (!decoded)
            return unexpected(decoded.error());

        auto ainfo = std::make_unique<AssetInfo>(header.info);
        return std::make_unique<TextureAtlasAsset>(std::move(ainfo), std::move(*decoded));
    }

    EAssetError
    TextureAtlasSerDeser::exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& ofs)
    {
        const auto* aasset = asset.as<TextureAtlasAsset>();
        if (!aasset)
            return EAssetError::FILE_TYPE_ERROR;
        const auto* atlas = static_cast<const lux::rdesc::TextureAtlas*>(
            aasset->data());
        if (!atlas)
            return EAssetError::ASSET_NO_DATA;
        return writeMetadataImage<EAssetType::TEXTURE_ATLAS>(
            *aasset->info(), detail::encodeTextureAtlasDescription(*atlas), ofs);
    }

    FlipbookClipSerDeser::FlipbookClipSerDeser(std::shared_ptr<AssetManager> manager)
        : TAssetSerDeser<FlipbookClipLoadConfig>(std::move(manager))
    {
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    FlipbookClipSerDeser::fromFileStream(std::ifstream& /*ifs*/)
    {
        return unexpected(EAssetError::UNSUPPORTED);
    }

    lux::cxx::expected<std::unique_ptr<lux::rdesc::FlipbookClip>, EAssetError>
    FlipbookClipSerDeser::decodeData(const void* bytes, std::size_t len) noexcept
    {
        return decodeMetadataImage<
            EAssetType::FLIPBOOK_CLIP,
            lux::rdesc::FlipbookClip>(
            bytes, len, &detail::decodeFlipbookClipDescription);
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    FlipbookClipSerDeser::fromLuxAssetStream(std::istream& ifs)
    {
        std::vector<std::byte> file;
        if (auto ec = readAll(ifs, file); ec != EAssetError::SUCCESS)
            return unexpected(ec);

        AssetFileHeader header{};
        if (auto ec = loadHeaderRaw<EAssetType::FLIPBOOK_CLIP>(file, header);
            ec != EAssetError::SUCCESS)
            return unexpected(ec);

        auto decoded = decodeData(file.data(), file.size());
        if (!decoded)
            return unexpected(decoded.error());

        auto ainfo = std::make_unique<AssetInfo>(header.info);
        return std::make_unique<FlipbookClipAsset>(std::move(ainfo), std::move(*decoded));
    }

    EAssetError
    FlipbookClipSerDeser::exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& ofs)
    {
        const auto* casset = asset.as<FlipbookClipAsset>();
        if (!casset)
            return EAssetError::FILE_TYPE_ERROR;
        const auto* clip = static_cast<const lux::rdesc::FlipbookClip*>(
            casset->data());
        if (!clip)
            return EAssetError::ASSET_NO_DATA;
        return writeMetadataImage<EAssetType::FLIPBOOK_CLIP>(
            *casset->info(), detail::encodeFlipbookClipDescription(*clip), ofs);
    }
}
