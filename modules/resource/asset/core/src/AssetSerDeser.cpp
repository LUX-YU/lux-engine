#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <AssetManagerImpl.hpp>
#include <filesystem>
#include <istream>
#include <streambuf>
#include <system_error>

namespace lux::asset
{
    namespace
    {
        // Minimal read-only istream over a caller-owned memory block (no
        // std::ispanstream until C++23). Lets fromLuxAssetMemory run a pak /
        // embedded .luxasset image through the exact same per-type stream
        // readers as loose files. Supports the seekg/tellg/read subset those
        // readers are contracted to (see fromLuxAssetStream doc).
        class imemstream : private std::streambuf, public std::istream
        {
        public:
            // Both bases expose identical pos_type/off_type; re-declare to
            // resolve the ambiguous name lookup.
            using pos_type = std::istream::pos_type;
            using off_type = std::istream::off_type;

            imemstream(const void* data, size_t len)
                : std::istream(static_cast<std::streambuf*>(this))
            {
                // setg wants char*; the buffer is never written through.
                auto* p = const_cast<char*>(static_cast<const char*>(data));
                setg(p, p, p + len);
            }

        protected:
            pos_type seekoff(off_type off, std::ios_base::seekdir dir,
                             std::ios_base::openmode which) override
            {
                if (!(which & std::ios_base::in))
                    return pos_type(off_type(-1));
                char* target =
                    dir == std::ios_base::beg ? eback() + off :
                    dir == std::ios_base::cur ? gptr()  + off :
                                                egptr() + off;
                if (target < eback() || target > egptr())
                    return pos_type(off_type(-1));
                setg(eback(), target, egptr());
                return pos_type(target - eback());
            }

            pos_type seekpos(pos_type pos, std::ios_base::openmode which) override
            {
                return seekoff(off_type(pos), std::ios_base::beg, which);
            }
        };

        // Walk the auxiliary payload region (everything after the data section,
        // i.e. [data_offset + data_size, EOF]) and return the tagged payloads.
        // Empty for old files / no-payload writes. Tolerant of a truncated or
        // garbage tail: stops at the first malformed block rather than throwing.
        std::vector<Payload> readPayloads(std::istream& ifs)
        {
            std::vector<Payload> out;

            ifs.clear();
            ifs.seekg(0, std::ios::end);
            const std::streamoff end = ifs.tellg();
            constexpr std::streamoff kLeadingEnd = static_cast<std::streamoff>(
                offsetof(AssetFileHeader, data_size) + sizeof(std::uint64_t));
            if (end < kLeadingEnd)
                return out;

            // magic/version/info_*/data_* are at version-stable offsets in every
            // header layout, so read data_offset/data_size raw without first
            // committing to a version.
            std::uint64_t data_offset = 0, data_size = 0;
            ifs.clear();
            ifs.seekg(static_cast<std::streamoff>(offsetof(AssetFileHeader, data_offset)),
                      std::ios::beg);
            ifs.read(reinterpret_cast<char*>(&data_offset), sizeof(data_offset));
            ifs.read(reinterpret_cast<char*>(&data_size),   sizeof(data_size));
            if (!ifs)
                return out;

            std::streamoff pos = static_cast<std::streamoff>(data_offset + data_size);
            if (pos < kLeadingEnd || pos > end)
                return out; // no region, or a corrupt/oversized data section

            while (pos + static_cast<std::streamoff>(sizeof(PayloadBlockHeader)) <= end)
            {
                ifs.clear();
                ifs.seekg(pos, std::ios::beg);
                PayloadBlockHeader bh{};
                ifs.read(reinterpret_cast<char*>(&bh), sizeof(bh));
                if (!ifs)
                    break;

                const std::streamoff block_data =
                    pos + static_cast<std::streamoff>(sizeof(bh));
                if (bh.size > static_cast<std::uint64_t>(end - block_data))
                    break; // truncated / malformed — ignore the rest of the tail

                std::vector<std::byte> bytes(static_cast<std::size_t>(bh.size));
                if (bh.size > 0)
                {
                    ifs.read(reinterpret_cast<char*>(bytes.data()),
                             static_cast<std::streamsize>(bh.size));
                    if (!ifs)
                        break;
                }
                out.push_back(Payload{ bh.tag, std::move(bytes) });
                pos = block_data + static_cast<std::streamoff>(bh.size);
            }
            return out;
        }
    }

