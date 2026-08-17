#include <lux/engine/authoring/assets/FlowGraphSerDeser.hpp>
#include <lux/engine/authoring/assets/FlowGraphCodec.hpp>
#include <lux/engine/authoring/assets/MaterialAuthoringCodec.hpp>

#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/authoring/flowforge/NodeRegistry.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <span>
#include <vector>

namespace lux::authoring
{
    using lux::asset::AssetFileHeader;
    using lux::asset::AssetInfo;
    using lux::asset::AssetSerDeser;
    using lux::asset::asset_magic_number_of;
    using lux::asset::EAssetError;
    using lux::asset::EAssetType;
    using lux::asset::LuxAsset;
    using lux::cxx::unexpected;

    FlowGraphSerDeser::FlowGraphSerDeser(std::shared_ptr<lux::asset::AssetManager> manager)
        : TAssetSerDeser(std::move(manager))
    {
    }

    FlowGraphSerDeser::~FlowGraphSerDeser() = default;

    // ─────────────────────────────────────────────────────────────────────────
    //  .luxasset FlowGraph format v1
    //
    //  Everything lives in the header's "info" section (data_size == 0), like
    //  MaterialSerDeser. Layout (LE POD copies):
    //
    //    u32 version (= kFormatVersion)
    //    u32 graph_blob_len;  byte × graph_blob_len   (detail::encodeFlowGraph)
    // ─────────────────────────────────────────────────────────────────────────
    namespace
    {
        constexpr std::uint32_t kFormatVersion = 1;

        template <class T>
        void appendPod(std::vector<std::byte>& buf, const T& v)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            const auto off = buf.size();
            buf.resize(off + sizeof(T));
            std::memcpy(buf.data() + off, &v, sizeof(T));
        }

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

    lux::cxx::expected<std::unique_ptr<FlowGraphData>, EAssetError>
    FlowGraphSerDeser::decodeData(const void* bytes, std::size_t len) noexcept
    {
        if (bytes == nullptr) return unexpected(EAssetError::ABNORMAL_FILE_SIZE);
        const auto file = std::span<const std::byte>{
            static_cast<const std::byte*>(bytes), len};

            AssetFileHeader header{};
            if (auto ec = loadHeaderRaw<EAssetType::FLOW_GRAPH>(file, header);
                ec != EAssetError::SUCCESS)
            {
                return unexpected(ec);
            }
            if (header.magic_number != asset_magic_number_of<EAssetType::FLOW_GRAPH>::value)
                return unexpected(EAssetError::WRONG_FILE_HEADER);
            if (header.info_offset != sizeof(AssetFileHeader))
                return unexpected(EAssetError::WRONG_FILE_HEADER);
            if (header.info_offset > file.size()
                || header.info_size > file.size() - header.info_offset)
                return unexpected(EAssetError::ABNORMAL_FILE_SIZE);

            const std::byte* p   = file.data() + header.info_offset;
            const std::byte* end = p + header.info_size;

            auto readPod = [&](auto& v) -> bool {
                using T = std::remove_reference_t<decltype(v)>;
                if (static_cast<std::size_t>(end - p) < sizeof(T)) return false;
                std::memcpy(&v, p, sizeof(T));
                p += sizeof(T);
                return true;
            };

            std::uint32_t version = 0;
            if (!readPod(version) || version != kFormatVersion)
                return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);

