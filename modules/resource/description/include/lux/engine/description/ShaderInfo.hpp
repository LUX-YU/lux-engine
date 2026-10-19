#pragma once
#include <cstddef>
#include <cstdint>
#include <lux/engine/description/ShaderBlock.hpp>
#include <span>
#include <string>
#include <vector>

namespace lux::rdesc
{
    // Shader stage
    enum class EShaderType : uint8_t
    {
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

    // Internal enum aligned with VkDescriptorType
    enum class EDescriptorType : uint8_t
    {
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

    struct EDescriptorBindingInfo
    {
        uint32_t set = 0;
        uint32_t binding = 0;
        EDescriptorType type = EDescriptorType::UNIFORM_BUFFER;
        uint32_t count = 1;     // Array size (1 if unknown)
        std::string name;       // Variable name
        uint32_t blockSize = 0; // UBO/SSBO size in bytes; 0 for non-block resources
        bool writable = false;  // For SSBO/StorageImage: whether it is writable.
        bool readable = true;
        ShaderBlock block;        // Reflected members/offsets/strides; opaque descriptors leave this empty.
        std::uint32_t stages = 0; // PassContract bits: Vertex=1, Fragment=2, Compute=4.
        bool operator==(const EDescriptorBindingInfo&) const noexcept = default;
    };

    struct DescriptorSetLayoutInfo
    {
        uint32_t set = 0;
        std::vector<EDescriptorBindingInfo> bindings;
        bool operator==(const DescriptorSetLayoutInfo&) const noexcept = default;
    };

    struct PushConstantRangeInfo
    {
        uint32_t offset = 0;
        uint32_t size = 0;
        ShaderBlock block;
        std::uint32_t stages = 0;
        bool operator==(const PushConstantRangeInfo&) const noexcept = default;
    };

    // —— Specialization Constant ——
    struct SpecDefaultValue
    {
        enum class EKind : uint8_t
        {
            BOOL,
            INT,
            UINT,
            FLOAT,
            DOUBLE,
            UNKNOWN
        } kind = EKind::UNKNOWN;
        uint32_t bit_width = 32; // 8/16/32/64, or 16/32/64 for float

        // Stored uniformly in a 64-bit container; floats use f64
        union
        {
            int64_t i64;
            uint64_t u64;
            double f64;
            uint8_t b8;
        } v{};

        bool operator==(const SpecDefaultValue& other) const noexcept
        {
            if (kind != other.kind || bit_width != other.bit_width)
            {
                return false;
            }
            switch (kind)
            {
            case EKind::BOOL:
                return v.b8 == other.v.b8;
            case EKind::INT:
                return v.i64 == other.v.i64;
            case EKind::UINT:
                return v.u64 == other.v.u64;
            case EKind::FLOAT:
            case EKind::DOUBLE:
                return v.f64 == other.v.f64;
            case EKind::UNKNOWN:
                return true;
            }
            return false;
        }
    };

    struct SpecConstantInfo
    {
        uint32_t id = 0;          // SPIR-V ID (for debugging)
        uint32_t constant_id = 0; // VkSpecializationMapEntry.constantID
        std::string name;
        SpecDefaultValue default_value;
        uint32_t vec_size = 1; // Records the dimension if this is a composite
        uint32_t columns = 1;
        bool operator==(const SpecConstantInfo&) const noexcept = default;
    };

    // Vertex input, only valid in the VERTEX stage
    enum class EVertexScalarBase : uint8_t
    {
        BOOL,
        INT,
        UINT,
        FLOAT,
        DOUBLE,
        UNKNOWN
    };

    struct VertexInputAttribute
    {
        uint32_t location = 0;
        std::string name;
        EVertexScalarBase base = EVertexScalarBase::UNKNOWN;
        uint32_t vec_size = 1;
        uint32_t columns = 1;
        uint32_t array_size = 1;
        bool operator==(const VertexInputAttribute&) const noexcept = default;
    };

    struct EntryPointInfo
    {
        std::string name{};
        EShaderType stage{};
        bool operator==(const EntryPointInfo&) const noexcept = default;
    };

