#include <lux/engine/resource/asset/script/ScriptSerDeser.hpp>
#include <lux/engine/resource/asset/script/ScriptDescriptionCodec.hpp>
#include <lux/engine/resource/asset/detail/AssetManagerImpl.hpp>

#include <fstream>
#include <iterator>
#include <new>
#include <span>
#include <string>
#include <vector>

namespace lux::asset
{
    using lux::cxx::unexpected;

    ScriptSerDeser::ScriptSerDeser(std::shared_ptr<AssetManager> manager)
        : TAssetSerDeser<ScriptLoadConfig>(std::move(manager))
    {
    }

    namespace
    {
        // Read an entire stream into a byte vector. Used for both the
        // .luxasset (header + metadata + payload) and the loose-dll import
        // flow. Streaming is unnecessary at this scale — manifest+dll for a
        // single Blueprint sits comfortably in memory.
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

        // Validate a SCRIPT .luxasset header against the file bytes and
        // populate `header`. Shared by decodeData() and fromLuxAssetStream() so
        // the section-offset contract lives in exactly one place. Pure: touches
        // only `file`. The data section may be empty (no payload).
        EAssetError validateScriptHeader(std::span<const std::byte> file,
                                         AssetFileHeader& header)
        {
            if (auto ec = AssetSerDeser::loadHeaderRaw<EAssetType::SCRIPT>(file, header);
                ec != EAssetError::SUCCESS)
            {
                return ec;
            }
            if (header.magic_number != asset_magic_number_of<EAssetType::SCRIPT>::value)
                return EAssetError::WRONG_FILE_HEADER;
            if (header.info_offset != sizeof(AssetFileHeader))
                return EAssetError::WRONG_FILE_HEADER;
            if (header.data_offset != header.info_offset + header.info_size)
                return EAssetError::WRONG_FILE_HEADER;
            if (file.size() < header.data_offset + header.data_size)
                return EAssetError::ABNORMAL_FILE_SIZE;
            return EAssetError::SUCCESS;
        }
    }

