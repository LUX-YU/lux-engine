#include <lux/engine/resource/asset/material/MaterialInstanceSerDeser.hpp>
#include <lux/engine/resource/asset/detail/AssetManagerImpl.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <span>
#include <type_traits>
#include <vector>

namespace lux::asset
{
    using lux::cxx::unexpected;

    MaterialInstanceSerDeser::MaterialInstanceSerDeser(std::shared_ptr<AssetManager> manager)
        : TAssetSerDeser(std::move(manager))
    {
    }

    MaterialInstanceSerDeser::~MaterialInstanceSerDeser() = default;

    // ─────────────────────────────────────────────────────────────────────────
    //  .luxasset MaterialInstance format v1
    //
    //  Info-section-only (data_size == 0), little-endian POD copies. Layout:
    //    u32  version (= kFormatVersion)
    //    u8[16] parent_material_id
    //    u32  param_override_mask
    //    { f32[4] }  for each set bit i (ascending) — self-describing via the mask
    //    u32  tex_override_mask
    //    { u8[16] uuid } for each set bit i (ascending)
    //    u32  render_state_override
    //    u32  alpha_mode
    //    u8   double_sided
    //
    //  No SPIR-V (the whole point — an instance shares the parent's shader/PSO).
    // ─────────────────────────────────────────────────────────────────────────
    namespace
    {
        // LENIENT version check (reject only version > kFormatVersion) — mirrors
        // MaterialSerDeser; a strict bump once broke a shipping project (err=19).
        constexpr std::uint32_t kFormatVersion = 1;

        template <class T>
        void appendPod(std::vector<std::byte>& buf, const T& v)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            const auto off = buf.size();
            buf.resize(off + sizeof(T));
            std::memcpy(buf.data() + off, &v, sizeof(T));
        }

        void appendBytes(std::vector<std::byte>& buf, const void* p, std::size_t n)
        {
            if (n == 0) return;
            const auto off = buf.size();
            buf.resize(off + n);
            std::memcpy(buf.data() + off, p, n);
        }

        void appendUuid(std::vector<std::byte>& buf, const asset_id_t& id)
        {
            const auto bytes = id.as_bytes();
            appendBytes(buf, bytes.data(), bytes.size());
        }

        struct Cursor
        {
            const std::byte* p;
            const std::byte* end;

            bool short_of(std::size_t n) const noexcept
            {
                return static_cast<std::size_t>(end - p) < n;
            }
            template <class T>
            bool readPod(T& out) noexcept
            {
                if (short_of(sizeof(T))) return false;
                std::memcpy(&out, p, sizeof(T));
                p += sizeof(T);
                return true;
            }
            bool readUuid(asset_id_t& out) noexcept
            {
                if (short_of(16)) return false;
                std::array<std::uint8_t, 16> bytes{};
                std::memcpy(bytes.data(), p, 16);
                p += 16;
                out = uuids::uuid(bytes);
                return true;
            }
        };

