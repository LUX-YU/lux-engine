#include <lux/engine/resource/asset/AssetCodecCatalog.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

namespace lux::asset
{
    lux::cxx::expected<AssetCodecCatalog, EAssetCodecCatalogError>
    AssetCodecCatalog::build(
        std::vector<AssetCodecDescriptor> descriptors) noexcept
    {
        for (const auto& descriptor : descriptors)
        {
            if (descriptor.cpp_type_name.empty())
            {
                return lux::cxx::unexpected(
                    EAssetCodecCatalogError::EMPTY_NAME);
            }
            if (descriptor.cpp_type_hash == 0u)
            {
                return lux::cxx::unexpected(
                    EAssetCodecCatalogError::INVALID_TYPE_IDENTITY);
            }
            if (descriptor.create == nullptr)
            {
                return lux::cxx::unexpected(
                    EAssetCodecCatalogError::MISSING_FACTORY);
            }
            if (descriptor.primary_magic == 0u)
            {
                return lux::cxx::unexpected(
                    EAssetCodecCatalogError::MISSING_PRIMARY_MAGIC);
            }
            if (descriptor.legacy_magic == descriptor.primary_magic)
            {
                return lux::cxx::unexpected(
                    EAssetCodecCatalogError::DUPLICATE_MAGIC);
            }
        }

        std::sort(
            descriptors.begin(),
            descriptors.end(),
            [](const AssetCodecDescriptor& lhs,
               const AssetCodecDescriptor& rhs) noexcept
            {
                return static_cast<std::uint32_t>(lhs.type) <
                       static_cast<std::uint32_t>(rhs.type);
            }
        );

        for (std::size_t index = 0u; index < descriptors.size(); ++index)
        {
            if (index != 0u &&
                descriptors[index - 1u].type == descriptors[index].type)
            {
                return lux::cxx::unexpected(
                    EAssetCodecCatalogError::DUPLICATE_ASSET_TYPE);
            }
            for (std::size_t other = 0u; other < index; ++other)
            {
                const auto magicConflicts = [](
                    const AssetCodecDescriptor& lhs,
                    const AssetCodecDescriptor& rhs) noexcept
                {
                    return lhs.primary_magic == rhs.primary_magic ||
                        (lhs.legacy_magic != 0u &&
                            (lhs.legacy_magic == rhs.primary_magic ||
                             lhs.legacy_magic == rhs.legacy_magic)) ||
                        (rhs.legacy_magic != 0u &&
                            rhs.legacy_magic == lhs.primary_magic);
                };
                if (magicConflicts(descriptors[other], descriptors[index]))
                {
                    return lux::cxx::unexpected(
                        EAssetCodecCatalogError::DUPLICATE_MAGIC);
                }
                if (descriptors[other].cpp_type_name ==
                    descriptors[index].cpp_type_name)
                {
                    return lux::cxx::unexpected(
                        EAssetCodecCatalogError::DUPLICATE_CPP_TYPE);
                }
                if (descriptors[other].cpp_type_hash !=
                    descriptors[index].cpp_type_hash)
                {
                    continue;
                }
                return lux::cxx::unexpected(
                    EAssetCodecCatalogError::TYPE_HASH_COLLISION);
            }
        }
        return AssetCodecCatalog{std::move(descriptors)};
    }

    const AssetCodecDescriptor* AssetCodecCatalog::findByMagic(
        std::uint32_t magic) const noexcept
    {
        if (magic == 0u)
            return nullptr;
        const auto found = std::find_if(
            descriptors_.begin(),
            descriptors_.end(),
            [magic](const AssetCodecDescriptor& descriptor) noexcept
            {
                return descriptor.primary_magic == magic ||
                    descriptor.legacy_magic == magic;
            }
        );
        return found != descriptors_.end() ? std::addressof(*found) : nullptr;
    }

    const AssetCodecDescriptor* AssetCodecCatalog::find(
        EAssetType type) const noexcept
    {
        const auto found = std::lower_bound(
            descriptors_.begin(),
            descriptors_.end(),
            type,
            [](const AssetCodecDescriptor& descriptor,
               EAssetType expected) noexcept
            {
                return static_cast<std::uint32_t>(descriptor.type) <
                       static_cast<std::uint32_t>(expected);
            }
        );
        return found != descriptors_.end() && found->type == type
            ? std::addressof(*found)
            : nullptr;
    }