    std::vector<std::byte> AssetSerDeser::serializeAssetInfo(const AssetInfo &asset)
    {
        static_assert(std::is_standard_layout_v<AssetInfo>);
        std::vector<std::byte> buffer;
        buffer.resize(sizeof(AssetInfo));

        memcpy(buffer.data(), &asset, sizeof(AssetInfo));

        return buffer;
    }

    lux::cxx::expected<AssetIDPair, EAssetError>
    AssetSerDeser::importFromFile(const std::filesystem::path& external_path)
    {
        if (!std::filesystem::exists(external_path))
            return lux::cxx::unexpected(EAssetError::FILE_NOT_EXIST);

        std::ifstream ifile(external_path, std::ios::binary);
        if (!ifile) 
            return lux::cxx::unexpected(EAssetError::FILE_OPEN_FAIL);

        auto expected = fromFileStream(ifile);
        
        if (!expected.has_value())
        {
            return lux::cxx::unexpected(expected.error());
        }

        const auto asset_id = expected.value()->id();
        auto* asset_ptr = expected.value().get();

		if (!submitAsset(std::move(expected.value())))
		{
			return lux::cxx::unexpected(EAssetError::ASSET_ALREADY_EXIST);
		}

        return AssetIDPair{ asset_ptr, asset_id };
    }

    lux::cxx::expected<AssetIDPair, EAssetError>
    AssetSerDeser::fromLuxAsset(const std::filesystem::path& path)
    {
        if (!std::filesystem::exists(path))
            return lux::cxx::unexpected(EAssetError::FILE_NOT_EXIST);

        std::ifstream ifile(path, std::ios::binary);
        if (!ifile) 
            return lux::cxx::unexpected(EAssetError::FILE_OPEN_FAIL);

        auto expected = fromLuxAssetStream(ifile);
        if (!expected.has_value())
        {
            return lux::cxx::unexpected(expected.error());
        }

        // Attach any auxiliary payloads (tagged binary slots after the data
        // section) before the asset is handed to the manager. Absent for old
        // files / no-payload writes — harmless no-op.
        if (auto pls = readPayloads(ifile); !pls.empty())
            expected.value()->setPayloads(std::move(pls));

        const auto asset_id = expected.value()->id();
        auto* asset_ptr     = expected.value().get();

        if(!submitAsset(std::move(expected.value())))
        {
            return lux::cxx::unexpected(EAssetError::ASSET_ALREADY_EXIST);
        }

        return AssetIDPair{ asset_ptr, asset_id };
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    AssetSerDeser::parseLuxAssetMemory(const void* data, size_t len)
    {
        if (!data || len == 0)
            return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);

        imemstream stream(data, len);

        auto expected = fromLuxAssetStream(stream);
        if (!expected.has_value())
            return lux::cxx::unexpected(expected.error());

        if (auto pls = readPayloads(stream); !pls.empty())
            expected.value()->setPayloads(std::move(pls));

        // Returned UNREGISTERED — caller (sync fromLuxAssetMemory, or an async
        // loader's main-thread step) decides when to registerAsset(). Nothing
        // above touches the manager, so this is safe off the main thread.
        return std::move(expected.value());
    }

    lux::cxx::expected<AssetIDPair, EAssetError>
    AssetSerDeser::fromLuxAssetMemory(const void* data, size_t len)
    {
        auto parsed = parseLuxAssetMemory(data, len);
        if (!parsed.has_value())
            return lux::cxx::unexpected(parsed.error());

        const auto asset_id = parsed.value()->id();
        auto* asset_ptr     = parsed.value().get();

        if (!submitAsset(std::move(parsed.value())))
        {
            return lux::cxx::unexpected(EAssetError::ASSET_ALREADY_EXIST);
        }

        return AssetIDPair{ asset_ptr, asset_id };
    }

    lux::cxx::expected<AssetIDPair, EAssetError>
    AssetSerDeser::importFromMemory(const void* data, size_t len)
    {
        auto expected = fromMemory(data, len);
        if (!expected.has_value())
        {
            return lux::cxx::unexpected(expected.error());
        }

        const auto asset_id = expected.value()->id();
        auto* asset_ptr     = expected.value().get();

        if (!submitAsset(std::move(expected.value())))
        {
            return lux::cxx::unexpected(EAssetError::ASSET_ALREADY_EXIST);
        }

        return AssetIDPair{ asset_ptr, asset_id };
    }