    lux::cxx::expected<AssetIDPair, EAssetError>
    ScriptSerDeser::importFromFile(const std::filesystem::path& p)
    {
        if (!std::filesystem::exists(p))
            return unexpected(EAssetError::FILE_NOT_EXIST);

        const auto ext = p.extension().string();
        const bool is_loose_lib = (ext == ".dll" || ext == ".so" || ext == ".dylib");

        // ── .lua source import: the file IS the payload; the description is
        // synthesized (kind = LuaSource, module = the stem).
        if (ext == ".lua")
        {
            std::ifstream ifs(p, std::ios::binary);
            if (!ifs) return unexpected(EAssetError::FILE_OPEN_FAIL);

            std::vector<std::byte> src_bytes;
            if (auto ec = readAll(ifs, src_bytes); ec != EAssetError::SUCCESS)
                return unexpected(ec);

            auto script = std::make_unique<lux::rdesc::Script>();
            script->module_name            = p.stem().string();
            script->body                   = lux::rdesc::LuaSourceScript{};
            script->provenance.compiler_id = "lua-source-import";
            script->provenance.source_id   = p.generic_string();

            auto ainfo = manager().createAssetInfo(
                EAssetType::SCRIPT, config().deterministic_seed);
            auto uptr  = std::make_unique<ScriptAsset>(
                std::move(ainfo), std::move(script), std::move(src_bytes));

            const auto& asset_id = uptr->id();
            auto* asset_ptr      = uptr.get();
            if (!submitAsset(std::move(uptr)))
                return unexpected(EAssetError::ASSET_ALREADY_EXIST);
            return AssetIDPair{ asset_ptr, asset_id };
        }

        if (is_loose_lib)
        {
            if (!config().accept_loose_library)
                return unexpected(EAssetError::FILE_TYPE_ERROR);

            // Loose-dll import: pair the binary with a sibling description
            // blob (same stem, ".luxscriptmeta"). Without the manifest we
            // cannot populate the Script description, and ScriptHost would
            // have nothing but an opaque blob.
            std::filesystem::path manifest_path = p;
            manifest_path.replace_extension(".luxscriptmeta");
            if (!std::filesystem::exists(manifest_path))
                return unexpected(EAssetError::ASSET_NO_INFO);

            std::ifstream lib_ifs(p, std::ios::binary);
            std::ifstream mf_ifs (manifest_path, std::ios::binary);
            if (!lib_ifs || !mf_ifs)
                return unexpected(EAssetError::FILE_OPEN_FAIL);

            std::vector<std::byte> lib_bytes;
            if (auto ec = readAll(lib_ifs, lib_bytes); ec != EAssetError::SUCCESS)
                return unexpected(ec);

            std::vector<std::byte> mf_bytes;
            if (auto ec = readAll(mf_ifs, mf_bytes); ec != EAssetError::SUCCESS)
                return unexpected(ec);

            auto script = std::make_unique<lux::rdesc::Script>();
            std::string err;
            if (!detail::decodeScriptDescription(
                    std::span<const std::byte>(mf_bytes), *script, &err))
            {
                return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
            }

            auto ainfo = manager().createAssetInfo(EAssetType::SCRIPT);
            auto uptr  = std::make_unique<ScriptAsset>(
                std::move(ainfo), std::move(script), std::move(lib_bytes));

            const auto& asset_id = uptr->id();
            auto* asset_ptr      = uptr.get();
            if (!submitAsset(std::move(uptr)))
                return unexpected(EAssetError::ASSET_ALREADY_EXIST);
            return AssetIDPair{ asset_ptr, asset_id };
        }

        // Anything else: assume it's already a .luxasset.
        std::ifstream ifs(p, std::ios::binary);
        if (!ifs) return unexpected(EAssetError::FILE_OPEN_FAIL);

        auto exp = this->fromLuxAssetStream(ifs);
        if (!exp) return unexpected(exp.error());

        const auto& asset_id = exp.value()->id();
        auto* asset_ptr      = exp.value().get();
        if (!submitAsset(std::move(exp.value())))
            return unexpected(EAssetError::ASSET_ALREADY_EXIST);
        return AssetIDPair{ asset_ptr, asset_id };
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    ScriptSerDeser::fromFileStream(std::ifstream& /*ifs*/)
    {
        // External-format import goes through importFromFile() above, which
        // routes by extension. The base-class entry point isn't meaningful
        // for scripts because we always need the sibling manifest.
        return unexpected(EAssetError::UNSUPPORTED);
    }

    lux::cxx::expected<std::unique_ptr<ScriptSerDeser::DATA>, EAssetError>
    ScriptSerDeser::decodeData(const void* bytes, std::size_t len) noexcept
    {
        if (bytes == nullptr && len != 0)
            return unexpected(EAssetError::READ_FILE_FAIL);

            // Zero-copy view (C8): the decode path only READS — no whole-image copy.
            const std::span<const std::byte> file(
                static_cast<const std::byte*>(bytes), len);

            AssetFileHeader header{};
            if (auto ec = validateScriptHeader(file, header); ec != EAssetError::SUCCESS)
                return unexpected(ec);

            // Info section: binary-encoded Script metadata (no payload). The
            // data section (raw payload bytes) belongs to the ScriptAsset
            // wrapper, not to the Script data object, so it is not decoded here.
            std::span<const std::byte> mf_blob(
                file.data() + header.info_offset,
                static_cast<std::size_t>(header.info_size));

            auto script = std::make_unique<DATA>();
            std::string err;
            if (!detail::decodeScriptDescription(mf_blob, *script, &err))
                return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);

        return script;
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    ScriptSerDeser::fromLuxAssetStream(std::istream& ifs)
    {
        std::vector<std::byte> file;
        if (auto ec = readAll(ifs, file); ec != EAssetError::SUCCESS)
            return unexpected(ec);

        // Decode the pure Script description through the shared, manager-less
        // entry point (header validation + info-section decode live there).
        auto data = decodeData(file.data(), file.size());
        if (!data)
            return unexpected(data.error());

        // Re-derive the validated header to locate the data section. This
        // repeats only the (cheap) header parse, NOT the description decode —
        // validateScriptHeader is the single source of truth for both calls.
        AssetFileHeader header{};
        if (auto ec = validateScriptHeader(file, header); ec != EAssetError::SUCCESS)
            return unexpected(ec);

        // Data section: raw payload bytes (semantics per script->kind).
        std::vector<std::byte> payload;
        if (header.data_size > 0)
        {
            payload.assign(
                file.begin() + static_cast<std::ptrdiff_t>(header.data_offset),
                file.begin() + static_cast<std::ptrdiff_t>(header.data_offset + header.data_size));
        }

        auto ainfo = std::make_unique<AssetInfo>(header.info);
        return std::make_unique<ScriptAsset>(
            std::move(ainfo), std::move(data.value()), std::move(payload));
    }

    EAssetError
    ScriptSerDeser::exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& ofs)
    {
        const auto* sasset = asset.as<ScriptAsset>();
        if (!sasset)
            return EAssetError::FILE_TYPE_ERROR;
        const auto* script = static_cast<const lux::rdesc::Script*>(
            sasset->rawData());
        if (!script)
            return EAssetError::ASSET_NO_DATA;

        const std::vector<std::byte> mf_blob = detail::encodeScriptDescription(*script);
        const std::size_t info_size = mf_blob.size();
        const std::size_t data_size = sasset->payload().size();

        const auto header_bytes = makeHeaderRaw<EAssetType::SCRIPT>(
            *sasset->info(), info_size, data_size);

        ofs.write(reinterpret_cast<const char*>(header_bytes.data()),
                  static_cast<std::streamsize>(header_bytes.size()));
        if (info_size > 0)
            ofs.write(reinterpret_cast<const char*>(mf_blob.data()),
                      static_cast<std::streamsize>(info_size));
        if (data_size > 0)
            ofs.write(reinterpret_cast<const char*>(sasset->payload().data()),
                      static_cast<std::streamsize>(data_size));
        return ofs.good() ? EAssetError::SUCCESS : EAssetError::WRITE_FILE_FAIL;
    }
}