    std::unique_ptr<AssetSerDeser> AssetCodecCatalog::create(
        EAssetType type,
        std::shared_ptr<AssetManager> owner) const
    {
        const auto* descriptor = find(type);
        return descriptor != nullptr
            ? descriptor->create(type, std::move(owner))
            : nullptr;
    }

    lux::cxx::expected<AssetDataInjector, EAssetError>
    AssetCodecCatalog::decode(lux::cxx::SharedBytes<> image) const noexcept
    {
        if (image.size() < sizeof(std::uint32_t))
            return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);

        std::uint32_t magic{};
        std::memcpy(&magic, image.data(), sizeof(magic));
        const auto* descriptor = findByMagic(magic);
        if (descriptor == nullptr || descriptor->decode == nullptr)
            return lux::cxx::unexpected(EAssetError::UNSUPPORTED);
        return descriptor->decode(std::move(image));
    }

    std::unique_ptr<LuxAsset> AssetCodecCatalog::createShell(
        std::unique_ptr<AssetInfo> info) const noexcept
    {
        if (!info)
            return nullptr;
        const auto* descriptor = find(info->type);
        return descriptor != nullptr && descriptor->create_shell != nullptr
            ? descriptor->create_shell(std::move(info))
            : nullptr;
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    makeShellFromMemory(
        const AssetCodecCatalog& catalog,
        const void* bytes,
        std::size_t len)
    {
        constexpr std::size_t kPrefix =
            sizeof(std::uint32_t) + sizeof(asset_version_t);
        if (bytes == nullptr || len < sizeof(std::uint32_t))
            return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);

        const auto* data = static_cast<const std::byte*>(bytes);
        std::uint32_t magic{};
        std::memcpy(&magic, data, sizeof(magic));
        const auto* descriptor = catalog.findByMagic(magic);
        if (descriptor == nullptr)
            return lux::cxx::unexpected(EAssetError::UNSUPPORTED);
        if (magic == descriptor->legacy_magic)
        {
            if (descriptor->create_shell_from_image == nullptr)
                return lux::cxx::unexpected(EAssetError::UNSUPPORTED);
            return descriptor->create_shell_from_image(
                std::span<const std::byte>{data, len});
        }
        if (len < kPrefix)
            return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);

        asset_version_t version{};
        std::memcpy(
            &version,
            data + sizeof(std::uint32_t),
            sizeof(version)
        );

        AssetInfo info{};
        if (version == current_asset_version)
        {
            if (len < sizeof(AssetFileHeader))
                return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);
            AssetFileHeader header{};
            std::memcpy(&header, data, sizeof(header));
            info = header.info;
        }
        else if (version == asset_version_v1)
        {
            if (len < sizeof(compat::AssetFileHeaderV1))
                return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);
            compat::AssetFileHeaderV1 header{};
            std::memcpy(&header, data, sizeof(header));
            info = compat::upgradeAssetInfo(header.info);
        }
        else
        {
            return lux::cxx::unexpected(EAssetError::UNSUPPORTED_VERSION);
        }

        if (info.type != descriptor->type)
            return lux::cxx::unexpected(EAssetError::WRONG_FILE_HEADER);
        auto shell = catalog.createShell(std::make_unique<AssetInfo>(info));
        if (!shell)
            return lux::cxx::unexpected(EAssetError::UNSUPPORTED);
        return shell;
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    makeShellFromFile(
        const AssetCodecCatalog& catalog,
        const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return lux::cxx::unexpected(EAssetError::FILE_OPEN_FAIL);

        stream.seekg(0, std::ios::end);
        const auto end = stream.tellg();
        if (end <= 0)
            return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);
        stream.seekg(0, std::ios::beg);
        std::vector<std::byte> buffer(static_cast<std::size_t>(end));
        stream.read(
            reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
        if (stream.gcount() != static_cast<std::streamsize>(buffer.size()))
            return lux::cxx::unexpected(EAssetError::READ_FILE_FAIL);
        return makeShellFromMemory(
            catalog,
            buffer.data(),
            buffer.size()
        );
    }
} // namespace lux::asset