    EAssetError AssetSerDeser::exportAsLuxAsset(const asset_id_t& asset_id, const std::filesystem::path& path)
    {
        if (!manager().hasAsset(asset_id))
        {
            return EAssetError::ASSET_NOT_EXIST;
        }

        const auto parent = path.parent_path();
        if (!parent.empty() && !existsOrMkdir(parent))
            return EAssetError::UNKNOWN_FILESYSTEM_ERROR;

        const LuxAsset* asset = manager().queryAsset(asset_id);
        if(!asset)
        {
            return EAssetError::UNKNOWN_ERROR;
        }

        // Atomic write: stream to a sibling temp file, then rename over the
        // target. A crash / disk-full / permission failure mid-write then
        // leaves the original .luxasset intact (or just a stray .tmp) instead
        // of a half-written file that fails header validation on next load.
        std::filesystem::path tmp = path;
        tmp += ".tmp";
        {
            std::ofstream ofile(tmp, std::ios::binary | std::ios::trunc);
            if (!ofile)
                return EAssetError::FILE_OPEN_FAIL;

            EAssetError err = exportAsLuxAssetStream(*asset, ofile);

            // Append the asset's auxiliary payloads (tagged binary slots) after
            // the data section, ordered by tag for reproducible output. Done
            // centrally so no per-type SerDeser is involved; placement after
            // the data section keeps it invisible to per-type readers (which
            // slice only [data_offset, data_offset + data_size]).
            if (err == EAssetError::SUCCESS && !asset->payloads().empty())
            {
                for (const auto& p : asset->payloads())
                {
                    const PayloadBlockHeader bh{
                        p.tag, static_cast<std::uint64_t>(p.data.size()) };
                    ofile.write(reinterpret_cast<const char*>(&bh), sizeof(bh));
                    if (!p.data.empty())
                        ofile.write(reinterpret_cast<const char*>(p.data.data()),
                                    static_cast<std::streamsize>(p.data.size()));
                }
            }

            // Flush + close EXPLICITLY before inspecting the stream state.
            // A buffered write that only fails at flush time (disk full / quota
            // / I/O error — the very case this atomic path defends against)
            // leaves good()==true until the bytes actually hit the OS. The
            // ofstream destructor would flush and swallow that error; checking
            // after an explicit flush+close catches it while we can still
            // discard the temp and leave the original intact.
            ofile.flush();
            ofile.close();
            if (err != EAssetError::SUCCESS || !ofile.good())
            {
                std::error_code rm_ec;
                std::filesystem::remove(tmp, rm_ec);
                return err != EAssetError::SUCCESS ? err : EAssetError::WRITE_FILE_FAIL;
            }
        }

        // std::filesystem::rename replaces an existing target atomically
        // (POSIX rename / Win32 MoveFileEx REPLACE_EXISTING). On failure the
        // original is untouched; drop the temp so we don't leak it.
        std::error_code mv_ec;
        std::filesystem::rename(tmp, path, mv_ec);
        if (mv_ec)
        {
            std::error_code rm_ec;
            std::filesystem::remove(tmp, rm_ec);
            return EAssetError::WRITE_FILE_FAIL;
        }
        return EAssetError::SUCCESS;
    }

    bool AssetSerDeser::deserializeAssetInfo(const std::vector<std::byte>& data, AssetInfo& asset)
    {
        static_assert(std::is_standard_layout_v<AssetInfo>);
        static_assert(std::is_trivially_copyable_v<AssetInfo>);
        if (data.size() != sizeof(AssetInfo))
            return false;

        memcpy(&asset, data.data(), sizeof(AssetInfo));
        return true;
    }

    bool AssetSerDeser::submitAsset(std::unique_ptr<LuxAsset> asset)
    {
        return manager().registerAsset(std::move(asset));
    }

    bool AssetSerDeser::existsOrMkdir(const std::filesystem::path& path)
    {
        if (std::filesystem::exists(path)) 
            return std::filesystem::is_directory(path);

        return std::filesystem::create_directories(path);
    }
}