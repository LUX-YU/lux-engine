#include <lux/engine/resource/asset/codecs/AnimationClipSerDeser.hpp>
#include <lux/engine/resource/asset/codecs/AnimationClipDescriptionCodec.hpp>
#include <lux/engine/resource/asset/detail/AssetManagerImpl.hpp>

#include <cstring>
#include <fstream>
#include <span>
#include <vector>

namespace lux::asset
{
    using lux::cxx::unexpected;

    AnimationClipSerDeser::AnimationClipSerDeser(std::shared_ptr<AssetManager> manager)
        : TAssetSerDeser<AnimationClipLoadConfig>(std::move(manager))
    {
    }

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
            {
                return EAssetError::READ_FILE_FAIL;
            }
            return EAssetError::SUCCESS;
        }
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    AnimationClipSerDeser::fromFileStream(std::ifstream& /*ifs*/)
    {
        // Animation clips have no standalone external source format — they
        // are emitted by asset_pipeline alongside the skeleton they target.
        return unexpected(EAssetError::UNSUPPORTED);
    }

    lux::cxx::expected<std::unique_ptr<lux::rdesc::AnimationClip>, EAssetError>
    AnimationClipSerDeser::decodeData(const void* bytes, std::size_t len) noexcept
    {
        if (bytes == nullptr)
            return unexpected(EAssetError::ABNORMAL_FILE_SIZE);

        const auto file = std::span<const std::byte>{
            static_cast<const std::byte*>(bytes), len};

        AssetFileHeader header{};
        if (auto ec = loadHeaderRaw<EAssetType::ANIMATION_CLIP>(file, header);
            ec != EAssetError::SUCCESS)
        {
            return unexpected(ec);
        }
        if (header.magic_number != asset_magic_number_of<EAssetType::ANIMATION_CLIP>::value)
            return unexpected(EAssetError::WRONG_FILE_HEADER);
        if (header.info_offset != sizeof(AssetFileHeader))
            return unexpected(EAssetError::WRONG_FILE_HEADER);
        if (header.data_offset != header.info_offset + header.info_size)
            return unexpected(EAssetError::WRONG_FILE_HEADER);
        if (header.data_size != 0)
            return unexpected(EAssetError::WRONG_FILE_HEADER);
        if (file.size() < header.data_offset)
            return unexpected(EAssetError::ABNORMAL_FILE_SIZE);

        // The encoded AnimationClip lives in the INFO section (clips are pure
        // metadata, so the data section is always empty — see header doc).
        std::span<const std::byte> ac_blob(
            file.data() + header.info_offset,
            static_cast<std::size_t>(header.info_size));

        auto clip = std::make_unique<lux::rdesc::AnimationClip>();

        std::string err;
        // decodeAnimationClipDescription is itself noexcept.
        if (!detail::decodeAnimationClipDescription(ac_blob, *clip, &err))
            return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);

        return std::move(clip);
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    AnimationClipSerDeser::fromLuxAssetStream(std::istream& ifs)
    {
        std::vector<std::byte> file;
        if (auto ec = readAll(ifs, file); ec != EAssetError::SUCCESS)
            return unexpected(ec);

        // Header is needed for the AssetInfo carried into the LuxAsset; the
        // clip itself is decoded by decodeData (single source of truth). The
        // extra header peek here is a cheap memcpy + a few comparisons.
        AssetFileHeader header{};
        if (auto ec = loadHeaderRaw<EAssetType::ANIMATION_CLIP>(file, header);
            ec != EAssetError::SUCCESS)
        {
            return unexpected(ec);
        }

        auto decoded = decodeData(file.data(), file.size());
        if (!decoded)
            return unexpected(decoded.error());

        auto ainfo = std::make_unique<AssetInfo>(header.info);
        return std::make_unique<AnimationClipAsset>(
            std::move(ainfo), std::move(*decoded));
    }

    EAssetError
    AnimationClipSerDeser::exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& ofs)
    {
        const auto* casset = asset.as<AnimationClipAsset>();
        if (!casset)
            return EAssetError::FILE_TYPE_ERROR;
        const auto* clip = static_cast<const lux::rdesc::AnimationClip*>(
            casset->rawData());
        if (!clip)
            return EAssetError::ASSET_NO_DATA;

        const std::vector<std::byte> ac_blob = detail::encodeAnimationClipDescription(*clip);
        const std::size_t info_size = ac_blob.size();
        const std::size_t data_size = 0;

        const auto header_bytes = makeHeaderRaw<EAssetType::ANIMATION_CLIP>(
            *casset->info(), info_size, data_size);

        ofs.write(reinterpret_cast<const char*>(header_bytes.data()),
                  static_cast<std::streamsize>(header_bytes.size()));
        if (info_size > 0)
            ofs.write(reinterpret_cast<const char*>(ac_blob.data()),
                      static_cast<std::streamsize>(info_size));
        return ofs.good() ? EAssetError::SUCCESS : EAssetError::WRITE_FILE_FAIL;
    }
}
