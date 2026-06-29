#include <lux/engine/asset/SkeletonSerDeser.hpp>
#include <lux/engine/asset/SkeletonDescriptionCodec.hpp>
#include <AssetManagerImpl.hpp>

#include <cstring>
#include <fstream>
#include <new>
#include <span>
#include <vector>

namespace lux::asset
{
    using lux::cxx::unexpected;

    SkeletonSerDeser::SkeletonSerDeser(std::shared_ptr<AssetManager> manager)
        : TAssetSerDeser<SkeletonLoadConfig>(std::move(manager))
    {
    }

    namespace
    {
        // Read an entire stream into a byte vector. Skeleton blobs are
        // measured in tens-of-kilobytes for typical humanoid rigs;
        // streaming is unnecessary.
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
    SkeletonSerDeser::fromFileStream(std::ifstream& /*ifs*/)
    {
        // Skeletons have no standalone external source format — they are
        // emitted by asset_pipeline alongside the mesh they were extracted
        // from. Direct external import is therefore not meaningful.
        return unexpected(EAssetError::UNSUPPORTED);
    }

    lux::cxx::expected<std::unique_ptr<Skeleton>, EAssetError>
    SkeletonSerDeser::decodeData(const void* bytes, std::size_t len) noexcept
    {
        if (bytes == nullptr || len == 0)
            return unexpected(EAssetError::ABNORMAL_FILE_SIZE);

        // The body allocates (vector copy + Skeleton); decodeData is contracted
        // noexcept (it is the worker-thread entry point), so the only thing that
        // can throw — std::bad_alloc — is turned into an OUT_OF_MEMORY error
        // rather than crossing the noexcept boundary. The codec itself is
        // already noexcept and reports via its bool return.
        try
        {
            std::vector<std::byte> file(len);
            std::memcpy(file.data(), bytes, len);

            AssetFileHeader header{};
            if (auto ec = loadHeaderRaw<EAssetType::SKELETON>(file, header);
                ec != EAssetError::SUCCESS)
            {
                return unexpected(ec);
            }
            if (header.magic_number != asset_magic_number_of<EAssetType::SKELETON>::value)
                return unexpected(EAssetError::WRONG_FILE_HEADER);
            if (header.info_offset != sizeof(AssetFileHeader))
                return unexpected(EAssetError::WRONG_FILE_HEADER);
            if (header.data_offset != header.info_offset + header.info_size)
                return unexpected(EAssetError::WRONG_FILE_HEADER);
            if (header.data_size != 0)
                // Skeletons have no payload section by design.
                return unexpected(EAssetError::WRONG_FILE_HEADER);
            if (file.size() < header.info_offset + header.info_size)
                return unexpected(EAssetError::ABNORMAL_FILE_SIZE);

            // Info section: binary-encoded Skeleton description.
            std::span<const std::byte> sk_blob(
                file.data() + header.info_offset,
                static_cast<std::size_t>(header.info_size));

            auto skeleton = std::make_unique<Skeleton>();
            std::string err;
            if (!detail::decodeSkeletonDescription(sk_blob, *skeleton, &err))
                return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);

            return skeleton;
        }
        catch (const std::bad_alloc&)
        {
            return unexpected(EAssetError::OUT_OF_MEMORY);
        }
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    SkeletonSerDeser::fromLuxAssetStream(std::istream& ifs)
    {
        std::vector<std::byte> file;
        if (auto ec = readAll(ifs, file); ec != EAssetError::SUCCESS)
            return unexpected(ec);

        // The header is needed for the AssetInfo; the pure Skeleton data decode
        // (header validation + info-section slice + codec) is owned by
        // decodeData() so the two paths never diverge.
        AssetFileHeader header{};
        if (auto ec = loadHeaderRaw<EAssetType::SKELETON>(file, header);
            ec != EAssetError::SUCCESS)
        {
            return unexpected(ec);
        }

        auto skeleton = decodeData(file.data(), file.size());
        if (!skeleton.has_value())
            return unexpected(skeleton.error());

        auto ainfo = std::make_unique<AssetInfo>(header.info);
        return std::make_unique<SkeletonAsset>(
            std::move(ainfo), std::move(skeleton.value()));
    }

    EAssetError
    SkeletonSerDeser::exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& ofs)
    {
        const auto* sasset = asset.as<SkeletonAsset>();
        if (!sasset)
            return EAssetError::FILE_TYPE_ERROR;
        const Skeleton* skel = static_cast<const Skeleton*>(sasset->rawData());
        if (!skel)
            return EAssetError::ASSET_NO_DATA;

        const std::vector<std::byte> sk_blob = detail::encodeSkeletonDescription(*skel);
        const std::size_t info_size = sk_blob.size();
        const std::size_t data_size = 0;

        const auto header_bytes = makeHeaderRaw<EAssetType::SKELETON>(
            *sasset->info(), info_size, data_size);

        ofs.write(reinterpret_cast<const char*>(header_bytes.data()),
                  static_cast<std::streamsize>(header_bytes.size()));
        if (info_size > 0)
            ofs.write(reinterpret_cast<const char*>(sk_blob.data()),
                      static_cast<std::streamsize>(info_size));
        return ofs.good() ? EAssetError::SUCCESS : EAssetError::WRITE_FILE_FAIL;
    }
}
