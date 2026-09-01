#pragma once
// =============================================================================
//  DataPin.hpp  —  Typed data pins and connections for the material graph
// -----------------------------------------------------------------------------
//  The material graph is pure dataflow: only data pins exist, none of FlowForge's
//  exec pins / tokens. Pin types use EMatValueType (a plain enum) and do not go
//  through the lux::meta reflection system.
//
//  Authoring owns this editable source model; cook lowers it to runtime data.
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include <lux/engine/description/MaterialGraphContract.hpp>  // EMatValueType

namespace lux::material
{
    using EMatValueType = lux::rdesc::EMatValueType;
    using EMaterialAttribute = lux::rdesc::EMaterialAttribute;
    using EMaterialInput = lux::rdesc::EMaterialInput;
    using EMaterialPass = lux::rdesc::EMaterialPass;
    using EMaterialShadingModel = lux::rdesc::EMaterialShadingModel;
    using EAlphaMode = lux::rdesc::EAlphaMode;
    using ELightingTechnique = lux::rdesc::ELightingTechnique;
    using MaterialAttributeDesc = lux::rdesc::MaterialAttributeDesc;
    using lux::rdesc::inputType;
    using lux::rdesc::kMaterialAttributes;
    using lux::rdesc::kMaterialInputs;

    using node_id = uint64_t;

    inline constexpr node_id invalid_node = ~0ull;
    inline constexpr uint32_t invalid_pin = ~0u;

    enum class EPinDirection : uint8_t
    {
        Input,
        Output
    };

    /**
     * @brief The source of a connection: an output pin on some node.
     *        An input pin has at most one source (pure dataflow, fan-in = 1).
     */
    struct PinLink
    {
        node_id  node = invalid_node;  ///< Source node
        uint32_t pin  = invalid_pin;   ///< Index of the output pin on the source node

        bool valid() const noexcept
        {
            return node != invalid_node && pin != invalid_pin;
        }
    };

    /**
     * @brief A typed data pin (an aggregate type, for easy initialization).
     *        - Input pin: `source` points to where the value comes from; when
     *          unconnected, `constant` is used as the default value.
     *        - Output pin: `source` / `constant` are ignored.
     */
    struct DataPin
    {
        std::string   name;
        EMatValueType type      = EMatValueType::Float;
        EPinDirection direction = EPinDirection::Input;
        PinLink       source;                    ///< Only meaningful for input pins
        float         constant[4] = { 0, 0, 0, 0 }; ///< Default constant used when an input is unconnected
    };

} // namespace lux::material
