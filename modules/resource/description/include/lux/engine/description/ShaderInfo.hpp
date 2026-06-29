#pragma once
#include <cstdint>
#include <cstddef>
#include <span>
#include <string>
#include <vector>
#include <lux/engine/resource/visibility.h>

namespace lux::rdesc
{
    // 着色器阶段
    enum class EShaderType : uint8_t {
        VERTEX, 
        FRAGMENT, 
        GEOMETRY, 
        COMPUTE, 
        TESSELLATION_CTRL, 
        TESSELLATION_EVAL, 
        RAYGEN, 
        ANY_HIT, 
        CLOSEST_HIT, 
        MISS, 
        INTERSECTION, 
        CALLABLE,
		UNDEFINED
    };

    // 与 VkDescriptorType 对齐的内部枚举
    enum class EDescriptorType : uint8_t {
        SAMPLER,
        COMBINED_IMAGE_SAMPLER,
        SAMPLED_IMAGE,
        STORAGE_IMAGE,
        UNIFORM_TEXEL_BUFFER,
        STORAGE_TEXEL_BUFFER,
        UNIFORM_BUFFER,
        STORAGE_BUFFER,
        INPUT_ATTACHMENT,
        ACCELERATION_STRUCTURE,
        INLINE_UNIFORM_BLOCK
    };

    struct EDescriptorBindingInfo {
        uint32_t       set = 0;
        uint32_t       binding = 0;
        EDescriptorType type = EDescriptorType::UNIFORM_BUFFER;
        uint32_t       count = 1;        // 数组大小（未知时为 1）
        std::string    name;             // 变量名
        uint32_t       blockSize = 0;    // UBO/SSBO 字节大小；非块资源为 0
        bool           writable = false; // 对 SSBO/StorageImage：是否可写
    };

    struct DescriptorSetLayoutInfo {
        uint32_t set = 0;
        std::vector<EDescriptorBindingInfo> bindings;
    };

    struct PushConstantRangeInfo {
        uint32_t offset = 0;
        uint32_t size = 0;
    };

    // —— 专用常量（Specialization Constant） ——
    struct SpecDefaultValue 
    {
        enum class Kind : uint8_t 
        { 
            Bool, 
            Int, 
            UInt, 
            Float, 
            Double, 
            Unknown 
        } kind = Kind::Unknown;
        uint32_t bit_width = 32; // 8/16/32/64 或 16/32/64 浮点
        // 统一成 64-bit 容器，浮点用 f64 存
        union {
            int64_t   i64;
            uint64_t  u64;
            double    f64;
            uint8_t   b8;
        } v{};
    };

    struct SpecConstantInfo {
        uint32_t    id = 0;     // SPIR-V ID（可调试）
        uint32_t    constant_id = 0;     // VkSpecializationMapEntry.constantID
        std::string name;
        SpecDefaultValue default_value;
        uint32_t    vec_size = 1;        // 若为 composite，则记录维度
        uint32_t    columns = 1;
    };

    // 顶点输入，仅在 VERTEX 阶段有效
    enum class VertexScalarBase : uint8_t { Bool, Int, UInt, Float, Double, Unknown };
    struct VertexInputAttribute {
        uint32_t         location = 0;
        std::string      name;
        VertexScalarBase base = VertexScalarBase::Unknown;
        uint32_t         vec_size = 1;
        uint32_t         columns = 1;
        uint32_t         array_size = 1;
    };

    struct EntryPointInfo
    {
        std::string name{};
		EShaderType stage{};
    };

    inline const char* to_string(EShaderType s)
    {
        switch (s) {
        case EShaderType::VERTEX: return "VERTEX";
        case EShaderType::FRAGMENT: return "FRAGMENT";
        case EShaderType::GEOMETRY: return "GEOMETRY";
        case EShaderType::COMPUTE: return "COMPUTE";
        case EShaderType::TESSELLATION_CTRL: return "TESSELLATION_CTRL";
        case EShaderType::TESSELLATION_EVAL: return "TESSELLATION_EVAL";
        case EShaderType::RAYGEN: return "RAYGEN";
        case EShaderType::ANY_HIT: return "ANY_HIT";
        case EShaderType::CLOSEST_HIT: return "CLOSEST_HIT";
        case EShaderType::MISS: return "MISS";
        case EShaderType::INTERSECTION: return "INTERSECTION";
        case EShaderType::CALLABLE: return "CALLABLE";
        default: return "UNDEFINED";
        }
    }

    inline const char* to_string(EDescriptorType t)
    {
        switch (t) {
        case EDescriptorType::SAMPLER: return "Sampler";
        case EDescriptorType::COMBINED_IMAGE_SAMPLER: return "CombinedImageSampler";
        case EDescriptorType::SAMPLED_IMAGE: return "SampledImage";
        case EDescriptorType::STORAGE_IMAGE: return "StorageImage";
        case EDescriptorType::UNIFORM_TEXEL_BUFFER: return "UniformTexelBuffer";
        case EDescriptorType::STORAGE_TEXEL_BUFFER: return "StorageTexelBuffer";
        case EDescriptorType::UNIFORM_BUFFER: return "UniformBuffer";
        case EDescriptorType::STORAGE_BUFFER: return "StorageBuffer";
        case EDescriptorType::INPUT_ATTACHMENT: return "InputAttachment";
        case EDescriptorType::ACCELERATION_STRUCTURE: return "AccelerationStructure";
        case EDescriptorType::INLINE_UNIFORM_BLOCK: return "InlineUniformBlock";
        default: return "Unknown";
        }
    }

    struct ShaderInfo {
		std::vector<EntryPointInfo>          entry_points;
        std::vector<DescriptorSetLayoutInfo> sets;
        std::vector<PushConstantRangeInfo>   push_constants;
        std::vector<SpecConstantInfo>        spec_constants;
        std::vector<VertexInputAttribute>    vertex_inputs;

        size_t findSet(uint32_t s) const {
            for (size_t i = 0; i < sets.size(); ++i) if (sets[i].set == s) return i;
            return static_cast<size_t>(-1);
        }
        static constexpr size_t npos = static_cast<size_t>(-1);

        LUX_RESOURCE_PUBLIC static std::vector<std::byte> serialize(const ShaderInfo& info);
        LUX_RESOURCE_PUBLIC static bool deserialize(std::span<const std::byte> buffer, ShaderInfo& out, std::string* err = nullptr);
    };
}