    inline const char* to_string(EShaderType s)
    {
        switch (s)
        {
        case EShaderType::VERTEX:
            return "VERTEX";
        case EShaderType::FRAGMENT:
            return "FRAGMENT";
        case EShaderType::GEOMETRY:
            return "GEOMETRY";
        case EShaderType::COMPUTE:
            return "COMPUTE";
        case EShaderType::TESSELLATION_CTRL:
            return "TESSELLATION_CTRL";
        case EShaderType::TESSELLATION_EVAL:
            return "TESSELLATION_EVAL";
        case EShaderType::RAYGEN:
            return "RAYGEN";
        case EShaderType::ANY_HIT:
            return "ANY_HIT";
        case EShaderType::CLOSEST_HIT:
            return "CLOSEST_HIT";
        case EShaderType::MISS:
            return "MISS";
        case EShaderType::INTERSECTION:
            return "INTERSECTION";
        case EShaderType::CALLABLE:
            return "CALLABLE";
        default:
            return "UNDEFINED";
        }
    }

    inline const char* to_string(EDescriptorType t)
    {
        switch (t)
        {
        case EDescriptorType::SAMPLER:
            return "Sampler";
        case EDescriptorType::COMBINED_IMAGE_SAMPLER:
            return "CombinedImageSampler";
        case EDescriptorType::SAMPLED_IMAGE:
            return "SampledImage";
        case EDescriptorType::STORAGE_IMAGE:
            return "StorageImage";
        case EDescriptorType::UNIFORM_TEXEL_BUFFER:
            return "UniformTexelBuffer";
        case EDescriptorType::STORAGE_TEXEL_BUFFER:
            return "StorageTexelBuffer";
        case EDescriptorType::UNIFORM_BUFFER:
            return "UniformBuffer";
        case EDescriptorType::STORAGE_BUFFER:
            return "StorageBuffer";
        case EDescriptorType::INPUT_ATTACHMENT:
            return "InputAttachment";
        case EDescriptorType::ACCELERATION_STRUCTURE:
            return "AccelerationStructure";
        case EDescriptorType::INLINE_UNIFORM_BLOCK:
            return "InlineUniformBlock";
        default:
            return "Unknown";
        }
    }

    struct ShaderInfo
    {
        std::vector<EntryPointInfo> entry_points;
        std::vector<DescriptorSetLayoutInfo> sets;
        std::vector<PushConstantRangeInfo> push_constants;
        std::vector<SpecConstantInfo> spec_constants;
        std::vector<VertexInputAttribute> vertex_inputs;

        /// Runtime-only flag, not serialized. Set when this reflection's descriptor
        /// locations have already been relocated to the merged-domain layout by the
        /// engine's frequency-domain remap table (set on the render side by
        /// relocateShaderInfoByName; always false for offline-generated output).
        ///
        /// Why this needs an explicit flag instead of being inferred structurally:
        /// Instance has offset 0 within the FEATURE domain, so its binding number is
        /// identical whether you read it as canonical or as merged — reflection alone
        /// cannot tell "an unmerged Instance set" apart from "a merged-domain set that
        /// happens to contain only Instance", and the two require different, mutually
        /// incompatible layouts. Pipeline layout routing dispatches on this flag.
        bool merged_domain_layout{false};

        /// Runtime-only data, not serialized. Records the relocation of private/explicit
        /// sets as {set number before patching -> set number after patching}. The actual
        /// allocation strategy only relocates non-engine sets whose slot number is > 3 or
        /// that collide with a domain slot used by this shader (e.g. mesh's visible set5
        /// -> 3, or caster's visible set2 colliding with the FEATURE domain slot -> 3);
        /// everything else is left in place. Consumer: raw-slot bindings recorded at
        /// record time (literal slot numbers such as bindTransientDS(5, ...)) are
        /// translated through this table to the post-relocation slot, so call sites never
        /// need to change their literals — when relocation isn't in effect the table is
        /// empty and behavior is unchanged.
        std::vector<std::pair<uint32_t, uint32_t>> private_set_remap;

        size_t findSet(uint32_t s) const
        {
            for (size_t i = 0; i < sets.size(); ++i)
            {
                if (sets[i].set == s)
                {
                    return i;
                }
            }
            return static_cast<size_t>(-1);
        }

        static constexpr size_t npos = static_cast<size_t>(-1);
        bool operator==(const ShaderInfo&) const noexcept = default;

        static std::vector<std::byte> serialize(const ShaderInfo& info);
        static bool deserialize(std::span<const std::byte> buffer, ShaderInfo& out, std::string* err = nullptr);
    };
} // namespace lux::rdesc