        EAssetError readAllStream(std::istream& ifs, std::vector<std::byte>& out)
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
    } // namespace

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    MaterialInstanceSerDeser::fromFileStream(std::ifstream&)
    {
        // No external source format — instances are authored in the editor.
        return unexpected(EAssetError::UNSUPPORTED);
    }

    lux::cxx::expected<std::unique_ptr<MaterialInstanceData>, EAssetError>
    MaterialInstanceSerDeser::decodeData(const void* bytes, std::size_t len) noexcept
    {
        if (bytes == nullptr || len == 0)
            return unexpected(EAssetError::ABNORMAL_FILE_SIZE);

        const auto file = std::span<const std::byte>{
            static_cast<const std::byte*>(bytes), len};

        AssetFileHeader header{};
        if (auto ec = loadHeaderRaw<EAssetType::MATERIAL_INSTANCE>(file, header);
            ec != EAssetError::SUCCESS)
            return unexpected(ec);
        if (header.magic_number != asset_magic_number_of<EAssetType::MATERIAL_INSTANCE>::value)
            return unexpected(EAssetError::WRONG_FILE_HEADER);
        // Validate the header's section bounds BEFORE forming the Cursor — info_offset
        // / info_size come straight from untrusted file bytes (loadHeaderRaw only checks
        // the header FITS), so a corrupt/truncated .luxasset would otherwise let the
        // Cursor read out of bounds. Wrap-safe form (no info_offset + info_size add).
        if (header.info_offset != assetFileHeaderSize(header.version))
            return unexpected(EAssetError::WRONG_FILE_HEADER);
        if (header.info_offset > file.size()
            || header.info_size > file.size() - header.info_offset)
            return unexpected(EAssetError::ABNORMAL_FILE_SIZE);

        Cursor c{
            file.data() + header.info_offset,
            file.data() + header.info_offset + header.info_size,
        };

        std::uint32_t version{};
        if (!c.readPod(version) || version > kFormatVersion)  // lenient
            return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);

        auto payload = std::make_unique<MaterialInstanceData>();

        if (!c.readUuid(payload->parent_material_id)) return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);

        if (!c.readPod(payload->param_override_mask)) return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
        for (std::uint32_t i = 0; i < MaterialInstanceData::kMaxParams; ++i)
            if (payload->param_override_mask & (1u << i))
                for (int k = 0; k < 4; ++k)
                    if (!c.readPod(payload->params[i][k])) return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);

        if (!c.readPod(payload->tex_override_mask)) return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
        for (std::uint32_t i = 0; i < MaterialInstanceData::kMaxTextures; ++i)
            if (payload->tex_override_mask & (1u << i))
                if (!c.readUuid(payload->texture_slot_ids[i])) return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);

        std::uint32_t ds = 0;
        if (!c.readPod(payload->render_state_override)) return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
        if (!c.readPod(payload->alpha_mode))            return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
        if (!c.readPod(ds))                             return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
        payload->double_sided = (ds != 0u);

        return payload;
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    MaterialInstanceSerDeser::fromLuxAssetStream(std::istream& ifs)
    {
        std::vector<std::byte> file;
        if (auto ec = readAllStream(ifs, file); ec != EAssetError::SUCCESS)
            return unexpected(ec);

        // Reuse the pure decoder for the payload (DRY) — it re-validates the header
        // and decodes MaterialInstanceData from the info section. We additionally need
        // the header's AssetInfo to assemble the full LuxAsset shell, so read the
        // (fixed-size, already-validated-by-decodeData) header here too; loadHeaderRaw
        // is a cheap memcpy and also migrates v1 → current AssetInfo.
        auto data = decodeData(file.data(), file.size());
        if (!data.has_value())
            return unexpected(data.error());

        AssetFileHeader header{};
        if (auto ec = loadHeaderRaw<EAssetType::MATERIAL_INSTANCE>(file, header);
            ec != EAssetError::SUCCESS)
            return unexpected(ec);

        auto ainfo = std::make_unique<AssetInfo>(header.info);
        auto asset = std::make_unique<MaterialInstanceAsset>(std::move(ainfo));
        asset->setData(std::move(data.value()));
        return std::unique_ptr<LuxAsset>(std::move(asset));
    }

    EAssetError
    MaterialInstanceSerDeser::exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& ofile)
    {
        const auto* mi = asset.as<MaterialInstanceAsset>();
        if (!mi) return EAssetError::FILE_TYPE_ERROR;
        const MaterialInstanceData* d = mi->data();
        if (!d) return EAssetError::ASSET_NO_DATA;

        std::vector<std::byte> info;
        appendPod<std::uint32_t>(info, kFormatVersion);
        appendUuid(info, d->parent_material_id);

        appendPod<std::uint32_t>(info, d->param_override_mask);
        for (std::uint32_t i = 0; i < MaterialInstanceData::kMaxParams; ++i)
            if (d->param_override_mask & (1u << i))
                for (int k = 0; k < 4; ++k)
                    appendPod<float>(info, d->params[i][k]);

        appendPod<std::uint32_t>(info, d->tex_override_mask);
        for (std::uint32_t i = 0; i < MaterialInstanceData::kMaxTextures; ++i)
            if (d->tex_override_mask & (1u << i))
                appendUuid(info, d->texture_slot_ids[i]);

        appendPod<std::uint32_t>(info, d->render_state_override);
        appendPod<std::uint32_t>(info, d->alpha_mode);
        appendPod<std::uint32_t>(info, d->double_sided ? 1u : 0u);

        const auto header_bytes = makeHeaderRaw<EAssetType::MATERIAL_INSTANCE>(
            *mi->info(), info.size(), /*data_size*/ 0);

        ofile.write(reinterpret_cast<const char*>(header_bytes.data()),
                    static_cast<std::streamsize>(header_bytes.size()));
        if (!info.empty())
            ofile.write(reinterpret_cast<const char*>(info.data()),
                        static_cast<std::streamsize>(info.size()));
        return ofile.good() ? EAssetError::SUCCESS : EAssetError::WRITE_FILE_FAIL;
    }

} // namespace lux::asset
