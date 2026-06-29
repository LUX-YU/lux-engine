#include <lux/engine/asset/MeshSerDeser.hpp>
#include <lux/engine/asset/MeshDescriptionCodec.hpp>
#include <AssetManagerImpl.hpp>

#include <fstream>
#include <new>
#include <span>
#include <vector>

namespace lux::asset
{
    using lux::cxx::unexpected;

    MeshSerDeser::MeshSerDeser(std::shared_ptr<AssetManager> manager)
        : TAssetSerDeser<MeshLoadConfig>(std::move(manager))
    {
    }

    namespace
    {
        // Read an entire stream into a byte vector. Mesh blobs are bounded by the
        // codec's vertex/index caps; streaming is unnecessary.
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
    MeshSerDeser::fromFileStream(std::ifstream& /*ifs*/)
    {
        // Raw mesh formats (obj / fbx / glb) are imported via ModelSerDeser
        // (Assimp); a standalone external mesh source format is not supported.
        return unexpected(EAssetError::UNSUPPORTED);
    }

    lux::cxx::expected<std::unique_ptr<Mesh>, EAssetError>
    MeshSerDeser::decodeData(const void* bytes, std::size_t len) noexcept
    {
        // Pure data path: byte image -> Mesh, never touching the manager. Kept
        // noexcept; the only potentially-throwing calls (allocations + the
        // header/blob copies) are guarded so a bad_alloc surfaces as an error
        // code rather than escaping. decodeMeshDescription is itself noexcept.
        try
        {
            if (!bytes || len == 0)
                return unexpected(EAssetError::ABNORMAL_FILE_SIZE);

            std::vector<std::byte> file(
                static_cast<const std::byte*>(bytes),
                static_cast<const std::byte*>(bytes) + len);

            AssetFileHeader header{};
            if (auto ec = loadHeaderRaw<EAssetType::MESH>(file, header);
                ec != EAssetError::SUCCESS)
            {
                return unexpected(ec);
            }
            if (header.magic_number != asset_magic_number_of<EAssetType::MESH>::value)
                return unexpected(EAssetError::WRONG_FILE_HEADER);
            if (header.info_offset != sizeof(AssetFileHeader))
                return unexpected(EAssetError::WRONG_FILE_HEADER);
            if (header.data_offset != header.info_offset + header.info_size)
                return unexpected(EAssetError::WRONG_FILE_HEADER);
            if (header.data_size != 0)
                // The whole mesh lives in the info (description) section; no payload.
                return unexpected(EAssetError::WRONG_FILE_HEADER);
            if (file.size() < header.data_offset)
                return unexpected(EAssetError::ABNORMAL_FILE_SIZE);

            // Info section: binary-encoded Mesh description.
            std::span<const std::byte> mesh_blob(
                file.data() + header.info_offset,
                static_cast<std::size_t>(header.info_size));

            auto mesh = std::make_unique<Mesh>();
            std::string err;
            if (!detail::decodeMeshDescription(mesh_blob, *mesh, &err))
                return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);

            return mesh;
        }
        catch (const std::bad_alloc&)
        {
            return unexpected(EAssetError::OUT_OF_MEMORY);
        }
        catch (...)
        {
            return unexpected(EAssetError::UNKNOWN_ERROR);
        }
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    MeshSerDeser::fromLuxAssetStream(std::istream& ifs)
    {
        std::vector<std::byte> file;
        if (auto ec = readAll(ifs, file); ec != EAssetError::SUCCESS)
            return unexpected(ec);

        // Decode the description into pure Mesh data through the single shared
        // path. The header is re-read inside decodeData; that read is cheap and
        // keeps the decode self-contained (no manager, no LuxAsset). We still
        // need the file header here for the asset id / info, so peek it.
        AssetFileHeader header{};
        if (auto ec = loadHeaderRaw<EAssetType::MESH>(file, header);
            ec != EAssetError::SUCCESS)
        {
            return unexpected(ec);
        }

        auto data = decodeData(file.data(), file.size());
        if (!data.has_value())
            return unexpected(data.error());

        // MeshAsset has no 2-arg ctor (unlike SkeletonAsset); set the data via the
        // public TAsset API so the asset id from the file header is preserved.
        auto ainfo = std::make_unique<AssetInfo>(header.info);
        auto mesh_asset = std::make_unique<MeshAsset>(std::move(ainfo));
        mesh_asset->setData(std::move(data.value()));
        return std::unique_ptr<LuxAsset>(std::move(mesh_asset));
    }

    EAssetError
    MeshSerDeser::exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& ofs)
    {
        const auto* masset = asset.as<MeshAsset>();
        if (!masset)
            return EAssetError::FILE_TYPE_ERROR;
        const Mesh* mesh = static_cast<const Mesh*>(masset->rawData());
        if (!mesh)
            return EAssetError::ASSET_NO_DATA;

        const std::vector<std::byte> mesh_blob = detail::encodeMeshDescription(*mesh);
        const std::size_t info_size = mesh_blob.size();
        const std::size_t data_size = 0;

        const auto header_bytes = makeHeaderRaw<EAssetType::MESH>(
            *masset->info(), info_size, data_size);

        ofs.write(reinterpret_cast<const char*>(header_bytes.data()),
                  static_cast<std::streamsize>(header_bytes.size()));
        if (info_size > 0)
            ofs.write(reinterpret_cast<const char*>(mesh_blob.data()),
                      static_cast<std::streamsize>(info_size));
        return ofs.good() ? EAssetError::SUCCESS : EAssetError::WRITE_FILE_FAIL;
    }
}
