#pragma once

#include <cstdint>
#include <lux/engine/description/Image.hpp>
#include <lux/engine/render/capabilities/CapabilityComposition.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/visibility.h>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace lux::render
{
    struct CapabilityPortRef final
    {
        std::string contribution;
        std::string port;
        bool operator==(const CapabilityPortRef&) const noexcept = default;
    };

    // Explicit wiring belongs to extension authoring. Ordinary effects use unique compatible producers.
    struct CapabilityConnection final
    {
        CapabilityPortRef source;
        CapabilityPortRef destination;
        bool operator==(const CapabilityConnection&) const noexcept = default;
    };

    enum class ECapabilityPortDirection : std::uint8_t
    {
        INPUT,
        OUTPUT
    };

    // Zero dimensions/counts are unconstrained during cold selection. Concrete
    // allocation extents and resource access remain the RenderGraph's responsibility.
    struct CapabilityImageShape final
    {
        std::optional<rdesc::ETextureFormat> format;
        std::uint32_t samples = 0;
        std::uint32_t layers = 0;
        std::uint32_t width = 0, height = 0;
        bool operator==(const CapabilityImageShape&) const noexcept = default;
    };

    struct CapabilityBufferShape final
    {
        std::uint32_t element_stride = 0;
        std::uint64_t minimum_elements = 0;
        bool operator==(const CapabilityBufferShape&) const noexcept = default;
    };

    using VCapabilityPortShape = std::variant<CapabilityImageShape, CapabilityBufferShape>;

    // A CPU description of an encoding boundary, not a Storage, descriptor binding
    // or allocation. Coordinate conventions are complete identities, never inferred
    // from equal image formats (e.g. world normals versus view normals).
    struct CapabilityPort final
    {
        std::string key;
        ECapabilityPortDirection direction = ECapabilityPortDirection::INPUT;
        std::string semantic;
        VCapabilityPortShape shape;
        std::string coordinates;
        bool required = true;
        bool operator==(const CapabilityPort&) const noexcept = default;
    };

    // Implementation-only cold graph interface. Public capability identity and payloads
    // are declared separately; ordinary clients never receive this resource metadata.
    struct CapabilityGraphDescription final
    {
        std::vector<CapabilityPort> ports;
        std::vector<rdesc::ETextureFormat> target_formats;
        std::vector<std::uint32_t> target_samples;
        std::vector<ETargetKind> target_kinds;
        bool operator==(const CapabilityGraphDescription&) const noexcept = default;
    };

    [[nodiscard]] LUX_RENDER_PUBLIC bool
    capabilityPortsCompatible(const CapabilityPort& output, const CapabilityPort& input) noexcept;

    struct ResolvedCapabilityPort final
    {
        std::uint32_t contribution = 0;
        std::uint32_t port = 0;
        bool operator==(const ResolvedCapabilityPort&) const noexcept = default;
    };

    struct ResolvedCapabilityConnection final
    {
        ResolvedCapabilityPort source;
        ResolvedCapabilityPort destination;
    };

    struct CapabilityConnections final
    {
        std::vector<ResolvedCapabilityConnection> connections;
        std::vector<ResolvedCapabilityPort> outputs;
        std::vector<std::uint32_t> order;
    };

    // Only these selected declarations participate. A required input with exactly
    // one compatible output is linked automatically; optional inputs stay unbound
    // unless explicitly selected. Ambiguity never selects registration order.
    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<CapabilityConnections> resolveCapabilityConnections(
        std::span<const std::string_view> keys,
        std::span<const CapabilityGraphDescription* const> declarations,
        std::span<const CapabilityConnection> connections,
        std::span<const CapabilityOutput> outputs
    ) noexcept;

} // namespace lux::render
