#include <lux/engine/resource/asset/animation/SkeletonSerDeser.hpp>
#include <lux/engine/resource/asset/animation/SkeletonDescriptionCodec.hpp>
#include <lux/engine/resource/asset/detail/AssetManagerImpl.hpp>

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

    lux::cxx::expected<std::unique_ptr<lux::rdesc::Skeleton>, EAssetError>
    SkeletonSerDeser::decodeData(const void* bytes, std::size_t len) noexcept
    {
        if (bytes == nullptr || len == 0)
            return unexpected(EAssetError::ABNORMAL_FILE_SIZE);

        const auto file = std::span<const std::byte>{
            static_cast<const std::byte*>(bytes), len};

            AssetFileHeader header{};
            if (auto ec = loadHeaderRaw<EAssetType::SKELETON>(file, header);
                ec != EAssetError::SUCCESS)
            {
                return unexpected(ec);
            }
            if (header.magic_number != asset_magic_number_of<EAssetType::SKELETON>::value)
                return unexpected(EAssetError::WRONG_FILE_HEADER);
            if (header.info_offset != assetFileHeaderSize(header.version))
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

            auto skeleton = std::make_unique<lux::rdesc::Skeleton>();
            std::string err;
            if (!detail::decodeSkeletonDescription(sk_blob, *skeleton, &err))
                return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);

        return skeleton;
    }

    lux::cxx::expected<std::vector<std::byte>, EAssetError>
    SkeletonSerDeser::encodeData(
        const asset_id_t& id,
        const lux::rdesc::Skeleton& skeleton) noexcept
    {
        if (id.is_nil() || skeleton.bones.empty())
            return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
        for (std::size_t index = 0u; index < skeleton.bones.size(); ++index)
        {
            const auto& bone = skeleton.bones[index];
            if (bone.name.empty() || bone.parent_index >=
                    static_cast<std::int32_t>(index) ||
                !bone.bind_local.matrix().allFinite() ||
                !bone.inv_bind_world.matrix().allFinite())
            {
                return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
            }
        }
        if (!skeleton.global_transform.matrix().allFinite())
            return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);

        const auto blob = detail::encodeSkeletonDescription(skeleton);
        AssetInfo info{};
        info.id = id;
        info.type = EAssetType::SKELETON;
        constexpr std::string_view name{"Generated Benchmark Skeleton"};
        std::memcpy(info.display_name, name.data(), name.size());
        auto image = makeHeaderRaw<EAssetType::SKELETON>(
            info, blob.size(), 0u);
        image.insert(image.end(), blob.begin(), blob.end());

        const auto verified = decodeData(image.data(), image.size());
        if (!verified)
            return unexpected(verified.error());
        return image;
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
        const auto* skel = static_cast<const lux::rdesc::Skeleton*>(
            sasset->rawData());
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
