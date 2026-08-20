#include <lux/engine/resource/asset/codecs/MaterialSerDeser.hpp>
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

    MaterialSerDeser::MaterialSerDeser(std::shared_ptr<AssetManager> manager)
        : TAssetSerDeser(std::move(manager))
    {
    }

    MaterialSerDeser::~MaterialSerDeser() = default;

    // ─────────────────────────────────────────────────────────────────────────
    //  .luxasset cooked Material format v4
    //
    //  The runtime image contains baked values only. The editable node graph is an
    //  authoring-owned tagged payload appended by the generic AssetSerDeser path;
    //  this codec never parses or links it. Layout (LE POD copies):
    //
    //    u32 version (= kFormatVersion)
    //    u32 parameter_count;    float[4] × parameter_count
    //    u32 alpha_mode;         u8 double_sided
    //    u32 tex_count; { u32 slot; u8[16] uuid } × tex_count   (bound slots only)
    //    u32 gbuffer_spv_words;  u32  × gbuffer_spv_words
    //    u32 gbuffer_info_bytes; byte × gbuffer_info_bytes       (ShaderInfo::serialize)
    //    u32 forward_spv_words;  u32  × forward_spv_words
    //    u32 forward_info_bytes; byte × forward_info_bytes
    //
    //  v3 mixed the authored graph into the runtime payload. It is intentionally
    //  rejected: project assets are re-baked by the Toolchain into v4.
    // ─────────────────────────────────────────────────────────────────────────
    namespace
    {
        constexpr std::uint32_t kFormatVersion = 4;

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

            // Read a length-prefixed (u32 count) array of u32 words.
            bool readU32Vector(std::vector<std::uint32_t>& out) noexcept
            {
                std::uint32_t count{};
                if (!readPod(count)) return false;
                if (short_of(static_cast<std::size_t>(count) * sizeof(std::uint32_t)))
                    return false;
                out.resize(count);
                if (count)
                {
                    std::memcpy(out.data(), p, static_cast<std::size_t>(count) * sizeof(std::uint32_t));
                    p += static_cast<std::size_t>(count) * sizeof(std::uint32_t);
                }
                return true;
            }

            // Read a length-prefixed (u32 len) raw byte block.
            bool readByteVector(std::vector<std::byte>& out) noexcept
            {
                std::uint32_t len{};
                if (!readPod(len)) return false;
                if (short_of(len)) return false;
                out.resize(len);
                if (len)
                {
                    std::memcpy(out.data(), p, len);
                    p += len;
                }
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

        // Decode the MaterialData fields from a cursor positioned just AFTER the
        // u32 version word (which the caller has already read + checked). This is
        // the single field-decode SSOT shared by decodeData() and
        // fromLuxAssetStream() so the two readers can never drift. Pure: touches
        // only the cursor; on any short/invalid read returns nullptr.
        std::unique_ptr<MaterialData> decodeMaterialFields(Cursor& c)
        {
            auto payload = std::make_unique<MaterialData>();

            if (!c.readPod(payload->parameter_count)
                || payload->parameter_count > MaterialData::kMaxParams)
                return nullptr;
            for (std::uint32_t i = 0; i < payload->parameter_count; ++i)
                for (float& value : payload->parameter_defaults[i])
                    if (!c.readPod(value)) return nullptr;

            std::uint8_t double_sided{};
            if (!c.readPod(payload->alpha_mode)
                || !c.readPod(double_sided)
                || double_sided > 1u)
                return nullptr;
            payload->double_sided = double_sided != 0u;

            std::uint32_t tex_count{};
            if (!c.readPod(tex_count)) return nullptr;
            for (std::uint32_t i = 0; i < tex_count; ++i)
            {
                std::uint32_t slot{};
                asset_id_t    id{};
                if (!c.readPod(slot) || !c.readUuid(id))
                    return nullptr;
                if (slot < MaterialData::kMaxTextures)
                    payload->texture_slot_ids[slot] = id;
            }

            // gbuffer SPIR-V + ShaderInfo
            if (!c.readU32Vector(payload->gbuffer_spirv)) return nullptr;
            {
                std::vector<std::byte> info_bytes;
                std::string err;
                if (!c.readByteVector(info_bytes)) return nullptr;
                if (!info_bytes.empty() &&
                    !lux::rdesc::ShaderInfo::deserialize(
                        std::span<const std::byte>(info_bytes),
                        payload->gbuffer_info,
                        &err))
                    return nullptr;
            }

            // forward SPIR-V + ShaderInfo
            if (!c.readU32Vector(payload->forward_spirv)) return nullptr;
            {
                std::vector<std::byte> info_bytes;
                std::string err;
                if (!c.readByteVector(info_bytes)) return nullptr;
                if (!info_bytes.empty() &&
                    !lux::rdesc::ShaderInfo::deserialize(
                        std::span<const std::byte>(info_bytes),
                        payload->forward_info,
                        &err))
                    return nullptr;
            }

            return payload;
        }
    } // namespace

    // ─────────────────────────────────────────────────────────────────────────
    //  Pure-data decode (no AssetManager / no registration)
    // ─────────────────────────────────────────────────────────────────────────

    lux::cxx::expected<std::unique_ptr<MaterialData>, EAssetError>
    MaterialSerDeser::decodeData(const void* bytes, std::size_t len) noexcept
    {
        if (bytes == nullptr) return unexpected(EAssetError::ABNORMAL_FILE_SIZE);
        const auto file = std::span<const std::byte>{
            static_cast<const std::byte*>(bytes),
            len
        };

            AssetFileHeader header{};
            if (auto ec = loadHeaderRaw<EAssetType::MATERIAL>(file, header);
                ec != EAssetError::SUCCESS)
            {
                return unexpected(ec);
            }
            if (header.magic_number != asset_magic_number_of<EAssetType::MATERIAL>::value)
                return unexpected(EAssetError::WRONG_FILE_HEADER);
            if (header.info_offset != sizeof(AssetFileHeader))
                return unexpected(EAssetError::WRONG_FILE_HEADER);
            // Wrap-safe bounds check: info_offset / info_size come straight from
            // untrusted file bytes (loadHeaderRaw only verifies the header FITS),
            // so a corrupt header could otherwise overflow info_offset + info_size
            // and let the Cursor read out of bounds.
            if (header.info_offset > file.size()
                || header.info_size > file.size() - header.info_offset)
                return unexpected(EAssetError::ABNORMAL_FILE_SIZE);

            // Material packs its whole payload into the info section (data_size
            // == 0); the "data" of the asset is the MaterialData decoded here.
            Cursor c{
                file.data() + header.info_offset,
                file.data() + header.info_offset + header.info_size,
            };

            std::uint32_t version{};
            if (!c.readPod(version) || version != kFormatVersion)
                return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);

            auto payload = decodeMaterialFields(c);
            if (!payload)
                return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
        return payload;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  AssetSerDeser overrides
    // ─────────────────────────────────────────────────────────────────────────

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    MaterialSerDeser::fromFileStream(std::ifstream&)
    {
        // No external source format — materials are authored in the editor.
        return unexpected(EAssetError::UNSUPPORTED);
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    MaterialSerDeser::fromLuxAssetStream(std::istream& ifs)
    {
        std::vector<std::byte> file;
        if (auto ec = readAllStream(ifs, file); ec != EAssetError::SUCCESS)
            return unexpected(ec);

        // The AssetInfo wrapper comes from the header; the pure MaterialData
        // payload comes from decodeData (the single field-decode SSOT — see
        // decodeMaterialFields). We re-read the header here only to lift
        // header.info; the field decode lives in exactly one place.
        AssetFileHeader header{};
        if (auto ec = loadHeaderRaw<EAssetType::MATERIAL>(file, header);
            ec != EAssetError::SUCCESS)
        {
            return unexpected(ec);
        }

        auto data = decodeData(file.data(), file.size());
        if (!data.has_value())
            return unexpected(data.error());

        auto ainfo = std::make_unique<AssetInfo>(header.info);
        auto asset = std::make_unique<MaterialAsset>(std::move(ainfo));
        asset->setData(std::move(data.value()));
        return std::unique_ptr<LuxAsset>(std::move(asset));
    }

    EAssetError
    MaterialSerDeser::exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& ofile)
    {
        const auto* ma = asset.as<MaterialAsset>();
        if (!ma) return EAssetError::FILE_TYPE_ERROR;
        const MaterialData* d = ma->data();
        if (!d) return EAssetError::ASSET_NO_DATA;

        const std::vector<std::byte> gb_info =
            lux::rdesc::ShaderInfo::serialize(d->gbuffer_info);
        const std::vector<std::byte> fwd_info =
            lux::rdesc::ShaderInfo::serialize(d->forward_info);

        std::vector<std::byte> info;
        info.reserve(64 + d->parameter_count * sizeof(d->parameter_defaults[0])
                     + d->gbuffer_spirv.size() * 4 +
                     d->forward_spirv.size() * 4 + gb_info.size() + fwd_info.size());

        appendPod<std::uint32_t>(info, kFormatVersion);

        if (d->parameter_count > MaterialData::kMaxParams)
            return EAssetError::ASSET_DESERIALIZE_FAIL;
        appendPod<std::uint32_t>(info, d->parameter_count);
        for (std::uint32_t i = 0; i < d->parameter_count; ++i)
            for (float value : d->parameter_defaults[i])
                appendPod(info, value);
        appendPod<std::uint32_t>(info, d->alpha_mode);
        appendPod<std::uint8_t>(info, d->double_sided ? 1u : 0u);

        // Bound texture slots (skip nil).
        std::uint32_t tex_count = 0;
        for (const auto& id : d->texture_slot_ids)
            if (!id.is_nil()) ++tex_count;
        appendPod<std::uint32_t>(info, tex_count);
        for (std::uint32_t i = 0; i < MaterialData::kMaxTextures; ++i)
        {
            if (d->texture_slot_ids[i].is_nil()) continue;
            appendPod<std::uint32_t>(info, i);
            appendUuid(info, d->texture_slot_ids[i]);
        }

        // gbuffer SPIR-V + ShaderInfo
        appendPod<std::uint32_t>(info, static_cast<std::uint32_t>(d->gbuffer_spirv.size()));
        appendBytes(info, d->gbuffer_spirv.data(), d->gbuffer_spirv.size() * sizeof(std::uint32_t));
        appendPod<std::uint32_t>(info, static_cast<std::uint32_t>(gb_info.size()));
        appendBytes(info, gb_info.data(), gb_info.size());

        // forward SPIR-V + ShaderInfo
        appendPod<std::uint32_t>(info, static_cast<std::uint32_t>(d->forward_spirv.size()));
        appendBytes(info, d->forward_spirv.data(), d->forward_spirv.size() * sizeof(std::uint32_t));
        appendPod<std::uint32_t>(info, static_cast<std::uint32_t>(fwd_info.size()));
        appendBytes(info, fwd_info.data(), fwd_info.size());

        const auto header_bytes = makeHeaderRaw<EAssetType::MATERIAL>(
            *ma->info(), info.size(), /*data_size*/ 0);

        ofile.write(reinterpret_cast<const char*>(header_bytes.data()),
                    static_cast<std::streamsize>(header_bytes.size()));
        if (!info.empty())
            ofile.write(reinterpret_cast<const char*>(info.data()),
                        static_cast<std::streamsize>(info.size()));
        return ofile.good() ? EAssetError::SUCCESS : EAssetError::WRITE_FILE_FAIL;
    }

} // namespace lux::asset
