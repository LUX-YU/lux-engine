#pragma once
#include <lux/engine/description/ShaderInfo.hpp>

namespace lux::asset
{
    // ===== Backward compatibility: re-export description types =====
    using lux::rdesc::EShaderType;
    using lux::rdesc::EDescriptorType;
    using lux::rdesc::EDescriptorBindingInfo;
    using lux::rdesc::DescriptorSetLayoutInfo;
    using lux::rdesc::PushConstantRangeInfo;
    using lux::rdesc::SpecDefaultValue;
    using lux::rdesc::SpecConstantInfo;
    using lux::rdesc::VertexScalarBase;
    using lux::rdesc::VertexInputAttribute;
    using lux::rdesc::EntryPointInfo;
    using lux::rdesc::ShaderInfo;
    using lux::rdesc::to_string;
}