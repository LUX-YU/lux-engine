#pragma once
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/material/graph/MaterialGraph.hpp>
#include <string_view>

namespace lux::material
{
    struct MaterialSource final
    {
        lux::asset::AssetId id;
        std::string name;
        MaterialGraph graph;
    };

    struct MaterialSourceLimits final
    {
        std::size_t max_bytes{16U * 1024U * 1024U};
        std::size_t max_nodes{16384}, max_pins{131072}, max_slots{1024}, max_string_bytes{4096};
    };
    enum class EMaterialSourceError : std::uint8_t
    {
        INVALID_ARGUMENT,
        LIMIT_EXCEEDED,
        PARSE_FAILURE,
        UNSUPPORTED_FORMAT,
        UNKNOWN_FIELD,
        INVALID_IDENTITY,
        INVALID_VALUE,
        UNKNOWN_NODE_KIND,
        INVALID_TOPOLOGY
    };

    struct MaterialSourceFailure final
    {
        EMaterialSourceError code{};
        std::string field;
        NodeId node;
        PinId pin;
        std::uint32_t line{}, column{};
        std::optional<MaterialCompileFailure> cause;
    };
    template <class T> using MaterialSourceResult = lux::cxx::expected<T, MaterialSourceFailure>;
    // Local draft validation; does not assign identities or construct a temporary graph.
    [[nodiscard]] LUX_ENGINE_MATERIAL_GRAPH_PUBLIC MaterialSourceResult<void> validateMaterialName(
        std::string_view,
        MaterialSourceLimits = {}
    ) noexcept;
    [[nodiscard]] LUX_ENGINE_MATERIAL_GRAPH_PUBLIC MaterialSourceResult<void> validateMaterialTextureSlots(
        std::span<const TextureSlotDecl>,
        MaterialSourceLimits = {}
    ) noexcept;
    [[nodiscard]] LUX_ENGINE_MATERIAL_GRAPH_PUBLIC MaterialSourceResult<void> validateMaterialParameterSlots(
        std::span<const ParamSlotDecl>,
        MaterialSourceLimits = {}
    ) noexcept;
    [[nodiscard]] LUX_ENGINE_MATERIAL_GRAPH_PUBLIC MaterialSourceResult<void> validateMaterialNode(
        const MaterialNode&,
        NodeId = {},
        MaterialSourceLimits = {}
    ) noexcept;
    // In-memory structural validation. Does not format/parse TOML or run the compiler.
    [[nodiscard]] LUX_ENGINE_MATERIAL_GRAPH_PUBLIC MaterialSourceResult<void> validateMaterialSource(
        const MaterialSource&,
        MaterialSourceLimits = {}
    ) noexcept;
    // Writes v2 with canonical node identity/version and registered payload codecs; reads v1 builtin sources.
    // No file I/O, compiler, Editor or GPU state enters this codec.
    // Disconnected nodes, missing output and cycles remain saveable; compilation reports semantic diagnostics.
    [[nodiscard]] LUX_ENGINE_MATERIAL_GRAPH_PUBLIC MaterialSourceResult<std::string> encodeMaterialSource(
        const MaterialSource&,
        MaterialSourceLimits = {}
    ) noexcept;
    [[nodiscard]] LUX_ENGINE_MATERIAL_GRAPH_PUBLIC MaterialSourceResult<MaterialSource> decodeMaterialSource(
        std::string_view,
        const MaterialNodeCatalog&,
        MaterialSourceLimits = {}
    ) noexcept;
    // Builtin-only convenience. Extensions supply their immutable catalog explicitly.
    [[nodiscard]] LUX_ENGINE_MATERIAL_GRAPH_PUBLIC MaterialSourceResult<MaterialSource> decodeMaterialSource(
        std::string_view,
        MaterialSourceLimits = {}
    ) noexcept;
} // namespace lux::material
