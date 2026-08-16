#pragma once
#include <lux/engine/authoring/assets/FlowGraphAsset.hpp>
#include <lux/engine/resource/asset/AssetCodecCatalog.hpp>
#include <lux/engine/authoring/assets/visibility.h>

namespace lux::authoring
{
    /// Flow graphs have no external source format (they are authored in the
    /// editor's node graph), so this config is empty — only the .luxasset
    /// (de)serialization paths are implemented.
    struct FlowGraphLoadConfig
    {
    };

    /// (De)serializes FlowGraphAsset to/from the engine .luxasset format.
    /// Layout mirrors MaterialSerDeser: [AssetFileHeader][info blob], with
    /// the whole payload packed into the info section (data_size == 0). The
    /// info blob is `u32 version; u32 blob_len; blob` where blob is the
    /// FlowGraphCodec binary graph.
    ///
    /// Decode resolves native-call nodes through NodeRegistry::global() —
    /// the editor must have registered its native nodes there before a
    /// graph referencing them is loaded.
    class LUX_ENGINE_AUTHORING_ASSETS_PUBLIC FlowGraphSerDeser final
        : public lux::asset::TAssetSerDeser<FlowGraphLoadConfig>
    {
    public:
        explicit FlowGraphSerDeser(
            std::shared_ptr<lux::asset::AssetManager> manager);
        ~FlowGraphSerDeser() override;

        /// Decode a complete .luxasset FLOW_GRAPH memory image into the pure
        /// asset data object (no AssetManager / registration / AssetInfo
        /// wrapper). Same contract as MaterialSerDeser::decodeData.
        [[nodiscard]] static lux::cxx::expected<
            std::unique_ptr<FlowGraphData>, lux::asset::EAssetError>
        decodeData(const void* bytes, std::size_t len) noexcept;

        /// Deep-copy a graph by round-tripping it through the binary codec
        /// (FlowGraph is move-only and node wiring is pointer-based, so the
        /// codec IS the clone). Used by the editor to open an asset's graph
        /// into an editable working copy without aliasing the asset's data.
        [[nodiscard]] static bool
        cloneGraph(const lux::flowforge::FlowGraph& src,
                   lux::flowforge::FlowGraph&       out,
                   std::string*                     err = nullptr);

        /// Serialize a graph into the canonical FlowGraphCodec binary blob —
        /// the graph's CONTENT IDENTITY. Derivative caches (the editor's
        /// flowforge AOT dll cache) hash it to detect graph changes. Empty
        /// result = serialization failure (err carries the reason).
        [[nodiscard]] static std::vector<std::byte>
        encodeGraph(const lux::flowforge::FlowGraph& src,
                    std::string*                     err = nullptr);

    protected:
        lux::cxx::expected<
            std::unique_ptr<lux::asset::LuxAsset>, lux::asset::EAssetError>
        fromFileStream(std::ifstream& ifs) override;

        lux::cxx::expected<
            std::unique_ptr<lux::asset::LuxAsset>, lux::asset::EAssetError>
        fromLuxAssetStream(std::istream& ifs) override;

        lux::asset::EAssetError exportAsLuxAssetStream(
            const lux::asset::LuxAsset& asset,
            std::ofstream& ofs) override;
    };

    /// Runtime codecs plus the authored FlowGraph codec, frozen as one
    /// immutable product snapshot.
    [[nodiscard]] LUX_ENGINE_AUTHORING_ASSETS_PUBLIC
    std::shared_ptr<const lux::asset::AssetCodecCatalog>
    authoringAssetCodecCatalog() noexcept;

} // namespace lux::authoring