            std::uint32_t blob_len = 0;
            if (!readPod(blob_len))
                return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
            if (static_cast<std::size_t>(end - p) < blob_len)
                return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);

            auto payload = std::make_unique<FlowGraphData>();
            std::string err;
            if (!detail::decodeFlowGraph(
                    std::span<const std::byte>(p, blob_len),
                    payload->graph,
                    lux::flowforge::NodeRegistry::global(),
                    &err))
            {
                return unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
            }
        return payload;
    }

    bool FlowGraphSerDeser::cloneGraph(const lux::flowforge::FlowGraph& src,
                                       lux::flowforge::FlowGraph&       out,
                                       std::string*                     err)
    {
        const std::vector<std::byte> blob = detail::encodeFlowGraph(src, err);
        if (blob.empty())
            return false;
        return detail::decodeFlowGraph(
            std::span<const std::byte>(blob), out,
            lux::flowforge::NodeRegistry::global(), err);
    }

    std::vector<std::byte>
    FlowGraphSerDeser::encodeGraph(const lux::flowforge::FlowGraph& src,
                                   std::string*                     err)
    {
        return detail::encodeFlowGraph(src, err);
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    FlowGraphSerDeser::fromFileStream(std::ifstream&)
    {
        // No external source format — flow graphs are authored in the editor.
        return unexpected(EAssetError::UNSUPPORTED);
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    FlowGraphSerDeser::fromLuxAssetStream(std::istream& ifs)
    {
        std::vector<std::byte> file;
        if (auto ec = readAllStream(ifs, file); ec != EAssetError::SUCCESS)
            return unexpected(ec);

        AssetFileHeader header{};
        if (auto ec = loadHeaderRaw<EAssetType::FLOW_GRAPH>(file, header);
            ec != EAssetError::SUCCESS)
        {
            return unexpected(ec);
        }

        auto data = decodeData(file.data(), file.size());
        if (!data.has_value())
            return unexpected(data.error());

        auto ainfo = std::make_unique<AssetInfo>(header.info);
        auto asset = std::make_unique<FlowGraphAsset>(std::move(ainfo));
        asset->setData(std::move(data.value()));
        return std::unique_ptr<LuxAsset>(std::move(asset));
    }

    EAssetError
    FlowGraphSerDeser::exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& ofile)
    {
        const auto* fa = asset.as<FlowGraphAsset>();
        if (!fa) return EAssetError::FILE_TYPE_ERROR;
        const FlowGraphData* d = fa->data();
        if (!d) return EAssetError::ASSET_NO_DATA;

        std::string err;
        const std::vector<std::byte> graph_blob =
            detail::encodeFlowGraph(d->graph, &err);
        if (graph_blob.empty())
            return EAssetError::UNSUPPORTED;  // graph contains a non-serializable node

        std::vector<std::byte> info;
        info.reserve(8 + graph_blob.size());
        appendPod<std::uint32_t>(info, kFormatVersion);
        appendPod<std::uint32_t>(info, static_cast<std::uint32_t>(graph_blob.size()));
        info.insert(info.end(), graph_blob.begin(), graph_blob.end());

        const auto header_bytes = makeHeaderRaw<EAssetType::FLOW_GRAPH>(
            *fa->info(), info.size(), /*data_size*/ 0);

        ofile.write(reinterpret_cast<const char*>(header_bytes.data()),
                    static_cast<std::streamsize>(header_bytes.size()));
        if (!info.empty())
            ofile.write(reinterpret_cast<const char*>(info.data()),
                        static_cast<std::streamsize>(info.size()));
        return ofile.good() ? EAssetError::SUCCESS : EAssetError::WRITE_FILE_FAIL;
    }

    namespace
    {
        std::unique_ptr<lux::asset::AssetSerDeser> createFlowGraphCodec(
        lux::asset::EAssetType type,
        std::shared_ptr<lux::asset::AssetManager> manager)
        {
            if (type != lux::asset::EAssetType::FLOW_GRAPH)
                return nullptr;
            return std::make_unique<FlowGraphSerDeser>(std::move(manager));
        }
    } // namespace

    std::shared_ptr<const lux::asset::AssetCodecCatalog>
    authoringAssetCodecCatalog() noexcept
    {
        static const auto catalog = []
        {
            const auto runtime = lux::asset::runtimeAssetCodecCatalog();
            std::vector<lux::asset::AssetCodecDescriptor> descriptors{
                runtime->descriptors().begin(),
                runtime->descriptors().end()};
            for (auto& descriptor : descriptors)
                if (descriptor.type == lux::asset::EAssetType::MATERIAL)
                    descriptor.decode = &detail::decodeAuthoringMaterial;
            descriptors.push_back(lux::asset::AssetCodecDescriptor{
                lux::asset::EAssetType::FLOW_GRAPH,
                lux::cxx::type_hash<FlowGraphAsset>(),
                std::string{lux::cxx::type_name<FlowGraphAsset>()},
                lux::asset::EAssetShippingClass::AUTHORING_ONLY,
                &createFlowGraphCodec,
                nullptr,
                nullptr,
                {}});
            auto built = lux::asset::AssetCodecCatalog::build(
                std::move(descriptors));
            if (!built)
                std::abort();
            return std::make_shared<const lux::asset::AssetCodecCatalog>(
                std::move(*built));
        }();
        return catalog;
    }

} // namespace lux::authoring
