#include <lux/engine/resource/asset/ModelCodec.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>
#include <vector>

namespace lux::asset
{
    namespace
    {
        constexpr std::uint32_t kLuxModelFormatVersion = 1;

        template <typename T>
        void appendPod(std::vector<std::byte>& bytes, const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            const auto offset = bytes.size();
            bytes.resize(offset + sizeof(T));
            std::memcpy(bytes.data() + offset, &value, sizeof(T));
        }

        void appendUuid(
            std::vector<std::byte>& bytes,
            const asset_id_t& id
        )
        {
            const auto id_bytes = id.as_bytes();
            const auto offset = bytes.size();
            bytes.resize(offset + id_bytes.size());
            std::memcpy(
                bytes.data() + offset,
                id_bytes.data(),
                id_bytes.size()
            );
        }

        void appendUuidVector(
            std::vector<std::byte>& bytes,
            const std::vector<asset_id_t>& ids
        )
        {
            appendPod<std::uint32_t>(
                bytes,
                static_cast<std::uint32_t>(ids.size())
            );
            for (const auto& id : ids)
            {
                appendUuid(bytes, id);
            }
        }

        struct InfoCursor
        {
            const std::byte* current{};
            const std::byte* end{};

            [[nodiscard]] bool has(std::size_t count) const noexcept
            {
                return end - current >= static_cast<std::ptrdiff_t>(count);
            }

            template <typename T>
            [[nodiscard]] std::optional<T> readPod() noexcept
            {
                if (!has(sizeof(T)))
                {
                    return std::nullopt;
                }
                T value{};
                std::memcpy(&value, current, sizeof(T));
                current += sizeof(T);
                return value;
            }

            [[nodiscard]] std::optional<asset_id_t> readUuid() noexcept
            {
                if (!has(16))
                {
                    return std::nullopt;
                }
                std::array<std::uint8_t, 16> bytes{};
                std::memcpy(bytes.data(), current, bytes.size());
                current += bytes.size();
                return uuids::uuid(bytes);
            }

            [[nodiscard]] bool readUuidVector(
                std::vector<asset_id_t>& output
            ) noexcept
            {
                const auto count = readPod<std::uint32_t>();
                if (!count)
                {
                    return false;
                }
                output.reserve(*count);
                for (std::uint32_t index = 0; index < *count; ++index)
                {
                    const auto id = readUuid();
                    if (!id)
                    {
                        return false;
                    }
                    output.push_back(*id);
                }
                return true;
            }
        };

        EAssetError readAll(
            std::istream& input,
            std::vector<std::byte>& output
        )
        {
            input.seekg(0, std::ios::end);
            const auto size = input.tellg();
            if (size < 0)
            {
                return EAssetError::ABNORMAL_FILE_SIZE;
            }
            input.seekg(0, std::ios::beg);
            output.resize(static_cast<std::size_t>(size));
            if (output.empty())
            {
                return EAssetError::SUCCESS;
            }
            if (!input.read(
                    reinterpret_cast<char*>(output.data()),
                    static_cast<std::streamsize>(output.size())
                ))
            {
                return EAssetError::READ_FILE_FAIL;
            }
            return EAssetError::SUCCESS;
        }
    } // namespace

    ModelCodec::ModelCodec(std::shared_ptr<AssetManager> manager)
        : TAssetSerDeser(std::move(manager))
    {
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    ModelCodec::fromLuxAssetStream(std::istream& input)
    {
        std::vector<std::byte> file;
        if (const auto error = readAll(input, file);
            error != EAssetError::SUCCESS)
        {
            return lux::cxx::unexpected(error);
        }

        AssetFileHeader header{};
        if (const auto error = loadHeaderRaw<EAssetType::MODEL>(file, header);
            error != EAssetError::SUCCESS)
        {
            return lux::cxx::unexpected(error);
        }
        if (header.info_offset != sizeof(AssetFileHeader) ||
            header.data_offset != header.info_offset + header.info_size ||
            header.data_size != 0 ||
            file.size() < header.data_offset)
        {
            return lux::cxx::unexpected(EAssetError::WRONG_FILE_HEADER);
        }

        InfoCursor cursor{
            file.data() + header.info_offset,
            file.data() + header.info_offset + header.info_size
        };
        const auto version = cursor.readPod<std::uint32_t>();
        const auto has_skeleton = cursor.readPod<std::uint8_t>();
        const auto skeleton = cursor.readUuid();
        if (!version || *version != kLuxModelFormatVersion ||
            !has_skeleton || !skeleton)
        {
            return lux::cxx::unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
        }

        std::vector<asset_id_t> meshes;
        std::vector<asset_id_t> materials;
        std::vector<asset_id_t> textures;
        std::vector<asset_id_t> animations;
        if (!cursor.readUuidVector(meshes) ||
            !cursor.readUuidVector(materials) ||
            !cursor.readUuidVector(textures) ||
            !cursor.readUuidVector(animations))
        {
            return lux::cxx::unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
        }

        auto model = std::make_unique<ModelAsset>(
            std::make_unique<AssetInfo>(header.info)
        );
        for (const auto& id : meshes)
        {
            model->addMeshAssetId(id);
        }
        for (const auto& id : materials)
        {
            model->addMaterialAssetId(id);
        }
        for (const auto& id : animations)
        {
            model->addAnimationClipAssetId(id);
        }
        if (*has_skeleton)
        {
            model->setSkeletonAssetId(*skeleton);
        }
        return std::unique_ptr<LuxAsset>(std::move(model));
    }

    EAssetError ModelCodec::exportAsLuxAssetStream(
        const LuxAsset& asset,
        std::ofstream& output
    )
    {
        const auto* model = asset.as<ModelAsset>();
        if (!model)
        {
            return EAssetError::FILE_TYPE_ERROR;
        }

        std::vector<std::byte> info;
        info.reserve(
            64 + 16 * (
                model->meshAssetIds().size() +
                model->materialAssetIds().size() +
                model->animationClipAssetIds().size()
            )
        );
        appendPod<std::uint32_t>(info, kLuxModelFormatVersion);
        const bool has_skeleton = model->skeletonAssetId().has_value();
        appendPod<std::uint8_t>(
            info,
            has_skeleton ? std::uint8_t{1} : std::uint8_t{0}
        );
        appendUuid(
            info,
            has_skeleton ? *model->skeletonAssetId() : asset_id_t{}
        );
        appendUuidVector(info, model->meshAssetIds());
        appendUuidVector(info, model->materialAssetIds());
        appendUuidVector(info, {});
        appendUuidVector(info, model->animationClipAssetIds());

        const auto header = makeHeaderRaw<EAssetType::MODEL>(
            *model->info(),
            info.size(),
            0
        );
        output.write(
            reinterpret_cast<const char*>(header.data()),
            static_cast<std::streamsize>(header.size())
        );
        output.write(
            reinterpret_cast<const char*>(info.data()),
            static_cast<std::streamsize>(info.size())
        );
        return output.good()
            ? EAssetError::SUCCESS
            : EAssetError::WRITE_FILE_FAIL;
    }
} // namespace lux::asset
