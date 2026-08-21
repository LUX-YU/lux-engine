#include <lux/engine/authoring/assets/MaterialAuthoringCodec.hpp>

#include <lux/engine/authoring/assets/MaterialGraphCodec.hpp>
#include <lux/engine/authoring/assets/MaterialGraphDocument.hpp>
#include <lux/engine/resource/asset/material/MaterialAsset.hpp>
#include <lux/engine/resource/asset/material/MaterialSerDeser.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::authoring::detail
{
    namespace
    {
        constexpr std::uint32_t kLegacyMaterialVersion = 3u;
        constexpr std::uint32_t kCookedMaterialVersion = 4u;

        struct ImageSections final
        {
            std::size_t info_offset{};
            std::size_t info_size{};
            std::size_t payload_offset{};
            lux::asset::AssetInfo asset_info{};
        };

        template <class T>
        bool readAt(
            std::span<const std::byte> image,
            std::size_t                offset,
            T&                         out
        ) noexcept
        {
            static_assert(std::is_trivially_copyable_v<T>);
            if (offset > image.size() || sizeof(T) > image.size() - offset)
                return false;
            std::memcpy(&out, image.data() + offset, sizeof(T));
            return true;
        }

        lux::cxx::expected<ImageSections, lux::asset::EAssetError>
        inspectImage(std::span<const std::byte> image) noexcept
        {
            using namespace lux::asset;

            std::uint32_t magic{};
            asset_version_t version{};
            if (!readAt(image, 0u, magic) ||
                !readAt(image, sizeof(magic), version))
                return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);
            if (magic != asset_magic_number_of<EAssetType::MATERIAL>::value)
                return lux::cxx::unexpected(EAssetError::WRONG_FILE_HEADER);

            std::uint64_t info_offset{};
            std::uint64_t info_size{};
            std::uint64_t data_offset{};
            std::uint64_t data_size{};
            std::size_t   header_size{};
            AssetInfo     asset_info{};
            if (version == current_asset_version)
            {
                AssetFileHeader header{};
                if (!readAt(image, 0u, header))
                    return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);
                info_offset = header.info_offset;
                info_size   = header.info_size;
                data_offset = header.data_offset;
                data_size   = header.data_size;
                header_size = sizeof(header);
                asset_info  = header.info;
            }
            else if (version == asset_version_v1)
            {
                compat::AssetFileHeaderV1 header{};
                if (!readAt(image, 0u, header))
                    return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);
                info_offset = header.info_offset;
                info_size   = header.info_size;
                data_offset = header.data_offset;
                data_size   = header.data_size;
                header_size = sizeof(header);
                asset_info  = compat::upgradeAssetInfo(header.info);
            }
            else
            {
                return lux::cxx::unexpected(EAssetError::UNSUPPORTED_VERSION);
            }

            if (info_offset != header_size ||
                data_offset != info_offset + info_size ||
                info_offset > image.size() ||
                info_size > image.size() - static_cast<std::size_t>(info_offset) ||
                data_offset > image.size() ||
                data_size > image.size() - static_cast<std::size_t>(data_offset))
                return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);

            if (asset_info.type != EAssetType::MATERIAL)
                return lux::cxx::unexpected(
                    EAssetError::WRONG_FILE_HEADER);

            return ImageSections{
                static_cast<std::size_t>(info_offset),
                static_cast<std::size_t>(info_size),
                static_cast<std::size_t>(data_offset + data_size),
                asset_info
            };
        }

        struct Cursor final
        {
            std::span<const std::byte> bytes;
            std::size_t                offset{};

            template <class T>
            bool readPod(T& out) noexcept
            {
                if (!readAt(bytes, offset, out))
                    return false;
                offset += sizeof(T);
                return true;
            }

            bool readUuid(lux::asset::asset_id_t& out) noexcept
            {
                if (offset > bytes.size() || 16u > bytes.size() - offset)
                    return false;
                std::array<std::uint8_t, 16> raw{};
                std::memcpy(raw.data(), bytes.data() + offset, raw.size());
                offset += raw.size();
                out = uuids::uuid(raw);
                return true;
            }

            bool readBytes(std::vector<std::byte>& out) noexcept
            {
                std::uint32_t size{};
                if (!readPod(size) ||
                    offset > bytes.size() || size > bytes.size() - offset)
                    return false;
                out.assign(
                    bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                    bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)
                );
                offset += size;
                return true;
            }

            bool readWords(std::vector<std::uint32_t>& out) noexcept
            {
                std::uint32_t count{};
                if (!readPod(count))
                    return false;
                const std::size_t byte_count =
                    static_cast<std::size_t>(count) * sizeof(std::uint32_t);
                if (offset > bytes.size() || byte_count > bytes.size() - offset)
                    return false;
                out.resize(count);
                if (byte_count != 0u)
                    std::memcpy(out.data(), bytes.data() + offset, byte_count);
                offset += byte_count;
                return true;
            }
        };

        std::vector<lux::asset::Payload> readPayloads(
            std::span<const std::byte> image,
            std::size_t                offset
        )
        {
            std::vector<lux::asset::Payload> payloads;
            while (offset <= image.size() &&
                   sizeof(lux::asset::PayloadBlockHeader) <= image.size() - offset)
            {
                lux::asset::PayloadBlockHeader header{};
                if (!readAt(image, offset, header))
                    break;
                offset += sizeof(header);
                if (header.size > image.size() - offset)
                    break;
                std::vector<std::byte> bytes(
                    image.begin() + static_cast<std::ptrdiff_t>(offset),
                    image.begin() + static_cast<std::ptrdiff_t>(
                        offset + static_cast<std::size_t>(header.size))
                );
                payloads.push_back(lux::asset::Payload{
                    header.tag,
                    std::move(bytes)
                });
                offset += static_cast<std::size_t>(header.size);
            }
            return payloads;
        }

        std::unique_ptr<lux::asset::MaterialData> decodeLegacyFields(
            Cursor&                cursor,
            std::vector<std::byte>& graph_blob
        )
        {
            lux::rdesc::MaterialGraph graph;
            if (!cursor.readBytes(graph_blob) ||
                !decodeMaterialGraph(graph_blob, graph, nullptr))
                return nullptr;

            auto data = std::make_unique<lux::asset::MaterialData>();
            data->parameter_count = static_cast<std::uint32_t>(
                std::min<std::size_t>(
                    graph.param_slots.size(),
                    lux::asset::MaterialData::kMaxParams
                )
            );
            for (std::uint32_t index = 0u;
                 index < data->parameter_count;
                 ++index)
                std::copy_n(
                    graph.param_slots[index].dflt,
                    data->parameter_defaults[index].size(),
                    data->parameter_defaults[index].begin()
                );
            data->alpha_mode = static_cast<std::uint32_t>(
                graph.render_state.alpha_mode
            );
            data->double_sided = graph.render_state.double_sided;

            std::uint32_t texture_count{};
            if (!cursor.readPod(texture_count))
                return nullptr;
            for (std::uint32_t index = 0u; index < texture_count; ++index)
            {
                std::uint32_t slot{};
                lux::asset::asset_id_t id{};
                if (!cursor.readPod(slot) || !cursor.readUuid(id))
                    return nullptr;
                if (slot < lux::asset::MaterialData::kMaxTextures)
                    data->texture_slot_ids[slot] = id;
            }

            if (!cursor.readWords(data->gbuffer_spirv))
                return nullptr;
            {
                std::vector<std::byte> bytes;
                std::string error;
                if (!cursor.readBytes(bytes) ||
                    (!bytes.empty() &&
                     !lux::rdesc::ShaderInfo::deserialize(
                         bytes,
                         data->gbuffer_info,
                         &error)))
                    return nullptr;
            }
            if (!cursor.readWords(data->forward_spirv))
                return nullptr;
            {
                std::vector<std::byte> bytes;
                std::string error;
                if (!cursor.readBytes(bytes) ||
                    (!bytes.empty() &&
                     !lux::rdesc::ShaderInfo::deserialize(
                         bytes,
                         data->forward_info,
                         &error)))
                    return nullptr;
            }
            return data;
        }
    } // namespace

    namespace
    {
        lux::cxx::expected<
            std::unique_ptr<lux::asset::LuxAsset>,
            lux::asset::EAssetError>
        decodeAuthoringMaterialAsset(
            std::span<const std::byte> bytes) noexcept
        {
            using namespace lux::asset;
            auto inspected = inspectImage(bytes);
            if (!inspected)
                return lux::cxx::unexpected(inspected.error());
            const ImageSections sections = *inspected;

            std::uint32_t material_version{};
            if (!readAt(bytes, sections.info_offset, material_version))
                return lux::cxx::unexpected(
                    EAssetError::ASSET_DESERIALIZE_FAIL);

            std::unique_ptr<MaterialData> data;
            std::vector<Payload> payloads = readPayloads(
                bytes,
                sections.payload_offset
            );
            if (material_version == kCookedMaterialVersion)
            {
                auto decoded = MaterialSerDeser::decodeData(
                    bytes.data(),
                    bytes.size()
                );
                if (!decoded)
                    return lux::cxx::unexpected(decoded.error());
                data = std::move(*decoded);
            }
            else if (material_version == kLegacyMaterialVersion)
            {
                Cursor cursor{
                    bytes.subspan(
                        sections.info_offset,
                        sections.info_size),
                    sizeof(material_version)
                };
                std::vector<std::byte> graph_blob;
                data = decodeLegacyFields(cursor, graph_blob);
                if (!data || cursor.offset != cursor.bytes.size())
                    return lux::cxx::unexpected(
                        EAssetError::ASSET_DESERIALIZE_FAIL
                    );

                std::erase_if(
                    payloads,
                    [](const Payload& payload)
                    {
                        return payload.tag == kMaterialGraphPayloadTag;
                    }
                );
                payloads.push_back(Payload{
                    kMaterialGraphPayloadTag,
                    std::move(graph_blob)
                });
            }
            else
            {
                return lux::cxx::unexpected(
                    EAssetError::ASSET_DESERIALIZE_FAIL
                );
            }

            auto asset = std::make_unique<MaterialAsset>(
                std::make_unique<AssetInfo>(sections.asset_info)
            );
            asset->setData(std::move(data));
            asset->setPayloads(std::move(payloads));
            return std::unique_ptr<LuxAsset>{std::move(asset)};
        }

        class AuthoringMaterialSerDeser final
            : public lux::asset::MaterialSerDeser
        {
        public:
            using lux::asset::MaterialSerDeser::MaterialSerDeser;

        protected:
            lux::cxx::expected<
                std::unique_ptr<lux::asset::LuxAsset>,
                lux::asset::EAssetError>
            fromLuxAssetStream(std::istream& stream) override
            {
                stream.seekg(0, std::ios::end);
                const auto end = stream.tellg();
                if (end <= 0)
                    return lux::cxx::unexpected(
                        lux::asset::EAssetError::ABNORMAL_FILE_SIZE);
                stream.seekg(0, std::ios::beg);
                std::vector<std::byte> bytes(
                    static_cast<std::size_t>(end)
                );
                if (!stream.read(
                        reinterpret_cast<char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size())))
                {
                    return lux::cxx::unexpected(
                        lux::asset::EAssetError::READ_FILE_FAIL);
                }
                return decodeAuthoringMaterialAsset(bytes);
            }
        };
    } // namespace

    std::unique_ptr<lux::asset::AssetSerDeser>
    createAuthoringMaterialSerDeser(
        lux::asset::EAssetType type,
        std::shared_ptr<lux::asset::AssetManager> manager)
    {
        if (type != lux::asset::EAssetType::MATERIAL)
            return nullptr;
        return std::make_unique<AuthoringMaterialSerDeser>(
            std::move(manager)
        );
    }
} // namespace lux::authoring::detail
